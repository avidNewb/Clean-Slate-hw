#include "hub75_driver.h"

#include <math.h>
#include <string.h>
#include "sdkconfig.h"
#include "board_pins.h"
#include "gfx_font.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "soc/lldesc.h"
#include "hal/i2s_hal.h"
#include "hal/i2s_ll.h"
#include "hal/i2s_types.h"
#include "hal/lcd_periph.h"
#include "hal/lcd_types.h"
#include "esp_private/i2s_platform.h"

static const char *TAG = "hub75";

// ---------------------------------------------------------------------------
// 16-bit DMA word bit assignments (mapped to I2S0O_DATA_OUT8_IDX..23_IDX)
// ---------------------------------------------------------------------------
#define BIT_R1   (1U << 0)
#define BIT_G1   (1U << 1)
#define BIT_B1   (1U << 2)
#define BIT_R2   (1U << 3)
#define BIT_G2   (1U << 4)
#define BIT_B2   (1U << 5)
#define BIT_A    (1U << 6)
#define BIT_B    (1U << 7)
#define BIT_C    (1U << 8)
#define BIT_D    (1U << 9)
#define BIT_E    (1U << 10)
#define BIT_LAT  (1U << 11)
#define BIT_OE   (1U << 12) // Active LOW: 0 = LEDs on, 1 = blanked

// 8-bit pipelined BCM timing per row (LSB weight = 2 clocks: 2..256 = 510 display clocks):
// Each step displays the previously latched bit-plane while shifting the next bit-plane
// (61 active-capable shift slots + 3 blanked pre-latch clocks, with LAT on col 63 only)
// plus 1 blanked post-latch settle clock and any remaining hold clocks for bits 5..7.
// Total per row = 8 * (1 + 64) + (3 + 67 + 195) = 785 words (16 rows = 12560 words = 25120 bytes).
#define BCM_BIT_DEPTH          8
#define SETTLE_WORDS           1U
#define SHIFT_ACTIVE_SLOTS     (CS_HUB75_WIDTH - 3U) // 61
#define TOTAL_HOLD_WORDS       (3U + 67U + 195U)     // 265
#define WORDS_PER_ROW          ((BCM_BIT_DEPTH * (SETTLE_WORDS + CS_HUB75_WIDTH)) + TOTAL_HOLD_WORDS)
#define FRAME_TOTAL_WORDS      (CS_HUB75_SCAN_ROWS * WORDS_PER_ROW)
#define FRAME_TOTAL_BYTES      (FRAME_TOTAL_WORDS * sizeof(uint16_t))

#define I2S_BUS_ID             0
#define I2S_PERIPH_CLK_PRESCALE 2 // 160 MHz / 2 = 80 MHz

static const int s_data_gpios[13] = {
    CS_PIN_HUB75_R1,
    CS_PIN_HUB75_G1,
    CS_PIN_HUB75_B1,
    CS_PIN_HUB75_R2,
    CS_PIN_HUB75_G2,
    CS_PIN_HUB75_B2,
    CS_PIN_HUB75_A,
    CS_PIN_HUB75_B,
    CS_PIN_HUB75_C,
    CS_PIN_HUB75_D,
    CS_PIN_HUB75_E,
    CS_PIN_HUB75_LAT,
    CS_PIN_HUB75_OE,
};

// CIE 1931-like gamma 2.2 LUT (0..255 -> 0..255)
static uint8_t s_gamma_lut[256];

static i2s_hal_context_t s_i2s_hal;
static intr_handle_t s_i2s_intr = NULL;
static volatile uint32_t s_frame_eof_count = 0;

static uint16_t *s_dma_buf[2] = {NULL, NULL};
static lldesc_t *s_dma_ring[2] = {NULL, NULL};
static int s_desc_count = 0;
static int s_front_idx = 0;

static uint8_t s_shadow_fb[CS_HUB75_WIDTH * CS_HUB75_HEIGHT * 3];
/* Scratch buffer for apply_status_dot + DMA encoding; kept off task stacks (6 KiB).
 * Guarded by s_lock during runtime swaps and single-threaded during hub75_init. */
static uint8_t s_work_fb[CS_HUB75_WIDTH * CS_HUB75_HEIGHT * 3];
static uint8_t s_brightness = CONFIG_CS_HUB75_DEFAULT_BRIGHTNESS;
static hub75_status_indicator_t s_status_indicator = HUB75_STATUS_NONE;
static SemaphoreHandle_t s_lock = NULL;
static bool s_initialized = false;

static void init_gamma_lut(void)
{
    for (int i = 0; i < 256; i++) {
        // Gamma 1.5 preserves LED contrast without crushing midtones at low indoor brightness
        float norm = (float)i / 255.0f;
        float corrected = powf(norm, 1.5f);
        int val = (int)(corrected * 255.0f + 0.5f);
        if (i > 0 && val == 0) {
            val = 1; // preserve dimmest non-zero pixels
        }
        s_gamma_lut[i] = (uint8_t)(val > 255 ? 255 : val);
    }
}

// Classic ESP32 I2S 16-bit parallel mode transmits halfwords in swapped pairs (idx ^ 1)
static inline void dma_write_word(uint16_t *buf, size_t word_idx, uint16_t val)
{
    buf[word_idx ^ 1U] = val;
}

static uint16_t row_addr_bits(int row)
{
    uint16_t bits = 0;
    if (row & 0x01) bits |= BIT_A;
    if (row & 0x02) bits |= BIT_B;
    if (row & 0x04) bits |= BIT_C;
    if (row & 0x08) bits |= BIT_D;
    if (row & 0x10) bits |= BIT_E;
    return bits;
}

static void apply_status_dot(uint8_t *fb)
{
    uint8_t r = 0, g = 0, b = 0;
    switch (s_status_indicator) {
        case HUB75_STATUS_CONNECTING: r = 0;   g = 80;  b = 255; break;
        case HUB75_STATUS_RETRYING:   r = 255; g = 140; b = 0;   break;
        case HUB75_STATUS_ERROR:      r = 255; g = 0;   b = 0;   break;
        case HUB75_STATUS_NONE:
        default:
            return;
    }
    gfx_fill_rect(fb, CS_HUB75_WIDTH, CS_HUB75_HEIGHT,
                  CS_HUB75_WIDTH - 2, CS_HUB75_HEIGHT - 2, 2, 2, r, g, b);
}

static void build_dma_buffer(uint16_t *dma_buf, const uint8_t *rgb888, uint8_t brightness)
{
    static const uint16_t k_bcm_weights[BCM_BIT_DEPTH] = { 2, 4, 8, 16, 32, 64, 128, 256 };
    static const uint16_t k_bcm_hold_words[BCM_BIT_DEPTH] = { 0, 0, 0, 0, 0, 3, 67, 195 };
    const int total_steps = CS_HUB75_SCAN_ROWS * BCM_BIT_DEPTH;

    memcpy(s_work_fb, rgb888, sizeof(s_work_fb));
    apply_status_dot(s_work_fb);

    size_t idx = 0;

    for (int step = 0; step < total_steps; step++) {
        const int row = step / BCM_BIT_DEPTH;
        const int b = step % BCM_BIT_DEPTH;
        const int prev_step = (step + total_steps - 1) % total_steps;
        const int prev_row = prev_step / BCM_BIT_DEPTH;
        const int prev_b = prev_step % BCM_BIT_DEPTH;

        const uint16_t prev_addr = row_addr_bits(prev_row);
        const uint32_t total_ticks = k_bcm_weights[prev_b];
        uint32_t active_ticks = 0;
        if (brightness > 0) {
            active_ticks = (total_ticks * (uint32_t)brightness + 127U) / 255U;
            if (active_ticks > total_ticks) {
                active_ticks = total_ticks;
            }
        }
        const uint32_t hold = k_bcm_hold_words[prev_b];

        // 1. Post-latch settle clock (LAT=0, OE=1): switch row address cleanly while blanked
        dma_write_word(dma_buf, idx++, prev_addr | BIT_OE);

        // 2. Remaining hold clocks for bits 5..7 before overlapping into the next shift phase
        for (uint32_t h = 0; h < hold; h++) {
            const uint16_t oe = (h < active_ticks) ? 0U : BIT_OE;
            dma_write_word(dma_buf, idx++, prev_addr | oe);
        }

        // 3. Shift next bit-plane (row, b) while displaying (prev_row, prev_b) across cols 0..60.
        // Cols 61..63 stay blanked (OE=1), and BIT_LAT pulses ONLY on col 63 so no trailing zeros
        // are clocked through a transparent latch (prevents 2-column left shift).
        const uint8_t bit_mask = (uint8_t)(1U << b);
        const uint8_t *upper_row = &s_work_fb[(size_t)row * CS_HUB75_WIDTH * 3];
        const uint8_t *lower_row = &s_work_fb[((size_t)row + CS_HUB75_SCAN_ROWS) * CS_HUB75_WIDTH * 3];

        for (int col = 0; col < CS_HUB75_WIDTH; col++) {
            const size_t px = (size_t)col * 3;
            const uint8_t r1 = s_gamma_lut[upper_row[px + 0]];
            const uint8_t g1 = s_gamma_lut[upper_row[px + 1]];
            const uint8_t b1 = s_gamma_lut[upper_row[px + 2]];
            const uint8_t r2 = s_gamma_lut[lower_row[px + 0]];
            const uint8_t g2 = s_gamma_lut[lower_row[px + 1]];
            const uint8_t b2 = s_gamma_lut[lower_row[px + 2]];

            uint16_t word = prev_addr;
            if (col < (int)SHIFT_ACTIVE_SLOTS) {
                const uint32_t slot = hold + (uint32_t)col;
                if (slot >= active_ticks) {
                    word |= BIT_OE;
                }
            } else {
                word |= BIT_OE;
            }
            if (col == CS_HUB75_WIDTH - 1) {
                word |= BIT_LAT;
            }
            if (r1 & bit_mask) word |= BIT_R1;
            if (g1 & bit_mask) word |= BIT_G1;
            if (b1 & bit_mask) word |= BIT_B1;
            if (r2 & bit_mask) word |= BIT_R2;
            if (g2 & bit_mask) word |= BIT_G2;
            if (b2 & bit_mask) word |= BIT_B2;

            dma_write_word(dma_buf, idx++, word);
        }
    }
}

static IRAM_ATTR void i2s_eof_isr(void *arg)
{
    i2s_dev_t *dev = s_i2s_hal.dev;
    uint32_t status = i2s_ll_get_intr_status(dev);
    if (status & I2S_LL_EVENT_TX_EOF) {
        i2s_ll_clear_intr_status(dev, I2S_LL_EVENT_TX_EOF);
        s_frame_eof_count = s_frame_eof_count + 1;
    }
}

static void swap_dma_buffers_locked(void)
{
    const int back_idx = 1 - s_front_idx;

    build_dma_buffer(s_dma_buf[back_idx], s_shadow_fb, s_brightness);

    // Ensure back ring loops onto itself once entered
    s_dma_ring[back_idx][s_desc_count - 1].qe.stqe_next = &s_dma_ring[back_idx][0];

    // Atomically splice front ring's tail into back ring's head at frame boundary
    s_dma_ring[s_front_idx][s_desc_count - 1].qe.stqe_next = &s_dma_ring[back_idx][0];

    // Wait for 2 frame EOFs (~3-4 ms) so DMA is guaranteed to be running inside back ring
    uint32_t start = s_frame_eof_count;
    int64_t deadline_us = esp_timer_get_time() + 50000; // 50 ms safety timeout
    while ((int32_t)(s_frame_eof_count - start) < 2) {
        if (esp_timer_get_time() > deadline_us) {
            break;
        }
        vTaskDelay(1);
    }

    s_front_idx = back_idx;
}

static esp_err_t configure_hub75_gpios(void)
{
    // Configure all 13 data/address/control pins + CLK as outputs via GPIO matrix
    for (size_t i = 0; i < sizeof(s_data_gpios) / sizeof(s_data_gpios[0]); i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << s_data_gpios[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config failed for pin %d", s_data_gpios[i]);
        // 16-bit I2S parallel LCD mode uses data_sigs[i + 8] (I2S0O_DATA_OUT8_IDX..23_IDX)
        esp_rom_gpio_connect_out_signal(s_data_gpios[i],
                                        soc_lcd_i2s_signals[I2S_BUS_ID].data_sigs[i + 8],
                                        false, false);
    }

    // Drive unused high bits (bits 13..15) unused — nothing connected
    gpio_config_t clk_cfg = {
        .pin_bit_mask = (1ULL << CS_PIN_HUB75_CLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&clk_cfg), TAG, "gpio_config failed for CLK");

    bool invert_clk = false;
#ifdef CONFIG_CS_HUB75_CLK_PHASE_INV
    invert_clk = true;
#endif
    esp_rom_gpio_connect_out_signal(CS_PIN_HUB75_CLK,
                                    soc_lcd_i2s_signals[I2S_BUS_ID].wr_sig,
                                    invert_clk, false);
    return ESP_OK;
}

esp_err_t hub75_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    init_gamma_lut();
    memset(s_shadow_fb, 0, sizeof(s_shadow_fb));

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    s_desc_count = lldesc_get_required_num(FRAME_TOTAL_BYTES);
    ESP_LOGI(TAG, "Allocating double DMA buffers: 2 x %u bytes (%d descriptors each)",
             (unsigned)FRAME_TOTAL_BYTES, s_desc_count);

    for (int i = 0; i < 2; i++) {
        s_dma_buf[i] = heap_caps_aligned_calloc(4, 1, FRAME_TOTAL_BYTES,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_dma_buf[i] != NULL, ESP_ERR_NO_MEM, TAG, "DMA buffer %d alloc failed", i);

        s_dma_ring[i] = heap_caps_calloc((size_t)s_desc_count, sizeof(lldesc_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_dma_ring[i] != NULL, ESP_ERR_NO_MEM, TAG, "DMA ring %d alloc failed", i);

        lldesc_setup_link(s_dma_ring[i], s_dma_buf[i], FRAME_TOTAL_BYTES, false);
        s_dma_ring[i][s_desc_count - 1].qe.stqe_next = &s_dma_ring[i][0];
    }

    build_dma_buffer(s_dma_buf[0], s_shadow_fb, s_brightness);
    build_dma_buffer(s_dma_buf[1], s_shadow_fb, s_brightness);

    ESP_RETURN_ON_ERROR(i2s_platform_acquire_occupation(I2S_CTLR_HP, I2S_BUS_ID, "hub75_i2s"),
                        TAG, "I2S0 is occupied");

    i2s_hal_init(&s_i2s_hal, I2S_BUS_ID);

    uint32_t src_clk_hz = 0;
    ESP_RETURN_ON_ERROR(esp_clk_tree_src_get_freq_hz((soc_module_clk_t)LCD_CLK_SRC_DEFAULT,
                                                       ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                                       &src_clk_hz),
                        TAG, "clk tree query failed");

    i2s_ll_tx_clk_set_src(s_i2s_hal.dev, (i2s_clock_src_t)LCD_CLK_SRC_DEFAULT);
    i2s_ll_set_raw_mclk_div(s_i2s_hal.dev, I2S_PERIPH_CLK_PRESCALE, 1, 0);
    uint32_t res_hz = src_clk_hz / I2S_PERIPH_CLK_PRESCALE; // 80 MHz
    uint32_t pclk_hz = (uint32_t)CONFIG_CS_HUB75_PCLK_MHZ * 1000000U;
    uint32_t bck_div = res_hz / 2U / pclk_hz;
    if (bck_div < 2) bck_div = 2;
    if (bck_div > I2S_LL_BCK_MAX_PRESCALE) bck_div = I2S_LL_BCK_MAX_PRESCALE;

    i2s_ll_tx_reset(s_i2s_hal.dev);
    i2s_ll_tx_reset_dma(s_i2s_hal.dev);
    i2s_ll_tx_reset_fifo(s_i2s_hal.dev);

    i2s_ll_enable_lcd(s_i2s_hal.dev, true);
    i2s_ll_tx_stop_on_fifo_empty(s_i2s_hal.dev, false); // Continuous circular DMA
    i2s_ll_tx_set_slave_mod(s_i2s_hal.dev, false);
    i2s_ll_tx_set_bits_mod(s_i2s_hal.dev, 16);
    i2s_ll_tx_select_std_slot(s_i2s_hal.dev, I2S_STD_SLOT_BOTH, true);
    i2s_ll_tx_enable_right_first(s_i2s_hal.dev, true);
#if SOC_MODULE_SUPPORT(I2S, DMA_EQUAL)
    i2s_ll_tx_enable_dma_equal(s_i2s_hal.dev, true);
#endif
    i2s_ll_tx_set_bck_div_num(s_i2s_hal.dev, bck_div);

    i2s_ll_dma_enable_eof_on_fifo_empty(s_i2s_hal.dev, true);
    i2s_ll_dma_enable_owner_check(s_i2s_hal.dev, false);
    i2s_ll_dma_enable_auto_write_back(s_i2s_hal.dev, false);
    i2s_ll_enable_dma(s_i2s_hal.dev, true);

    ESP_RETURN_ON_ERROR(
        esp_intr_alloc_intrstatus(soc_lcd_i2s_signals[I2S_BUS_ID].irq_id,
                                  ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_LOWMED,
                                  (uint32_t)i2s_ll_get_intr_status_reg(s_i2s_hal.dev),
                                  I2S_LL_EVENT_TX_EOF,
                                  i2s_eof_isr,
                                  NULL,
                                  &s_i2s_intr),
        TAG, "I2S EOF interrupt alloc failed");

    i2s_ll_clear_intr_status(s_i2s_hal.dev, I2S_LL_EVENT_TX_EOF);
    i2s_ll_enable_intr(s_i2s_hal.dev, I2S_LL_EVENT_TX_EOF, true);

    ESP_RETURN_ON_ERROR(configure_hub75_gpios(), TAG, "HUB75 GPIO config failed");

    s_front_idx = 0;
    i2s_ll_set_out_link_addr(s_i2s_hal.dev, (uint32_t)&s_dma_ring[s_front_idx][0]);
    i2s_ll_start_out_link(s_i2s_hal.dev);
    i2s_ll_tx_start(s_i2s_hal.dev);

    s_initialized = true;
    uint32_t actual_pclk = res_hz / 2U / bck_div;
    uint32_t refresh_hz = actual_pclk / FRAME_TOTAL_WORDS;
    ESP_LOGI(TAG, "HUB75 DMA started: PCLK=%" PRIu32 " Hz (~%" PRIu32 " Hz refresh)",
             actual_pclk, refresh_hz);
    return ESP_OK;
}

esp_err_t hub75_present_rgb888(const uint8_t *rgb888)
{
    ESP_RETURN_ON_FALSE(s_initialized && rgb888, ESP_ERR_INVALID_STATE, TAG, "not ready");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_shadow_fb, rgb888, sizeof(s_shadow_fb));
    swap_dma_buffers_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t hub75_get_frame_rgb888(uint8_t *out_rgb888)
{
    ESP_RETURN_ON_FALSE(out_rgb888 != NULL, ESP_ERR_INVALID_ARG, TAG, "null out");
    if (!s_initialized || !s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_rgb888, s_shadow_fb, sizeof(s_shadow_fb));
    apply_status_dot(out_rgb888);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t hub75_present_rgb565(const uint16_t *rgb565)
{
    ESP_RETURN_ON_FALSE(s_initialized && rgb565, ESP_ERR_INVALID_STATE, TAG, "not ready");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < CS_HUB75_WIDTH * CS_HUB75_HEIGHT; i++) {
        uint16_t px = rgb565[i];
        uint8_t r5 = (uint8_t)((px >> 11) & 0x1F);
        uint8_t g6 = (uint8_t)((px >> 5) & 0x3F);
        uint8_t b5 = (uint8_t)(px & 0x1F);
        s_shadow_fb[i * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        s_shadow_fb[i * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        s_shadow_fb[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
    swap_dma_buffers_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t hub75_clear(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not ready");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_shadow_fb, 0, sizeof(s_shadow_fb));
    swap_dma_buffers_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t hub75_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        s_brightness = brightness;
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_brightness != brightness) {
        s_brightness = brightness;
        swap_dma_buffers_locked();
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint8_t hub75_get_brightness(void)
{
    return s_brightness;
}

esp_err_t hub75_set_status_indicator(hub75_status_indicator_t indicator)
{
    if (!s_initialized) {
        s_status_indicator = indicator;
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status_indicator != indicator) {
        s_status_indicator = indicator;
        swap_dma_buffers_locked();
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t hub75_show_status_screen(const char *line1, const char *line2,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not ready");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_shadow_fb, 0, sizeof(s_shadow_fb));
    if (line1) {
        gfx_draw_text(s_shadow_fb, CS_HUB75_WIDTH, CS_HUB75_HEIGHT, 2, 6, line1, r, g, b);
    }
    if (line2) {
        gfx_draw_text(s_shadow_fb, CS_HUB75_WIDTH, CS_HUB75_HEIGHT, 2, 18, line2, 180, 180, 180);
    }
    swap_dma_buffers_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
