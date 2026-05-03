#pragma once

// GPIO0 = BOOT button = pin monitored by TinyUF2 at startup
#define BOOT_UF2_GPIO 0

// Neopixel (WS2812B on Waveshare ESP32-S3-Zero)
#define NEOPIXEL_PIN  21

// I2C OLED (SSD1306 128x64)
#define OLED_SDA_PIN  10
#define OLED_SCL_PIN  11
#define OLED_I2C_ADDR 0x3C

// I2S PDM TX outputs (TWO_LINE_DAC)
#define TONE_DOUT1_PIN 1
#define TONE_DOUT2_PIN 2
