#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize UART1 for DFPlayer Mini on RX=GPIO13, TX=GPIO0 @ 9600 8N1.
 * Safe no-op if CONFIG_CS_ENABLE_DFPLAYER is disabled.
 */
esp_err_t dfplayer_init(void);

/**
 * Set DFPlayer volume (0..30, 0 = mute).
 */
esp_err_t dfplayer_set_volume(uint8_t volume);

/**
 * Set DFPlayer equalizer preset (0=Normal, 1=Pop, 2=Rock, 3=Jazz, 4=Classic, 5=Bass).
 */
esp_err_t dfplayer_set_eq(uint8_t eq);

/**
 * Play track by global index (1..2999).
 */
esp_err_t dfplayer_play_track(uint16_t track);

/**
 * Play track inside a numbered folder (/01/001.mp3 .. /99/255.mp3).
 */
esp_err_t dfplayer_play_folder_track(uint8_t folder, uint8_t track);

/**
 * Stop current playback.
 */
esp_err_t dfplayer_stop(void);

#ifdef __cplusplus
}
#endif
