/*
 * Pin map. CHANGE TO MATCH YOUR WIRING.
 * Camera pins below are the common ESP32-S3 camera layout (ESP32-S3-EYE / Freenove ESP32-S3 CAM).
 *
 * Note for the electronics team: the camera uses most of GPIO4-18, which overlaps the
 * ESP32-S3 native touch pins (GPIO1-14) - another reason to use the MPR121 over I2C for touch.
 */
#pragma once

#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D0     11
#define CAM_PIN_D1      9
#define CAM_PIN_D2      8
#define CAM_PIN_D3     10
#define CAM_PIN_D4     12
#define CAM_PIN_D5     18
#define CAM_PIN_D6     17
#define CAM_PIN_D7     16
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

#define LED_RING_GPIO      14
#define LED_RING_COUNT     24
#define LED_RING_FRONT_IDX  0   /* LED closest to the camera window */

/* MPR121 capacitive touch controller (I2C). The ESP32-S3 native touch pins (GPIO1-14)
 * clash with the camera bus, which is why touch goes over I2C instead. */
#define TOUCH_I2C_SDA  39
#define TOUCH_I2C_SCL  38
#define TOUCH_I2C_ADDR 0x5A
#define TOUCH_PADS      9   /* 8 pads around the equator + 1 crown pad on top */
