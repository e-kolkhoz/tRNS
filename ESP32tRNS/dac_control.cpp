#include "dac_control.h"
#include "display_control.h"
#include "adc_control.h"
#include <math.h>

// Глобальные переменные
int16_t* signal_buffer = NULL;  // МОНО знаковый буфер (исходный сигнал)
bool dma_prefilled = false;
char current_preset_name[PRESET_NAME_MAX_LEN] = "No preset loaded";
float dynamic_dac_gain = 0.0f;  // Динамический gain для fadein/fadeout (начинаем с 0)
static float amplitude_scale = 1.0f;  // Масштаб амплитуды (0..1) для мА → DAC

// Временный стерео-буфер для отправки в I2S DMA
// Формируется из МОНО signal_buffer → СТЕРЕО [sign, magnitude]
static int16_t* stereo_buffer = NULL;
static const uint32_t STEREO_BUFFER_SIZE = SIGNAL_SAMPLES * 2;

// Позиция в stereo_buffer для кольцевого доступа (в стерео-сэмплах)
static uint32_t stereo_buffer_pos = 0;

// Буфер для фрагмента (FRAGMENT_SAMPLES стерео-сэмплов)
static int16_t* stereo_buffer_fragment = NULL;
static bool dac_active = false;

// Заполнить стерео-буфер из МОНО с амплитудным масштабом (без fade gain)
// Формирование sign-magnitude стерео для H-моста:
// Левый канал = знак (32767=положительный, 0=отрицательный)
// Правый канал = модуль * amplitude_scale (без dynamic_dac_gain)
static void fillStereoBuffer() {
  for (int i = 0; i < SIGNAL_SAMPLES; i++) {
    int16_t sample = signal_buffer[i];
    
    // Определяем знак и модуль
    bool is_positive = (sample >= 0);
    
    // Применяем инверсию полярности (если электроды перепутаны)
    #if POLARITY_INVERT
      is_positive = !is_positive;
    #endif
    
    // Левый канал = знак
    stereo_buffer[i * 2] = is_positive ? DAC_SIGN_POSITIVE : DAC_SIGN_NEGATIVE;
    
    // Правый канал = модуль * amplitude_scale (с насыщением)
    int16_t mag = (sample >= 0) ? sample : -sample;
    float scaled = mag * amplitude_scale;
    if (scaled > 32767.0f) scaled = 32767.0f;
    stereo_buffer[i * 2 + 1] = (int16_t)scaled;
  }
}

// Копировать фрагмент из stereo_buffer в stereo_buffer_fragment с кольцевым доступом
// ВАЖНО: dynamic_dac_gain применяется ТОЛЬКО на выходе (fadein/fadeout)
// ВАЖНО: i % 2 определяет L/R в выходном fragment, НЕ src_idx!
static void copyFragmentFromStereoBuffer(uint32_t start_pos) {
  float gain = dynamic_dac_gain;  // Копируем локально для консистентности
  
  // В IDLE (gain=0) выводим тишину на оба канала!
  if (gain <= 0.0f) {
    for (uint32_t i = 0; i < FRAGMENT_SAMPLES; i++) {
      stereo_buffer_fragment[i] = 0;
    }
    return;
  }
  
  // ГАРАНТИРУЕМ что start_pos чётный (начинаем с L канала)!
  start_pos = start_pos & ~1u;
  
  for (uint32_t i = 0; i < FRAGMENT_SAMPLES; i++) {
    uint32_t src_idx = (start_pos + i) % STEREO_BUFFER_SIZE;
    
    // i % 2 == 0 = левый канал (знак) - НЕ масштабируем!
    // i % 2 == 1 = правый канал (модуль) - применяем dynamic_dac_gain!
    if (i % 2 == 0) {
      // Левый канал = знак, копируем как есть (ВСЕГДА полный уровень!)
      stereo_buffer_fragment[i] = stereo_buffer[src_idx];
    } else {
      // Правый канал = модуль, применяем gain
      float scaled = stereo_buffer[src_idx] * gain;
      stereo_buffer_fragment[i] = (scaled > 32767.0f) ? 32767 : (int16_t)scaled;
    }
  }
}

// Записать подготовленный фрагмент во внутренний DMA буфер I2S
// timeout_ticks = 0 → неблокирующий; больше 0 → ждём указанное время
static bool writeFragmentToDMA(TickType_t timeout_ticks) {
  // Стартуем только с чётной позиции (L канал)
  const uint32_t start_pos = stereo_buffer_pos & ~1u;
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
    // Всегда сохраняем выравнивание по L/R
    samples_written &= ~1u;
    if (samples_written > 0) {
      stereo_buffer_pos = (start_pos + samples_written) % STEREO_BUFFER_SIZE;
    }
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

// Отладка DAC - время последнего вывода
// Поддержание DMA буферов заполненными (неблокирующе!)
// Возвращает true если удалось отправить новый фрагмент
bool keepDMAFilled() {
  static uint32_t last_call_ms = 0;
  static uint32_t last_gap_warn_ms = 0;
  
  uint32_t now = millis();
  uint32_t gap_ms = now - last_call_ms;
  
  // Алерт: если между вызовами прошло > 200мс — возможен underrun
  // При 8000Hz и буфере 3200 фреймов = 400мс, так что 200мс — половина буфера
  if (dac_active && last_call_ms > 0 && gap_ms > 200) {
    if (now - last_gap_warn_ms > 500) {  // не спамить чаще 500мс
      last_gap_warn_ms = now;
      extern uint32_t session_timer_start_ms;
      uint32_t session_sec = (session_timer_start_ms > 0) ? (now - session_timer_start_ms) / 1000 : 0;
      Serial.printf("[DAC @%lus] LOOP GAP: %lu ms! UNDERRUN RISK\n", session_sec, gap_ms);
    }
  }
  last_call_ms = now;
  
  if (!dac_active) {
    return false;
  }
  // Timeout 10ms — достаточно чтобы дождаться места в буфере, но не блокировать надолго
  // Заполняем все доступные DMA слоты, но ограничиваем число попыток
  bool result = false;
  const int kMaxWritesPerLoop = 4;
  for (int i = 0; i < kMaxWritesPerLoop; i++) {
    if (!writeFragmentToDMA(pdMS_TO_TICKS(10))) {
      break;
    }
    result = true;
  }
  
  return result;
}

// Обновить стерео-буфер после изменения signal_buffer (например, после generateSignal)
void updateStereoBuffer() {
  fillStereoBuffer();
}

void setAmplitudeScale(float scale) {
  if (scale < 0.0f) scale = 0.0f;
  if (scale > 1.0f) scale = 1.0f;
  amplitude_scale = scale;
}

void resetDacPlayback() {
  // Полный перезапуск I2S, чтобы гарантированно убрать старые данные
  i2s_stop(I2S_NUM);
  i2s_zero_dma_buffer(I2S_NUM);
  // Сбрасываем позиции и флаги
  stereo_buffer_pos = 0;
  dma_prefilled = false;
  // Запускаем I2S и заново заполняем DMA актуальным сигналом
  i2s_start(I2S_NUM);
  dac_active = true;
  prefillDMABuffers();
}

void startDacPlayback() {
  if (!dac_active) {
    i2s_start(I2S_NUM);
    dac_active = true;
  }
}

void stopDacPlayback() {
  if (dac_active) {
    i2s_stop(I2S_NUM);
    dac_active = false;
    i2s_zero_dma_buffer(I2S_NUM);
  }
}

