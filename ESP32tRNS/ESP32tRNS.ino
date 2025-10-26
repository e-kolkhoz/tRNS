#include <driver/i2s.h>
#include <driver/adc.h>
#include <math.h>

// ============================================================================
// === tRNS/tACS Device for LOLIN S2 Mini (ESP32-S2) ===
// ============================================================================

// === TIMING CONFIG ===
#define SAMPLE_RATE         8000
#define I2S_NUM             I2S_NUM_0
#define SINE_FREQ           640

// Стратегия работы с DMA:
// 1. Создаём большие DMA буферы
// 2. Заполняем их циклическим сигналом (2 сек луп)
// 3. DMA САМ гоняет данные, мы только подкладываем когда есть место
// 4. loop() свободен для ADC, USB OTG, команд и т.д.

#define DMA_BUFFER_COUNT    16   // Количество DMA дескрипторов
#define DMA_BUFFER_LEN      512  // Сэмплов в каждом буфере (512*2*2 = 2KB на буфер)
// Итого: 16 * 512 = 8192 сэмпла = ~1 секунда звука в DMA кольце

// Генерируемый сигнал: 2 секунды = целое число периодов для бесшовного луплинга
const int LOOP_DURATION_SEC = 2;
const int SIGNAL_SAMPLES = SAMPLE_RATE * LOOP_DURATION_SEC;  // 16000 samples = 2 sec
const int SIGNAL_PERIODS = SINE_FREQ * LOOP_DURATION_SEC;    // 1280 периодов

// === PIN CONFIGURATION ===

// --- I2S → PCM5102A (стерео DAC) ---
// GPIO идут подряд - удобно для разводки на одной стороне платы
#define I2S_BCLK    33  // BCK (Bit clock) PCM5102A
#define I2S_WCLK    35  // LRCK (Word select / LRC) PCM5102A  
#define I2S_DOUT    37  // DIN (Data in) PCM5102A

// --- X9C103S цифровой потенциометр 10kΩ ---
#define X9C_INC     5   // Increment pulse
#define X9C_UD      7   // Up/Down direction (HIGH=up, LOW=down)
#define X9C_CS      9   // Chip Select (LOW=active)

// --- ADC показометр тока (через шунт ~100Ω) ---
#define ADC_CURRENT 3   // ADC1_CH2 - измерение тока
#define ADC_CHANNEL ADC1_CHANNEL_2

// === ADC CONFIG ===
#define ADC_OVERSAMPLE_RATE 80000  // Частота оверсэмплинга (макс для ESP32-S2)
#define ADC_OUTPUT_RATE     8000   // Частота после децимации
#define ADC_DECIMATION_FACTOR (ADC_OVERSAMPLE_RATE / ADC_OUTPUT_RATE)  // 10:1
#define ADC_BUFFER_SIZE     8000   // 1 секунда буфер для отправки в Android

// === GLOBALS ===

// --- DAC сигнал ---
int16_t* signal_buffer = NULL;   // Буфер с сигналом для циклирования (2 сек)
bool dma_prefilled = false;      // Флаг: DMA буферы заполнены

// Параметры сигнала
const int16_t max_val = 32767;
const float max_volt = 3.1;
const float ampl_volts = 1.0;
const float LEFT_CHANNEL_VOLTS = -0.5;  // Смещение для диф. усилителя

// --- X9C103S потенциометр ---
int x9c_position = 99;  // Текущая позиция (0-99), старт на максимуме
const int X9C_MAX_STEPS = 99;
const int X9C_PULSE_DELAY_US = 1;  // Задержка импульса (мкс)

// --- ADC показометр ---
uint16_t* adc_buffer = NULL;     // Буфер для ADC данных
int adc_buffer_index = 0;        // Индекс заполнения
uint32_t adc_decimation_accumulator = 0;  // Накопитель для усреднения
int adc_decimation_count = 0;    // Счётчик для децимации
uint32_t last_adc_read_us = 0;   // Таймер для ADC
const uint32_t adc_interval_us = 1000000 / ADC_OVERSAMPLE_RATE;  // ~12.5 мкс

// ============================================================================
// === SETUP ===
// ============================================================================

void setup() {
  Serial.begin(921600);
  delay(100);
  Serial.println("\n=== tRNS/tACS Device Booting ===");
  Serial.println("Hardware: LOLIN S2 Mini (ESP32-S2)");
  
  // --- Инициализация X9C103S ---
  initDigitalPot();
  
  // --- Инициализация ADC ---
  initADC();
  
  // --- Выделение памяти ---
  // Буфер для DAC сигнала (2 сек, стерео)
  signal_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  if (!signal_buffer) {
    Serial.println("ERROR: Failed to allocate signal buffer!");
    while(1);
  }
  Serial.printf("Signal buffer: %d samples (%.1f sec, %d periods), %d KB\n", 
                SIGNAL_SAMPLES, (float)LOOP_DURATION_SEC, SIGNAL_PERIODS, 
                (SIGNAL_SAMPLES * 2 * sizeof(int16_t)) / 1024);
  
  // Буфер для ADC (1 сек)
  adc_buffer = (uint16_t*)malloc(ADC_BUFFER_SIZE * sizeof(uint16_t));
  if (!adc_buffer) {
    Serial.println("ERROR: Failed to allocate ADC buffer!");
    while(1);
  }
  Serial.printf("ADC buffer: %d samples (1 sec), %d KB\n", 
                ADC_BUFFER_SIZE, (ADC_BUFFER_SIZE * sizeof(uint16_t)) / 1024);
  
  // --- Генерация сигнала ---
  generateSignal();

  // --- I2S CONFIG с БОЛЬШИМИ DMA буферами ---
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = DMA_BUFFER_COUNT,  // 16 буферов
    .dma_buf_len = DMA_BUFFER_LEN,       // по 512 samples каждый
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_WCLK,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM, &pin_config);
  i2s_set_clk(I2S_NUM, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

  int total_dma_samples = DMA_BUFFER_COUNT * DMA_BUFFER_LEN;
  Serial.printf("I2S initialized. DMA ring buffer: %d samples (%.1f sec)\n", 
                total_dma_samples, (float)total_dma_samples / SAMPLE_RATE);
  
  // Предзаполняем DMA буферы
  prefillDMABuffers();
  
  Serial.println("=== Ready! ===");
}

// Генерация сигнала (синус или шум)
void generateSignal() {
  int16_t LEFT_CHANNEL_VAL = (int16_t)(max_val * LEFT_CHANNEL_VOLTS / max_volt);
  int16_t ampl_val = (int16_t)(max_val * ampl_volts / max_volt);
  
  Serial.println("Generating sine wave...");
  
  for (int i = 0; i < SIGNAL_SAMPLES; i++) {
    // Синус
    float t = (float)(i % samples_per_cycle) / samples_per_cycle;
    int16_t right_value = (int16_t)(sinf(2.0f * M_PI * t) * ampl_val);
    
    // I2S_CHANNEL_FMT_RIGHT_LEFT → буфер: [L, R, L, R, ...]
    signal_buffer[i * 2]     = LEFT_CHANNEL_VAL; // Левый канал: постоянное напряжение
    signal_buffer[i * 2 + 1] = right_value;      // Правый канал: синус
  }
  
  Serial.println("Signal generated!");
}

// Предзаполнение DMA буферов
void prefillDMABuffers() {
  Serial.println("Prefilling DMA buffers...");
  
  const int total_dma_samples = DMA_BUFFER_COUNT * DMA_BUFFER_LEN;
  int samples_written = 0;
  
  // Заполняем все DMA буферы, циклически повторяя наш сигнал
  while (samples_written < total_dma_samples) {
    size_t bytes_written = 0;
    
    // Отправляем наш циклический сигнал
    esp_err_t result = i2s_write(I2S_NUM, 
                                   signal_buffer, 
                                   SIGNAL_SAMPLES * 2 * sizeof(int16_t), 
                                   &bytes_written, 
                                   portMAX_DELAY);
    
    if (result != ESP_OK) {
      Serial.printf("ERROR: i2s_write failed: %d\n", result);
      break;
    }
    
    samples_written += bytes_written / (2 * sizeof(int16_t));
  }
  
  dma_prefilled = true;
  Serial.printf("DMA prefilled: %d samples written\n", samples_written);
}

// ============================================================================
// === LOOP ===
// ============================================================================

void loop() {
  // ============================================================
  // СТРАТЕГИЯ: Неблокирующий loop для работы в реальном времени
  // ============================================================
  
  // 1. Подкладываем данные в DMA (неблокирующе, ~1мс)
  keepDMAFilled();
  
  // 2. Читаем ADC с оверсэмплингом
  readADCNonBlocking();
  
  // 3. TODO: Обрабатываем USB OTG команды от Android
  // processUSBCommands();
  
  // 4. Если ADC буфер заполнен - отправляем в Serial (потом в USB)
  if (adc_buffer_index >= ADC_BUFFER_SIZE) {
    sendADCBuffer();
  }
  
  // Минимальная задержка для стабильности
  delayMicroseconds(10);
}

// Поддерживаем DMA буферы заполненными (неблокирующе!)
void keepDMAFilled() {
  size_t bytes_written = 0;
  
  // Пытаемся отправить данные с КОРОТКИМ timeout (1 мс)
  // Если DMA буфер полон - функция вернётся быстро, и мы продолжим loop()
  esp_err_t result = i2s_write(I2S_NUM, 
                                 signal_buffer, 
                                 SIGNAL_SAMPLES * 2 * sizeof(int16_t), 
                                 &bytes_written, 
                                 pdMS_TO_TICKS(1));  // КОРОТКИЙ timeout!
  
  // Если что-то записали - хорошо
  // Если DMA полон (bytes_written == 0) - тоже норм, значит звук играет
  // Ошибки логируем только если критичные
  if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
    Serial.printf("WARN: i2s_write returned %d\n", result);
  }
}

// ============================================================================
// === X9C103S DIGITAL POTENTIOMETER ===
// ============================================================================

// Инициализация цифрового потенциометра
void initDigitalPot() {
  pinMode(X9C_INC, OUTPUT);
  pinMode(X9C_UD, OUTPUT);
  pinMode(X9C_CS, OUTPUT);
  
  // Начальное состояние: неактивен
  digitalWrite(X9C_CS, HIGH);
  digitalWrite(X9C_INC, HIGH);
  digitalWrite(X9C_UD, HIGH);
  
  // Сбрасываем на максимальное сопротивление (безопасный старт)
  setDigitalPotPosition(X9C_MAX_STEPS);
  
  Serial.println("X9C103S: Initialized at max resistance (safe start)");
}

// Установка абсолютной позиции потенциометра (0-99)
void setDigitalPotPosition(int target_position) {
  if (target_position < 0) target_position = 0;
  if (target_position > X9C_MAX_STEPS) target_position = X9C_MAX_STEPS;
  
  int steps = abs(target_position - x9c_position);
  bool direction = (target_position > x9c_position);  // true = up
  
  if (steps == 0) return;  // Уже на месте
  
  digitalWrite(X9C_CS, LOW);  // Активируем чип
  digitalWrite(X9C_UD, direction ? HIGH : LOW);  // Устанавливаем направление
  delayMicroseconds(1);
  
  // Посылаем импульсы
  for (int i = 0; i < steps; i++) {
    digitalWrite(X9C_INC, LOW);
    delayMicroseconds(X9C_PULSE_DELAY_US);
    digitalWrite(X9C_INC, HIGH);
    delayMicroseconds(X9C_PULSE_DELAY_US);
  }
  
  digitalWrite(X9C_CS, HIGH);  // Деактивируем чип (сохраняет позицию)
  x9c_position = target_position;
}

// Увеличить сопротивление на N шагов
void digitalPotIncrease(int steps) {
  setDigitalPotPosition(x9c_position + steps);
}

// Уменьшить сопротивление на N шагов
void digitalPotDecrease(int steps) {
  setDigitalPotPosition(x9c_position - steps);
}

// Получить текущую позицию
int getDigitalPotPosition() {
  return x9c_position;
}

// ============================================================================
// === ADC CURRENT MONITOR (показометр тока) ===
// ============================================================================

// Инициализация ADC
void initADC() {
  // Настройка ADC1
  adc1_config_width(ADC_WIDTH_BIT_12);  // 12-бит разрешение (0-4095)
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);  // 0-3.3V диапазон
  
  Serial.println("ADC: Initialized (12-bit, 0-3.3V range)");
  Serial.printf("ADC: Oversample rate: %d Hz, Output rate: %d Hz (decimation %d:1)\n",
                ADC_OVERSAMPLE_RATE, ADC_OUTPUT_RATE, ADC_DECIMATION_FACTOR);
}

// Неблокирующее чтение ADC с оверсэмплингом
void readADCNonBlocking() {
  uint32_t current_time_us = micros();
  
  // Проверяем, пора ли читать следующий сэмпл
  if (current_time_us - last_adc_read_us < adc_interval_us) {
    return;  // Ещё рано
  }
  
  last_adc_read_us = current_time_us;
  
  // Читаем ADC
  int raw_value = adc1_get_raw(ADC_CHANNEL);
  
  // Накапливаем для усреднения (децимация)
  adc_decimation_accumulator += raw_value;
  adc_decimation_count++;
  
  // Когда накопили достаточно - усредняем и сохраняем
  if (adc_decimation_count >= ADC_DECIMATION_FACTOR) {
    uint16_t averaged_value = adc_decimation_accumulator / ADC_DECIMATION_FACTOR;
    
    // Сохраняем в буфер (если есть место)
    if (adc_buffer_index < ADC_BUFFER_SIZE) {
      adc_buffer[adc_buffer_index++] = averaged_value;
    }
    
    // Сбрасываем накопители
    adc_decimation_accumulator = 0;
    adc_decimation_count = 0;
  }
}

// Отправка ADC буфера (пока в Serial, потом в USB)
void sendADCBuffer() {
  Serial.printf("\n=== ADC Buffer Ready: %d samples ===\n", adc_buffer_index);
  
  // TODO: Отправить через USB OTG в Android
  // Пока просто показываем статистику
  
  // Вычисляем базовую статистику
  uint32_t sum = 0;
  uint16_t min_val = 4095;
  uint16_t max_val = 0;
  
  for (int i = 0; i < adc_buffer_index; i++) {
    sum += adc_buffer[i];
    if (adc_buffer[i] < min_val) min_val = adc_buffer[i];
    if (adc_buffer[i] > max_val) max_val = adc_buffer[i];
  }
  
  uint16_t avg = sum / adc_buffer_index;
  float voltage_avg = (avg / 4095.0) * 3.3;
  float voltage_min = (min_val / 4095.0) * 3.3;
  float voltage_max = (max_val / 4095.0) * 3.3;
  
  Serial.printf("Average: %d (%.3fV), Min: %d (%.3fV), Max: %d (%.3fV)\n",
                avg, voltage_avg, min_val, voltage_min, max_val, voltage_max);
  Serial.printf("Peak-to-peak: %.3fV\n", voltage_max - voltage_min);
  
  // Сбрасываем буфер
  adc_buffer_index = 0;
}

// ============================================================================
// === SIGNAL UPDATE (для команд от Android) ===
// ============================================================================

// Обновление сигнала "на лету" (вызывать из USB команды)
void updateSignal(int16_t* new_buffer, int num_samples) {
  // TODO: Можно обновить signal_buffer новыми данными
  // DMA автоматически начнёт проигрывать новый сигнал
  Serial.println("Signal update requested (not implemented yet)");
}
