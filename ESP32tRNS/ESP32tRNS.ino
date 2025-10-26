#include <driver/i2s.h>
#include <math.h>

// === CONFIG ===
#define SAMPLE_RATE         8000
#define I2S_NUM             I2S_NUM_0
#define SINE_FREQ           640

// Стратегия работы с DMA:
// 1. Создаём большие DMA буферы (2 секунды = 16000 сэмплов)
// 2. Заполняем их циклическим сигналом
// 3. DMA САМ гоняет данные, мы только подкладываем когда есть место
// 4. loop() свободен для ADC, USB OTG, команд и т.д.

#define DMA_BUFFER_COUNT    16   // Количество DMA дескрипторов
#define DMA_BUFFER_LEN      512  // Сэмплов в каждом буфере (512*2*2 = 2KB на буфер)
// Итого: 16 * 512 = 8192 сэмпла = ~1 секунда звука в DMA кольце

// Генерируемый сигнал: целое число периодов для бесшовного луплинга
const int samples_per_cycle = SAMPLE_RATE / SINE_FREQ;  // 12.5 -> округлится
const int SIGNAL_PERIODS = 10;  // Периодов в нашем сигнале
const int SIGNAL_SAMPLES = samples_per_cycle * SIGNAL_PERIODS;  // ~125 samples

// --- I2S Pins ---
#define I2S_BCLK    12  // BCK (Bit clock) PCM5102A
#define I2S_WCLK    16  // LRCK (Word select / LRC) PCM5102A
#define I2S_DOUT    18  // DIN (Data in) PCM5102A

// --- Globals ---
int16_t* signal_buffer = NULL;   // Короткий буфер с сигналом для циклирования
bool dma_prefilled = false;      // Флаг: DMA буферы заполнены

// Параметры сигнала
const int16_t max_val = 32767;
const float max_volt = 3.1;
const float ampl_volts = 1.0;
const float LEFT_CHANNEL_VOLTS = -0.5;

// === SETUP ===
void setup() {
  Serial.begin(921600);
  Serial.println("=== tRNS Device Booting ===");
  
  // Выделяем память под короткий сигнал (стерео = 2 канала)
  signal_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  if (!signal_buffer) {
    Serial.println("ERROR: Failed to allocate signal buffer!");
    while(1);
  }
  
  Serial.printf("Signal buffer: %d samples (%d periods), %d bytes\n", 
                SIGNAL_SAMPLES, SIGNAL_PERIODS, SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  
  // Генерируем сигнал ОДИН РАЗ
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

// === LOOP ===
void loop() {
  // ============================================================
  // СТРАТЕГИЯ: Неблокирующий loop для работы в реальном времени
  // ============================================================
  
  // 1. Подкладываем данные в DMA (неблокирующе)
  keepDMAFilled();
  
  // 2. TODO: Читаем ADC
  // readADC();
  
  // 3. TODO: Обрабатываем USB OTG команды
  // processUSBCommands();
  
  // 4. TODO: Управляем цифровым потенциометром
  // updateDigitalPot();
  
  // Небольшая задержка, чтобы не молотить впустую
  delay(1);
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

// Обновление сигнала "на лету" (вызывать из USB команды)
void updateSignal(int16_t* new_buffer, int num_samples) {
  // TODO: Можно обновить signal_buffer новыми данными
  // DMA автоматически начнёт проигрывать новый сигнал
  Serial.println("Signal update requested (not implemented yet)");
}
