#include "dfplayer.h"

#include "sdkconfig.h"
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "dfplayer";
static bool s_ready = false;
static SemaphoreHandle_t s_uart_mutex = NULL;
static int64_t s_last_tx_us = 0;

#define DFPLAYER_MIN_CMD_GAP_US (40000LL)

static esp_err_t send_cmd(uint8_t cmd, uint16_t param)
{
    if (!s_ready || !s_uart_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t pkt[10] = {
        0x7E, 0xFF, 0x06, cmd, 0x00,
        (uint8_t)(param >> 8), (uint8_t)(param & 0xFF),
        0x00, 0x00, 0xEF
    };

    uint16_t sum = 0;
    for (int i = 1; i <= 6; i++) {
        sum += pkt[i];
    }
    uint16_t csum = (uint16_t)(0U - sum);
    pkt[7] = (uint8_t)(csum >> 8);
    pkt[8] = (uint8_t)(csum & 0xFF);

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_last_tx_us;
    if (s_last_tx_us > 0 && elapsed_us < DFPLAYER_MIN_CMD_GAP_US) {
        uint32_t wait_ms = (uint32_t)((DFPLAYER_MIN_CMD_GAP_US - elapsed_us + 999LL) / 1000LL);
        if (wait_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    int written = uart_write_bytes(CS_DFPLAYER_UART_NUM, pkt, sizeof(pkt));
    s_last_tx_us = esp_timer_get_time();
    xSemaphoreGive(s_uart_mutex);
    return (written == sizeof(pkt)) ? ESP_OK : ESP_FAIL;
}

esp_err_t dfplayer_init(void)
{
#ifdef CONFIG_CS_ENABLE_DFPLAYER
    if (s_ready) {
        return ESP_OK;
    }

    const uart_config_t uart_cfg = {
        .baud_rate = CS_DFPLAYER_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(CS_DFPLAYER_UART_NUM, 256, 0, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(CS_DFPLAYER_UART_NUM, &uart_cfg),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(CS_DFPLAYER_UART_NUM,
                                     CS_PIN_DFPLAYER_TX,
                                     CS_PIN_DFPLAYER_RX,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

    if (!s_uart_mutex) {
        s_uart_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_uart_mutex != NULL, ESP_ERR_NO_MEM, TAG, "uart mutex alloc failed");
    }

    s_ready = true;
    ESP_LOGI(TAG, "DFPlayer UART ready (UART%d, TX=GPIO%d, RX=GPIO%d)",
             CS_DFPLAYER_UART_NUM, CS_PIN_DFPLAYER_TX, CS_PIN_DFPLAYER_RX);
#else
    ESP_LOGI(TAG, "DFPlayer disabled via Kconfig");
#endif
    return ESP_OK;
}

esp_err_t dfplayer_set_volume(uint8_t volume)
{
    if (volume > 30) {
        volume = 30;
    }
    return send_cmd(0x06, volume);
}

esp_err_t dfplayer_set_eq(uint8_t eq)
{
    if (eq > 5) {
        eq = 0;
    }
    return send_cmd(0x07, eq);
}

esp_err_t dfplayer_play_track(uint16_t track)
{
    if (track == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_cmd(0x03, track);
}

esp_err_t dfplayer_play_folder_track(uint8_t folder, uint8_t track)
{
    if (folder == 0 || track == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_cmd(0x0F, ((uint16_t)folder << 8) | track);
}

esp_err_t dfplayer_stop(void)
{
    return send_cmd(0x16, 0);
}

static uint8_t s_rx_pkt[10];
static size_t s_rx_pos = 0;

void dfplayer_flush_rx(void)
{
    if (!s_ready || !s_uart_mutex) {
        return;
    }
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_rx_pos = 0;
        uart_flush_input(CS_DFPLAYER_UART_NUM);
        xSemaphoreGive(s_uart_mutex);
    }
}

bool dfplayer_poll_track_finished(void)
{
    if (!s_ready || !s_uart_mutex) {
        return false;
    }

    uint8_t buf[32];
    bool finished = false;

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    int n = uart_read_bytes(CS_DFPLAYER_UART_NUM, buf, sizeof(buf), 0);
    for (int i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (s_rx_pos == 0) {
            if (b == 0x7E) {
                s_rx_pkt[s_rx_pos++] = b;
            }
        } else {
            s_rx_pkt[s_rx_pos++] = b;
            if (s_rx_pos == 2 && b != 0xFF) {
                s_rx_pos = (b == 0x7E) ? 1 : 0;
            } else if (s_rx_pos == 3 && b != 0x06) {
                s_rx_pos = (b == 0x7E) ? 1 : 0;
            } else if (s_rx_pos == sizeof(s_rx_pkt)) {
                if (s_rx_pkt[9] == 0xEF) {
                    uint8_t cmd = s_rx_pkt[3];
                    if (cmd == 0x3C || cmd == 0x3D || cmd == 0x3E) {
                        finished = true;
                    }
                }
                s_rx_pos = 0;
            }
        }
    }
    xSemaphoreGive(s_uart_mutex);
    return finished;
}
