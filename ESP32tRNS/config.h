#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// === tRNS/tACS Device Configuration ===
// === Hardware: LOLIN S2 Mini (ESP32-S2) ===
// ============================================================================

// === TIMING CONFIG ===
#define SAMPLE_RATE         8000

// Буфер DAC: 2 секунды (для любых пресетов - tACS, tRNS, tDCS и т.д.)
// ВАЖНО: Храним только МОНО (правый канал), левый - константа для ADC!
#define LOOP_DURATION_SEC   2
#define SIGNAL_SAMPLES      (SAMPLE_RATE * LOOP_DURATION_SEC)  // 16000 samples = 2 sec МОНО

// === I2S DMA CONFIG (для DAC - PCM5102A) ===
#define I2S_NUM             I2S_NUM_0
#define DMA_BUFFER_COUNT    40   // Количество DMA дескрипторов (40×400 = 16000 фреймов = 2.00 сек @ 8kHz)
#define DMA_BUFFER_LEN      400  // Стерео-фреймов в каждом буфере (400 фреймов × 4 байта = 1600 байт)

// === PIN CONFIGURATION ===

// --- I2S → PCM5102A (стерео DAC) ---
#define I2S_BCLK    33  // BCK (Bit clock) PCM5102A
#define I2S_WCLK    35  // LRCK (Word select / LRC) PCM5102A  
#define I2S_DOUT    37  // DIN (Data in) PCM5102A

// --- ADC показометр тока (через шунт ~100Ω) ---
#define ADC_CURRENT_PIN 3        // GPIO3 для ADC
#define ADC_CHANNEL     ADC_CHANNEL_2  // ADC1_CH2
#define ADC_UNIT        ADC_UNIT_1

// === ADC DMA CONFIG ===
// Сигнал на шунте: ±0.1-0.2V + смещение 0.5V = диапазон 0.3-0.7V
// Для tRNS (100-640 Hz) достаточно 20 kHz
#define ADC_SAMPLE_RATE     20000  // Частота семплирования (20 kHz - оптимально)
#define ADC_FRAME_SIZE      1024   // Размер фрейма DMA (сэмплов)
#define ADC_DMA_BUF_COUNT   4      // Количество DMA буферов
#define ADC_READ_TIMEOUT_MS 100    // Timeout для чтения

// Кольцевой буфер для накопления данных
// ВАЖНО: Буфер согласован с DAC лупом (2 сек)! 
// ADC = 1× DAC луп → квадратное окно без растекания спектра
#define ADC_RING_SIZE       40000  // 2 сек @ 20kHz = 1× DAC луп (16000 @ 8kHz)
#define ADC_INVALID_VALUE   -32768 // Запрещенное значение (метка "данных ещё нет")

// === DAC SIGNAL PARAMETERS ===
#define MAX_VAL                 32767
#define MAX_VOLT                3.1f
#define DAC_RIGHT_AMPL_VOLTS    1.0f   // Амплитуда правого канала (tRNS/tACS сигнал)
#define DAC_LEFT_OFFSET_VOLTS   -0.5f  // Смещение левого канала для диф. усилителя ADC

// Максимальная длина имени пресета
#define PRESET_NAME_MAX_LEN     128

// === GAIN CONTROL ===
#define DEFAULT_GAIN            1.0f    // Коэффициент усиления по умолчанию (без изменений)
#define MIN_GAIN                0.0f    // Минимальный gain (полное подавление)
// Максимальный gain не ограничен (защита через насыщение int16)

#endif // CONFIG_H

