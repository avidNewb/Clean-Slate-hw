#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CS_INPUT_BTN_SHORT = 1,
    CS_INPUT_BTN_DOUBLE,
    CS_INPUT_BTN_TRIPLE,
    CS_INPUT_BTN_LONG,
    CS_INPUT_ENC_CW,
    CS_INPUT_ENC_CCW,
} cs_input_event_type_t;

typedef struct {
    cs_input_event_type_t type;
    int32_t delta; // +1 / -1 for encoder steps, hold ms for button
} cs_input_event_t;

/**
 * Initialize push button (GPIO 35), rotary encoder (GPIO 39/36),
 * and ambient light sensor ADC (GPIO 34).
 *
 * Returns a FreeRTOS queue emitting cs_input_event_t items.
 */
esp_err_t peripherals_init(QueueHandle_t *out_event_queue);

/**
 * Enable/disable immediate single-click dispatch (skips the 350 ms multi-click
 * wait window when true, making alert dismissal and menu clicks instantaneous).
 */
void peripherals_set_fast_click(bool fast);

/**
 * Configure the maximum multi-click count (2 in menu editors, 3 when idle).
 */
void peripherals_set_max_clicks(uint8_t max_clicks);

/**
 * Get the latest smoothed ambient light ADC reading (0..4095).
 * Returns -1 if light sensor is disabled or unavailable.
 */
int peripherals_get_ambient_raw(void);

/**
 * Convert ambient raw ADC value to a recommended display brightness within [min_b..max_b].
 */
uint8_t peripherals_ambient_to_brightness_range(int raw_adc, uint8_t min_b, uint8_t max_b);

/**
 * Convert ambient raw ADC value using Kconfig default min..max range.
 */
uint8_t peripherals_ambient_to_brightness(int raw_adc);

#ifdef __cplusplus
}
#endif
