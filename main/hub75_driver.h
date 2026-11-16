#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HUB75_STATUS_NONE = 0,
    HUB75_STATUS_CONNECTING, // Subtle blue corner dot
    HUB75_STATUS_RETRYING,   // Subtle amber corner dot
    HUB75_STATUS_ERROR,      // Subtle red corner dot
} hub75_status_indicator_t;

/**
 * Initialize I2S0 16-bit parallel DMA HUB75 driver for the 64x32 matrix.
 * Starts continuous hardware DMA refresh immediately with a cleared frame.
 */
esp_err_t hub75_init(void);

/**
 * Present a 64x32 RGB888 frame (6144 bytes, row-major top-to-bottom, R,G,B).
 */
esp_err_t hub75_present_rgb888(const uint8_t *rgb888);

/**
 * Copy the currently displayed 64x32 RGB888 frame (6144 bytes) into out_rgb888.
 */
esp_err_t hub75_get_frame_rgb888(uint8_t *out_rgb888);

/**
 * Present a 64x32 RGB565 frame (4096 uint16_t pixels, little-endian).
 */
esp_err_t hub75_present_rgb565(const uint16_t *rgb565);

/**
 * Clear the display to black.
 */
esp_err_t hub75_clear(void);

/**
 * Set global panel brightness (0 = blanked, 255 = full brightness).
 * Re-renders the active frame at the new brightness level without tearing.
 */
esp_err_t hub75_set_brightness(uint8_t brightness);

/**
 * Get current global panel brightness (0..255).
 */
uint8_t hub75_get_brightness(void);

/**
 * Set or clear the subtle 2x2 bottom-right status indicator overlaid on the
 * current frame during transient/persistent connection failures.
 */
esp_err_t hub75_set_status_indicator(hub75_status_indicator_t indicator);

/**
 * Draw a simple 2-line status screen (used before the first API frame arrives).
 */
esp_err_t hub75_show_status_screen(const char *line1, const char *line2,
                                   uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
