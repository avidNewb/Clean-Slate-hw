#include "peripherals.h"

#include "sdkconfig.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "peripherals";

static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_periph_task = NULL;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static volatile int s_ambient_ema = -1;
static volatile bool s_fast_click = false;
static volatile uint8_t s_max_clicks = 3;

#ifdef CONFIG_CS_ENABLE_ENCODER
/* Gray-code quadrature transitions matching (old_ab << 2) | new_ab */
static const DRAM_ATTR int8_t k_qdec_lut[16] = {
     0,  1, -1,  0,
    -1,  0,  0,  1,
     1,  0,  0, -1,
     0, -1,  1,  0
};
static volatile uint8_t s_enc_old_ab = 0;
static volatile uint8_t s_enc_rest_ab = 0;
static volatile int8_t s_enc_accum = 0;
static volatile int64_t s_enc_last_edge_us = 0;

static void IRAM_ATTR enc_gpio_isr(void *arg)
{
    (void)arg;
    uint8_t ab = (uint8_t)((gpio_get_level(CS_PIN_ENCODER_A) << 1) |
                           gpio_get_level(CS_PIN_ENCODER_B));
    if (ab == s_enc_old_ab) {
        return;
    }
    /* Reject <2 us ADC1 / Wi-Fi coupling spikes on classic ESP32 GPIOs 36/39 */
    esp_rom_delay_us(2);
    uint8_t ab2 = (uint8_t)((gpio_get_level(CS_PIN_ENCODER_A) << 1) |
                            gpio_get_level(CS_PIN_ENCODER_B));
    if (ab != ab2) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_enc_last_edge_us) >= 250000LL && (ab == 0U || ab == 3U)) {
        /* Clear stale remainder only after an extended idle pause at a resting detent */
        if (ab == s_enc_rest_ab || s_enc_accum == 0 ||
            (s_enc_rest_ab != 0U && s_enc_rest_ab != 3U)) {
            s_enc_accum = 0;
            s_enc_rest_ab = ab;
        }
    }

    int8_t step = k_qdec_lut[(s_enc_old_ab << 2) | ab];
    s_enc_old_ab = ab;
    if (step == 0) {
        return;
    }

    s_enc_last_edge_us = now_us;
    int8_t accum = (int8_t)(s_enc_accum + step);

    /* Recover a skipped edge when completing a full cycle back to the resting detent */
    if (ab == s_enc_rest_ab) {
        if (accum >= 2 && accum < 4) {
            accum = 4;
        } else if (accum <= -2 && accum > -4) {
            accum = -4;
        }
    }

    BaseType_t hpw = pdFALSE;
    while (accum >= 4) {
        accum -= 4;
        if (s_event_queue) {
            cs_input_event_t ev = { .type = CS_INPUT_ENC_CW, .delta = 1 };
            xQueueSendFromISR(s_event_queue, &ev, &hpw);
        }
    }
    while (accum <= -4) {
        accum += 4;
        if (s_event_queue) {
            cs_input_event_t ev = { .type = CS_INPUT_ENC_CCW, .delta = -1 };
            xQueueSendFromISR(s_event_queue, &ev, &hpw);
        }
    }
    s_enc_accum = accum;

    if (hpw) {
        portYIELD_FROM_ISR();
    }
}
#endif

#ifdef CONFIG_CS_ENABLE_BUTTON
static void IRAM_ATTR btn_gpio_isr(void *arg)
{
    (void)arg;
    if (s_periph_task) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_periph_task, &hpw);
        if (hpw) {
            portYIELD_FROM_ISR();
        }
    }
}
#endif

static void configure_input_only_gpio(gpio_num_t pin, gpio_int_type_t intr_type)
{
    // GPIOs 34-39 are input-only on classic ESP32 with no internal pull-ups/downs.
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = intr_type,
    };
    gpio_config(&cfg);
}

static void peripherals_task(void *arg)
{
    (void)arg;
    int btn_stable = 1; // assume active-low external pull-up
    int btn_last_raw = 1;
    int64_t btn_debounce_ts = 0;
    int64_t btn_press_ts = 0;
    int64_t btn_release_ts = 0;
    bool btn_long_fired = false;
    uint8_t btn_click_pending = 0;
    int64_t last_adc_ms = -1000;

#ifdef CONFIG_CS_ENABLE_BUTTON
    btn_stable = gpio_get_level(CS_PIN_BUTTON);
    btn_last_raw = btn_stable;
#endif

    while (1) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        uint32_t wait_ms = 200;

#ifdef CONFIG_CS_ENABLE_BUTTON
        int raw = gpio_get_level(CS_PIN_BUTTON);
        if (raw != btn_last_raw) {
            btn_last_raw = raw;
            btn_debounce_ts = now_ms;
            wait_ms = 5;
        } else if ((now_ms - btn_debounce_ts) >= 25 && raw != btn_stable) {
            btn_stable = raw;
            if (btn_stable == 0) { // pressed (active low)
                btn_press_ts = now_ms;
                btn_long_fired = false;
            } else { // released
                int32_t held_ms = (int32_t)(now_ms - btn_press_ts);
                if (!btn_long_fired && held_ms >= 25) {
                    uint8_t max_clicks = s_fast_click ? 1 : s_max_clicks;
                    if (max_clicks < 1) {
                        max_clicks = 1;
                    } else if (max_clicks > 3) {
                        max_clicks = 3;
                    }
                    uint8_t next_count = (uint8_t)(btn_click_pending + 1);
                    if (next_count >= max_clicks) {
                        btn_click_pending = 0;
                        cs_input_event_type_t type = (next_count >= 3)
                                                         ? CS_INPUT_BTN_TRIPLE
                                                         : ((next_count == 2) ? CS_INPUT_BTN_DOUBLE : CS_INPUT_BTN_SHORT);
                        cs_input_event_t ev = { .type = type, .delta = held_ms };
                        xQueueSend(s_event_queue, &ev, 0);
                    } else {
                        btn_click_pending = next_count;
                        btn_release_ts = now_ms;
                    }
                }
            }
        } else if (raw != btn_stable) {
            wait_ms = 5;
        }

        if (btn_stable == 0 && !btn_long_fired) {
            if ((now_ms - btn_press_ts) >= 900) {
                btn_long_fired = true;
                btn_click_pending = 0;
                cs_input_event_t ev = { .type = CS_INPUT_BTN_LONG, .delta = (int32_t)(now_ms - btn_press_ts) };
                xQueueSend(s_event_queue, &ev, 0);
            } else if (wait_ms > 20) {
                wait_ms = 20;
            }
        }
        if (btn_click_pending > 0 && btn_stable == 1) {
            if ((now_ms - btn_release_ts) >= 350) {
                uint8_t count = btn_click_pending;
                btn_click_pending = 0;
                cs_input_event_type_t type = (count >= 2) ? CS_INPUT_BTN_DOUBLE : CS_INPUT_BTN_SHORT;
                cs_input_event_t ev = { .type = type, .delta = count };
                xQueueSend(s_event_queue, &ev, 0);
            } else if (wait_ms > 20) {
                wait_ms = 20;
            }
        }
#endif

#ifdef CONFIG_CS_ENABLE_LIGHT_SENSOR
        if ((now_ms - last_adc_ms) >= 200) {
            last_adc_ms = now_ms;
            int raw_adc = 0;
            if (s_adc_handle && adc_oneshot_read(s_adc_handle, CS_LIGHT_ADC_CHANNEL, &raw_adc) == ESP_OK) {
                if (s_ambient_ema < 0) {
                    s_ambient_ema = raw_adc;
                } else {
                    s_ambient_ema = (s_ambient_ema * 7 + raw_adc) / 8;
                }
            }
        }
#endif

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t peripherals_init(QueueHandle_t *out_event_queue)
{
    s_event_queue = xQueueCreate(32, sizeof(cs_input_event_t));
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "event queue alloc failed");

#if defined(CONFIG_CS_ENABLE_BUTTON) || defined(CONFIG_CS_ENABLE_ENCODER)
    esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        return isr_err;
    }
#endif

#ifdef CONFIG_CS_ENABLE_BUTTON
    configure_input_only_gpio(CS_PIN_BUTTON, GPIO_INTR_ANYEDGE);
#endif

#ifdef CONFIG_CS_ENABLE_ENCODER
    configure_input_only_gpio(CS_PIN_ENCODER_A, GPIO_INTR_ANYEDGE);
    configure_input_only_gpio(CS_PIN_ENCODER_B, GPIO_INTR_ANYEDGE);
    s_enc_old_ab = (uint8_t)((gpio_get_level(CS_PIN_ENCODER_A) << 1) |
                             gpio_get_level(CS_PIN_ENCODER_B));
    s_enc_rest_ab = s_enc_old_ab;
    s_enc_accum = 0;
    s_enc_last_edge_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(CS_PIN_ENCODER_A, enc_gpio_isr, NULL),
                        TAG, "encoder A ISR add failed");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(CS_PIN_ENCODER_B, enc_gpio_isr, NULL),
                        TAG, "encoder B ISR add failed");
    ESP_LOGI(TAG, "Rotary encoder GPIO interrupt 4x enabled on GPIO %d / %d",
             CS_PIN_ENCODER_A, CS_PIN_ENCODER_B);
#endif

#ifdef CONFIG_CS_ENABLE_LIGHT_SENSOR
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = CS_LIGHT_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle), TAG, "adc unit init failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, CS_LIGHT_ADC_CHANNEL, &chan_cfg),
                        TAG, "adc channel config failed");
    ESP_LOGI(TAG, "Light sensor ADC enabled on GPIO %d (ADC1_CH%d)", CS_PIN_LIGHT_ADC, CS_LIGHT_ADC_CHANNEL);
#endif

    BaseType_t ok = xTaskCreate(peripherals_task, "periph_task", 3072, NULL, 7, &s_periph_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");

#ifdef CONFIG_CS_ENABLE_BUTTON
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(CS_PIN_BUTTON, btn_gpio_isr, NULL),
                        TAG, "button ISR add failed");
    ESP_LOGI(TAG, "Button GPIO interrupt enabled on GPIO %d (external pull-up required)", CS_PIN_BUTTON);
#endif

    if (out_event_queue) {
        *out_event_queue = s_event_queue;
    }
    return ESP_OK;
}

int peripherals_get_ambient_raw(void)
{
    return s_ambient_ema;
}

void peripherals_set_fast_click(bool fast)
{
    s_fast_click = fast;
}

void peripherals_set_max_clicks(uint8_t max_clicks)
{
    if (max_clicks < 1) {
        max_clicks = 1;
    } else if (max_clicks > 3) {
        max_clicks = 3;
    }
    s_max_clicks = max_clicks;
}

uint8_t peripherals_ambient_to_brightness_range(int raw_adc, uint8_t min_b, uint8_t max_b)
{
    int lo = (min_b < 1U) ? 1 : (int)min_b;
    int hi = ((int)max_b < lo) ? lo : (int)max_b;
    if (raw_adc < 0) {
        int def_b = CONFIG_CS_HUB75_DEFAULT_BRIGHTNESS;
        if (def_b < lo) def_b = lo;
        if (def_b > hi) def_b = hi;
        return (uint8_t)def_b;
    }
    if (raw_adc > 4095) {
        raw_adc = 4095;
    }
    /* Linear 0..4095 -> min..max (matches legacy lightsensor map without crushing indoor ADC readings) */
    float t = (float)raw_adc / 4095.0f;
    int b = lo + (int)(t * (float)(hi - lo) + 0.5f);
    if (b < lo) b = lo;
    if (b > hi) b = hi;
    return (uint8_t)b;
}

uint8_t peripherals_ambient_to_brightness(int raw_adc)
{
    return peripherals_ambient_to_brightness_range(
        raw_adc,
        (uint8_t)CONFIG_CS_HUB75_MIN_BRIGHTNESS,
        (uint8_t)CONFIG_CS_HUB75_MAX_BRIGHTNESS);
}
