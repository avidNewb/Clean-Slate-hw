#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"

/**
 * Hardware Pinout — Clean Slate 64x32 HUB75 Display
 *
 * Preserves the exact legacy ESP32_FORUM_PINOUT + devicePinSetup.h wiring.
 *
 * IMPORTANT HARDWARE NOTES:
 * 1. GPIO 2 is dedicated exclusively to HUB75 R1. Do NOT drive it as an onboard LED.
 * 2. GPIOs 34, 35, 36, 39 are input-only on classic ESP32 and have NO internal
 *    pull-up or pull-down resistors. External biasing resistors are required.
 * 3. GPIOs 0, 2, 5, 12, 15 are ESP32 strapping pins. Keep HUB75 / DFPlayer
 *    external circuitry from pulling GPIO 0 low or GPIO 12 high during boot.
 */

// ---------------------------------------------------------------------------
// 64x32 HUB75 RGB LED Matrix (1/16 scan, ESP32_FORUM_PINOUT)
// ---------------------------------------------------------------------------
#define CS_HUB75_WIDTH          64
#define CS_HUB75_HEIGHT         32
#define CS_HUB75_SCAN_ROWS      16

#define CS_PIN_HUB75_R1         GPIO_NUM_2
#define CS_PIN_HUB75_G1         GPIO_NUM_15
#define CS_PIN_HUB75_B1         GPIO_NUM_4
#define CS_PIN_HUB75_R2         GPIO_NUM_14
#define CS_PIN_HUB75_G2         GPIO_NUM_27
#define CS_PIN_HUB75_B2         GPIO_NUM_23

#define CS_PIN_HUB75_A          GPIO_NUM_5
#define CS_PIN_HUB75_B          GPIO_NUM_18
#define CS_PIN_HUB75_C          GPIO_NUM_19
#define CS_PIN_HUB75_D          GPIO_NUM_21
#define CS_PIN_HUB75_E          GPIO_NUM_12  // Held low for 1/16 scan (64x32)

#define CS_PIN_HUB75_CLK        GPIO_NUM_22
#define CS_PIN_HUB75_LAT        GPIO_NUM_26
#define CS_PIN_HUB75_OE         GPIO_NUM_25

// ---------------------------------------------------------------------------
// Physical Controls & Ambient Light Sensor (Input-only GPIOs 34..39)
// ---------------------------------------------------------------------------
#define CS_PIN_BUTTON           GPIO_NUM_35
#define CS_PIN_ENCODER_A        GPIO_NUM_39
#define CS_PIN_ENCODER_B        GPIO_NUM_36
#define CS_PIN_LIGHT_ADC        GPIO_NUM_34
#define CS_LIGHT_ADC_UNIT       ADC_UNIT_1
#define CS_LIGHT_ADC_CHANNEL    ADC_CHANNEL_6  // GPIO 34 = ADC1_CH6 on classic ESP32

// ---------------------------------------------------------------------------
// DFPlayer Mini Serial Audio (UART1 @ 9600 baud, 8N1)
// Legacy Arduino call: hwSerial.begin(9600, SERIAL_8N1, 13, 0) -> RX=13, TX=0
// ---------------------------------------------------------------------------
#define CS_DFPLAYER_UART_NUM    UART_NUM_1
#define CS_PIN_DFPLAYER_RX      GPIO_NUM_13
#define CS_PIN_DFPLAYER_TX      GPIO_NUM_0
#define CS_DFPLAYER_BAUD        9600
