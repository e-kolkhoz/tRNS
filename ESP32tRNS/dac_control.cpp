#include "dac_control.h"
#include "display_control.h"
#include "adc_control.h"
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
static const uint32_t STEREO_BUFFER_SIZE = SIGNAL_SAMPLES * 2;

// Позиция в stereo_buffer для кольцевого доступа (в стерео-сэмплах)
static uint32_t stereo_buffer_pos = 0;

// Буфер для фрагмента (FRAGMENT_SAMPLES стерео-сэмплов)
static int16_t* stereo_buffer_fragment = NULL;

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

// Копировать фрагмент из stereo_buffer в stereo_buffer_fragment с кольцевым доступом
static void copyFragmentFromStereoBuffer(uint32_t start_pos) {
  for (uint32_t i = 0; i < FRAGMENT_SAMPLES; i++) {
    uint32_t src_idx = (start_pos + i) % STEREO_BUFFER_SIZE;
    stereo_buffer_fragment[i] = stereo_buffer[src_idx];
  }
}

// Записать подготовленный фрагмент во внутренний DMA буфер I2S
// timeout_ticks = 0 → неблокирующий; больше 0 → ждём указанное время
static bool writeFragmentToDMA(TickType_t timeout_ticks) {
  const uint32_t start_pos = stereo_buffer_pos;
  copyFragmentFromStereoBuffer(start_pos);
  
  size_t bytes_written = 0;
  const size_t bytes_to_write = FRAGMENT_SAMPLES * sizeof(int16_t);
  
  esp_err_t result = i2s_write(I2S_NUM,
                               stereo_buffer_fragment,
                               bytes_to_write,
                               &bytes_written,
                               timeout_ticks);
  
  if (result == ESP_OK && bytes_written > 0) {
    uint32_t samples_written = bytes_written / sizeof(int16_t);
    stereo_buffer_pos = (start_pos + samples_written) % STEREO_BUFFER_SIZE;
    return true;
  }
  
  if (result == ESP_ERR_TIMEOUT || bytes_written == 0) {
    // DMA заполнен - ничего страшного, позицию не меняем
    return false;
  }
  

  return false;
}

// Инициализация I2S и DMA для DAC
void initDAC() {
  
  // Вычисляем константу для левого канала (смещение ADC)
  LEFT_CHANNEL_CONST = (int16_t)(MAX_VAL * DAC_LEFT_OFFSET_VOLTS / MAX_VOLT);
  
  // Выделяем временный стерео-буфер для I2S DMA
  stereo_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  if (!stereo_buffer) {
    return;
  }
  
  // Выделяем буфер для фрагмента
  stereo_buffer_fragment = (int16_t*)malloc(FRAGMENT_SAMPLES * sizeof(int16_t));
  if (!stereo_buffer_fragment) {
    return;
  }
  
  // Сбрасываем позицию в начале
  stereo_buffer_pos = 0;
  
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
  
  // Заполняем stereo_buffer из signal_buffer
  fillStereoBuffer();
  
  // Предзаполняем DMA буферы
  prefillDMABuffers();

}

// Установить новый сигнал для DAC (замена текущего буфера)
void setSignalBuffer(int16_t* new_buffer, int num_samples) {
  if (new_buffer == NULL || num_samples != SIGNAL_SAMPLES) {
    return;
  }
  
  // Заменяем указатель (МОНО буфер - только правый канал)
  signal_buffer = new_buffer;
  
  // Пересобираем стерео-буфер с новым сигналом
  fillStereoBuffer();
  
  // Сбрасываем позицию для кольцевого доступа
  stereo_buffer_pos = 0;
  
  // Сбрасываем флаг предзаполнения, чтобы DMA перезагрузилась
  dma_prefilled = false;
  prefillDMABuffers();
  
  // Обновляем дисплей с новым пресетом
  refreshDisplay();
}

// Предзаполнение DMA буферов
void prefillDMABuffers() {
  
  // Сбрасываем позицию
  stereo_buffer_pos = 0;
  int fragments_written = 0;
  
  // Пытаемся заполнить DMA до отказа
  while (writeFragmentToDMA(pdMS_TO_TICKS(1))) {
    fragments_written++;
  }
  
  dma_prefilled = fragments_written > 0;
  
  // Запускаем сбор ADC после небольшой задержки, чтобы исключить стартовые переходные процессы
  scheduleADCCaptureStart(ADC_CAPTURE_DELAY_MS);
}

// Поддержание DMA буферов заполненными (неблокирующе!)
// Возвращает true если удалось отправить новый фрагмент
bool keepDMAFilled() {
  return writeFragmentToDMA(pdMS_TO_TICKS(1));
}

