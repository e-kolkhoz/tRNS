#include "dac_control.h"
#include "usb_commands.h"
#include <math.h>

// Глобальные переменные
int16_t* signal_buffer = NULL;  // МОНО буфер (только правый канал)
bool dma_prefilled = false;
char current_preset_name[PRESET_NAME_MAX_LEN] = "No preset loaded";
float dac_gain = DEFAULT_GAIN;  // Коэффициент усиления (по умолчанию 1.0)

// Константа для левого канала (вычисляется один раз)
static int16_t LEFT_CHANNEL_CONST = 0;

// Временный стерео-буфер для отправки в I2S DMA
// Формируется на лету из МОНО signal_buffer + LEFT_CHANNEL_CONST
static int16_t* stereo_buffer = NULL;

// Заполнить стерео-буфер из МОНО: [L_CONST, R*gain, L_CONST, R*gain, ...]
// С применением gain и насыщением для int16
static void fillStereoBuffer() {
  for (int i = 0; i < SIGNAL_SAMPLES; i++) {
    stereo_buffer[i * 2] = LEFT_CHANNEL_CONST;  // Левый: константа
    
    // Правый: сигнал с gain и насыщением
    float sample_with_gain = signal_buffer[i] * dac_gain;
    
    // Насыщение (clamping) для int16: [-32768, 32767]
    if (sample_with_gain > 32767.0f) {
      stereo_buffer[i * 2 + 1] = 32767;
    } else if (sample_with_gain < -32768.0f) {
      stereo_buffer[i * 2 + 1] = -32768;
    } else {
      stereo_buffer[i * 2 + 1] = (int16_t)sample_with_gain;
    }
  }
}

// Инициализация I2S и DMA для DAC
void initDAC() {
  usbLog("=== I2S DAC Init (PCM5102A) ===");
  
  // Вычисляем константу для левого канала (смещение ADC)
  LEFT_CHANNEL_CONST = (int16_t)(MAX_VAL * DAC_LEFT_OFFSET_VOLTS / MAX_VOLT);
  usbLogf("Left channel const: %d (%.3fV for ADC offset)", 
          LEFT_CHANNEL_CONST, DAC_LEFT_OFFSET_VOLTS);
  
  // Выделяем временный стерео-буфер для I2S DMA
  stereo_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  if (!stereo_buffer) {
    usbError("Failed to allocate stereo temp buffer!");
    return;
  }
  usbLogf("Stereo temp buffer: %d bytes", SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  
  // Конфигурация I2S с большими DMA буферами
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = DMA_BUFFER_COUNT,
    .dma_buf_len = DMA_BUFFER_LEN,
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
  usbLogf("I2S initialized. DMA ring buffer: %d samples (%.1f sec)", 
          total_dma_samples, (float)total_dma_samples / SAMPLE_RATE);
  
  // Предзаполняем DMA буферы
  prefillDMABuffers();
  
  usbLog("I2S DAC: Ready!");
}

// Установить новый сигнал для DAC (замена текущего буфера)
void setSignalBuffer(int16_t* new_buffer, int num_samples) {
  if (new_buffer == NULL || num_samples != SIGNAL_SAMPLES) {
    usbError("setSignalBuffer: Invalid buffer or size mismatch!");
    return;
  }
  
  // Заменяем указатель (МОНО буфер - только правый канал)
  signal_buffer = new_buffer;
  
  // Сбрасываем флаг предзаполнения, чтобы DMA перезагрузилась
  dma_prefilled = false;
  prefillDMABuffers();
  
  usbLogf("Signal buffer updated: %d samples (MONO)", num_samples);
}

// Генерация демо-сигнала (синус 640 Гц) - для отладки
void generateDemoSignal() {
  int16_t ampl_val = (int16_t)(MAX_VAL * DAC_RIGHT_AMPL_VOLTS / MAX_VOLT);
  const int samples_per_cycle = SAMPLE_RATE / 640;
  
  usbLog("Generating demo sine wave (640 Hz)...");
  
  // Генерируем ТОЛЬКО МОНО (правый канал)!
  // Левый канал = константа, не храним в памяти
  for (int i = 0; i < SIGNAL_SAMPLES; i++) {
    float t = (float)(i % samples_per_cycle) / samples_per_cycle;
    signal_buffer[i] = (int16_t)(sinf(2.0f * M_PI * t) * ampl_val);
  }
  
  // Устанавливаем имя пресета
  snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tACS %dHz 1mA demo", 640);
  
  usbLog("Demo signal generated!");
  usbLogf("Preset: '%s'", current_preset_name);
}

// Предзаполнение DMA буферов
void prefillDMABuffers() {
  usbLog("Prefilling DMA buffers...");
  
  // Формируем стерео из МОНО + константа
  fillStereoBuffer();
  
  const int total_dma_samples = DMA_BUFFER_COUNT * DMA_BUFFER_LEN;
  int samples_written = 0;
  
  // Заполняем все DMA буферы, циклически повторяя наш сигнал
  while (samples_written < total_dma_samples) {
    size_t bytes_written = 0;
    
    // Отправляем стерео-буфер в I2S
    esp_err_t result = i2s_write(I2S_NUM, 
                                   stereo_buffer, 
                                   SIGNAL_SAMPLES * 2 * sizeof(int16_t), 
                                   &bytes_written, 
                                   portMAX_DELAY);
    
    if (result != ESP_OK) {
      usbError("i2s_write failed during prefill");
      break;
    }
    
    samples_written += bytes_written / (2 * sizeof(int16_t));
  }
  
  dma_prefilled = true;
  usbLogf("DMA prefilled: %d samples written", samples_written);
}

// Поддержание DMA буферов заполненными (неблокирующе!)
void keepDMAFilled() {
  size_t bytes_written = 0;
  
  // Стерео-буфер уже заполнен, просто отправляем с коротким timeout
  // (fillStereoBuffer уже вызван в prefillDMABuffers или setSignalBuffer)
  
  // Пытаемся отправить данные с КОРОТКИМ timeout (1 мс)
  // Если DMA буфер полон - функция вернётся быстро, и мы продолжим loop()
  esp_err_t result = i2s_write(I2S_NUM, 
                                 stereo_buffer, 
                                 SIGNAL_SAMPLES * 2 * sizeof(int16_t), 
                                 &bytes_written, 
                                 pdMS_TO_TICKS(1));  // КОРОТКИЙ timeout!
  
  // Если что-то записали - хорошо
  // Если DMA полон (bytes_written == 0) - тоже норм, значит звук играет
  // Ошибки логируем только если критичные
  if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
    usbWarn("i2s_write error in keepDMAFilled");
  }
}

// === GAIN CONTROL ===

// Установить коэффициент усиления (gain) для правого канала
void setDACGain(float gain) {
  if (gain < MIN_GAIN) {
    gain = MIN_GAIN;  // Ограничение снизу
  }
  
  dac_gain = gain;
  
  // Пересобираем стерео-буфер с новым gain
  fillStereoBuffer();
  
  usbLogf("DAC Gain set to %.2f", dac_gain);
}

// Получить текущий gain
float getDACGain() {
  return dac_gain;
}


