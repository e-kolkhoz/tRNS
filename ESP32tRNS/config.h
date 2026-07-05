#pragma once

// GPIO0 = BOOT button = pin monitored by TinyUF2 at startup
#define BOOT_UF2_GPIO 0

// ENC SIQ-02FVS3 с кнопкой простейший вариант, коротит при нажатии на землю
#define ENC_A      1
#define ENC_S      2
#define ENC_B      3

// Neopixel (WS2812B on Lolin S3)
#define NEOPIXEL_PIN  47  // SPICLC_P

// BATTERY AND POWER
#define PLUS_BAT_ADC  6   // аналоговый вход с делителя 360k/100k батарейки 3.7V 
#define CHRG_PIN      7   // TP4054 CHRG (open drain): к GND пока идёт заряд; hi-Z когда нет. К ESP через R или напрямую; INPUT_PULLUP
#define EN_WAKEUP     17  // выход вкл. аналоговые модули
#define USB_DET       21  // вход детектор VBUS c USB c делителя 51k/100k

// --- DAC I2S → PCM5102A (стерео DAC) ---
#define I2S_BCLK               33          // BCK (Bit clock) PCM5102A
#define I2S_WCLK               35          // LRCK (Word select / LRC) PCM5102A  
#define I2S_DOUT               37          // DIN (Data in) PCM5102A
#define DAC_SAMPLE_RATE         8000
#define DEF_DAC_CODE_TO_MA     5500.0f     // коды/мА (калибровка: ~1В ≈ 2мА)

// I2C OLED Display (128x64, SSD1306) ---
#define OLED_SDA  8
#define OLED_SCL  9
#define OLED_I2C_HZ    100000
#define DISPLAY_ADDR   0x3C 

// ADC токовая ОС: униполяр + сдвиг (ИОН), стартовые дефолты под atten=11dB
// Только ADC1 GPIO1-10! GPIO11+ = ADC2, continuous на S3 не работает.
#define ADC_SENSE1  4    // LEFT  ADC1_CH3
#define ADC_SENSE2  5    // RIGHT ADC1_CH4
#define ADC_MOD_ATTEN           ADC_ATTEN_DB_12
#define ADC_MAX_VOLTAGE         3.3f
#define DEF_ADC_OFFSET_L_V      1.18f   // V offset для левого канала
#define DEF_ADC_OFFSET_R_V      1.18f   // V offset для правого канала
#define DEF_ADC_V_TO_MA         0.37f   // коэффициент пересчета V -> mA

#define ADC_RAW_RATE_HZ         32000
#define ADC_OUT_RATE_HZ         8000
#define ADC_DECIM               (ADC_RAW_RATE_HZ / ADC_OUT_RATE_HZ)
#define ADC_FRAME_SIZE          256
#define ADC_DMA_BUF_COUNT       4
#define ADC_RING_SIZE           8192
#define ADC_STATS_WINDOW_MS     200
#define ADC_STATS_WINDOW_SAMPLES ((ADC_STATS_WINDOW_MS * ADC_OUT_RATE_HZ) / 1000)
#define ADC_CAPTURE_DELAY_MS    300
#define ADC_READ_TIMEOUT_MS     10