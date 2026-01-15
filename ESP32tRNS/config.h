#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// === tRNS/tACS Device Configuration ===
// === Hardware: LOLIN S2 Mini (ESP32-S2) ===
// ============================================================================

// === TIMING CONFIG ===
#define SAMPLE_RATE         8000

// Буфер DAC: 16384 сэмпла (2.048 сек @ 8kHz) - FFT-friendly размер (степень 2)
// ВАЖНО: Храним МОНО знаковый сигнал, преобразуем в СТЕРЕО sign-magnitude для H-моста
// Правый канал = модуль (abs), Левый канал = знак (32767=pos, 0=neg)
#define SIGNAL_SAMPLES      16384  // Ровно 2^14 для FFT без растекания спектра
#define LOOP_DURATION_SEC   ((float)SIGNAL_SAMPLES / SAMPLE_RATE)  // 2.048 сек

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

// ENC с кнопкой
#define ENC_S1      18
#define ENC_S2      16
#define ENC_KEY     39

// --- I2S → PCM5102A (стерео DAC) ---
#define I2S_BCLK    33  // BCK (Bit clock) PCM5102A
#define I2S_WCLK    35  // LRCK (Word select / LRC) PCM5102A  
#define I2S_DOUT    37  // DIN (Data in) PCM5102A

// --- ADC показометр тока (sign-magnitude от H-моста) ---
#define ADC_SIGN_PIN     3             // GPIO3 (ADC1_CH2) для знака (можно как цифровой)
#define ADC_MOD_PIN      5             // GPIO5 (ADC1_CH4) для модуля (аналоговый)
#define ADC_SIGN_CHANNEL ADC_CHANNEL_2 // Канал для знака
#define ADC_MOD_CHANNEL  ADC_CHANNEL_4 // Канал для модуля
#define ADC_UNIT         ADC_UNIT_1

// --- I2C OLED Display (128x64, SSD1306 или SH1106) ---
#define I2C_SDA        7   // GPIO7 для I2C SDA
#define I2C_SCL        9   // GPIO9 для I2C SCL (если не доступен, можно использовать GPIO6)
#define I2C_FREQ       400000  // Частота I2C: 400 kHz
#define DISPLAY_ADDR   0x3C  // Адрес I2C для SSD1306 (0x3C или 0x3D)

// === ADC DMA CONFIG ===
// Sign-magnitude ADC: 2 канала синхронно (знак + модуль)
// Для tRNS (100-640 Hz) достаточно 20 kHz
#define ADC_SAMPLE_RATE      20000   // Частота семплирования (20 kHz - оптимально)
#define ADC_FRAME_SIZE       512     // Размер фрейма DMA (×2 канала = 1024 сэмпла)
#define ADC_DMA_BUF_COUNT    4       // Количество DMA буферов
#define ADC_READ_TIMEOUT_MS  100     // Timeout для чтения

// Аттенюатор ADC (настраиваемый для разных этапов тестирования)
// ADC_ATTEN_DB_0:   0-1.1V   (боевой режим с делителем)
// ADC_ATTEN_DB_11:  0-3.3V   (тестирование напрямую с DAC)
#define ADC_ATTEN            ADC_ATTEN_DB_11  // Меняй на DB_0 для боевого режима
#define ADC_MAX_VOLTAGE      ((ADC_ATTEN == ADC_ATTEN_DB_0) ? 1.1f : 3.3f)

// Конвертация ADC кодов в напряжение и ток
#define ADC_V_TO_MA          10.0f   // Множитель для получения тока в mA
#define ADC_CAPTURE_DELAY_MS 200     // Задержка запуска записи ADC после старта DAC (мс)
#define ADC_STATS_WINDOW_MS  200     // Окно статистики/гистограммы для ADC (мс)
#define ADC_STATS_WINDOW_SAMPLES ((ADC_STATS_WINDOW_MS * ADC_SAMPLE_RATE) / 1000)

// Порог детектирования знака (если sign > этого, то положительный)
#define ADC_SIGN_THRESHOLD   2048    // Середина диапазона 12-bit ADC

// Кольцевой буфер для накопления данных
// ВАЖНО: Буфер согласован с DAC лупом (2.048 сек)! 
// ADC = 1× DAC луп → квадратное окно без растекания спектра
#define ADC_RING_SIZE       40960  // 2.048 сек @ 20kHz = 1× DAC луп (16384 @ 8kHz)
#define ADC_INVALID_VALUE   -32768 // Запрещенное значение (метка "данных ещё нет")

// === DAC SIGNAL PARAMETERS (Sign-Magnitude для H-моста) ===
#define MAX_VAL                 32767
#define MAX_VOLT                3.1f
// ВАЖНО: DAC униполярный! Выходное напряжение всегда >= 0
// Правый канал = модуль сигнала (всегда abs >= 0, диапазон [0, 32767])
// Левый канал = знак (32767 = положительный, 0 = отрицательный)
#define DAC_SIGN_POSITIVE       32767  // Значение для положительного знака
#define DAC_SIGN_NEGATIVE       0      // Значение для отрицательного знака

// === POLARITY INVERSION (для случая перепутанных электродов) ===
// Если катод и анод перепутаны - меняй на true, перепрошей, готово!
#define POLARITY_INVERT         false  // true = инвертировать знак на DAC и ADC

// Максимальная длина имени пресета
#define PRESET_NAME_MAX_LEN     128

// === GAIN CONTROL ===
#define DEFAULT_GAIN            0.375f  // 0.714f // Коэффициент усиления по умолчанию (без изменений)
#define MIN_GAIN                0.0f    // Минимальный gain (полное подавление)
// Максимальный gain не ограничен (защита через насыщение int16)

#endif // CONFIG_H

