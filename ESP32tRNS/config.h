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
// Размер фрагмента для пополнения DMA (в миллисекундах)
// Меньший размер = быстрее отклик на gain, но больше вызовов keepDMAFilled
#define FRAGMENT_SIZE_MS    100  // 100 мс = 800 моно-сэмплов = 1600 стерео-сэмплов @ 8kHz
#define FRAGMENT_SAMPLES    ((FRAGMENT_SIZE_MS * SAMPLE_RATE * 2) / 1000)  // Стерео-сэмплов в фрагменте
// Уменьшенный DMA буфер: достаточно 2-3 циклов пополнения (200-300 мс)
// Это ускоряет отклик на gain и уменьшает задержку
#define DMA_BUFFER_COUNT    8    // Количество DMA дескрипторов (8×400 = 3200 фреймов = 0.40 сек @ 8kHz)
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

// --- I2C OLED Display (128x64, SSD1306 или SH1106) ---
#define I2C_SDA        7   // GPIO7 для I2C SDA
#define I2C_SCL        9   // GPIO9 для I2C SCL (если не доступен, можно использовать GPIO6)
#define I2C_FREQ       400000  // Частота I2C: 400 kHz
#define DISPLAY_ADDR   0x3C  // Адрес I2C для SSD1306 (0x3C или 0x3D)

// === ADC DMA CONFIG ===
// Сигнал на шунте: ±0.1-0.2V + смещение 0.5V = диапазон 0.3-0.7V
// Для tRNS (100-640 Hz) достаточно 20 kHz
#define ADC_SAMPLE_RATE     20000   // Частота семплирования (20 kHz - оптимально)
#define ADC_FRAME_SIZE      1024    // Размер фрейма DMA (сэмплов)
#define ADC_DMA_BUF_COUNT   4       // Количество DMA буферов
#define ADC_READ_TIMEOUT_MS 100     // Timeout для чтения
#define ADC_CENTER_V        0.6175f // Центральное напряжение, соотвтетствующее нулю вольт
#define ADC_V_TO_MA         10.0f   // Множитель для получения тока в mA
#define ADC_CAPTURE_DELAY_MS 200    // Задержка запуска записи ADC после старта DAC (мс)
#define ADC_STATS_WINDOW_MS 200     // Окно статистики/гистограммы для ADC (мс)
#define ADC_STATS_WINDOW_SAMPLES ((ADC_STATS_WINDOW_MS * ADC_SAMPLE_RATE) / 1000)

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
#define DEFAULT_GAIN            0.714f    // Коэффициент усиления по умолчанию (без изменений)
#define MIN_GAIN                0.0f    // Минимальный gain (полное подавление)
// Максимальный gain не ограничен (защита через насыщение int16)

#endif // CONFIG_H

