#pragma once

// GPIO0 = BOOT button = pin monitored by TinyUF2 at startup
#define BOOT_UF2_GPIO 0

// ENC SIQ-02FVS3 с кнопкой простейший вариант, коротит при нажатии на землю
#define ENC_A      1
#define ENC_S      2
#define ENC_B      3

// Neopixel (WS2812B on Lolin S3)
#define NEOPIXEL_PIN  47  // SPICLC_P

// NAND FLASH
#define SD_CLK        4
#define SD_COM        5
#define SD_D0        10
#define SD_D1        11
#define SD_D2        13
#define SD_D3        14

// BATTERY AND POWER
#define PLUS_BAT_ADC  6   // аналоговый вход с делителя 360k/100k батарейки 3.7V 
#define VBUS_STAT     7   // вход детектор VBUS c USB c делителя 51k/100k относительно входа CHRG TP4054S5
#define EN_WAKEUP     17  // выход вкл. аналоговые модули
#define USB_DET       21  // вход детектор VBUS c USB c делителя 51k/100k

// --- I2S → PCM5102A (стерео DAC) ---
#define I2S_BCLK               33          // BCK (Bit clock) PCM5102A
#define I2S_WCLK               35          // LRCK (Word select / LRC) PCM5102A  
#define I2S_DOUT               37          // DIN (Data in) PCM5102A

// I2C OLED Display (128x64, SSD1306) ---
#define OLED_SDA  8
#define OLED_SCL  9
#define OLED_I2C_HZ    100000  // на отладке надёжнее 100k; потом можно 400000
#define DISPLAY_ADDR   0x3C 

// ADC DMA 
#define ADC_SENCE2 36
#define ADC_SENCE1 34

