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
#define MIN_BATTERY_START_PCT  40   // ниже — запрет старта сеанса (TODO #3)
#define CHRG_PIN      7   // TP4054 CHRG (open drain): к GND пока идёт заряд; hi-Z когда нет. К ESP через R или напрямую; INPUT_PULLUP
#define EN_WAKEUP     17  // LDO тракта; HIGH=PRE_START/сеанс/FINISH, LOW=меню/sleep
#define USB_DET       21  // вход детектор VBUS c USB c делителя 51k/100k

// --- DAC I2S → PCM5102A (стерео DAC) ---
#define I2S_BCLK               33          // BCK (Bit clock) PCM5102A
#define I2S_WCLK               35          // LRCK (Word select / LRC) PCM5102A  
#define I2S_DOUT               37          // DIN (Data in) PCM5102A
#define DAC_SAMPLE_RATE         8000
// Калибровка кодов ЦАП на 1 мА — поканально (TODO #1)
#define DEF_DAC_CODE_TO_MA_L   5900.0f     // коды/мА, левый канал
#define DEF_DAC_CODE_TO_MA_R   5900.0f     // коды/мА, правый канал
#define CAL_DAC_CODE_MIN       1000.0f     // диапазон/шаг редактора калибровки
#define CAL_DAC_CODE_MAX       12000.0f
#define CAL_DAC_CODE_STEP      50.0f

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
#define DEF_ADC_OFFSET_L_V      1.25f   // V offset для левого канала
#define DEF_ADC_OFFSET_R_V      1.25f   // V offset для правого канала
#define CAL_VOFFSET_MIN         0.0f    // диапазон/шаг редактора V offset
#define CAL_VOFFSET_MAX         3.30f
#define CAL_VOFFSET_STEP        0.01f
#define DEF_ADC_V_TO_MA         3.15f   // коэффициент пересчета V -> mA
#define CAL_V_TO_MA_MIN         0.0f    // диапазон/шаг редактора V->mA
#define CAL_V_TO_MA_MAX         10.0f
#define CAL_V_TO_MA_STEP        0.05f

#define ADC_RAW_RATE_HZ         32000
#define ADC_OUT_RATE_HZ         8000
#define ADC_DECIM               (ADC_RAW_RATE_HZ / ADC_OUT_RATE_HZ)
#define ADC_FRAME_SIZE          256
#define ADC_DMA_BUF_COUNT       4
#define ADC_RING_SIZE           8192
#define ADC_STATS_WINDOW_MS     1024   // кольцо 8192 @ 8кГц ≈ 1 с — как в v0.9.0
#define ADC_STATS_WINDOW_SAMPLES ((ADC_STATS_WINDOW_MS * ADC_OUT_RATE_HZ) / 1000)
#define ADC_CAPTURE_DELAY_MS    300
#define ADC_READ_TIMEOUT_MS     10