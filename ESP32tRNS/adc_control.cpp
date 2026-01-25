#include "adc_control.h"
#include "session_control.h"
#include "dac_control.h"  // для dynamic_dac_gain

// Глобальные переменные
adc_continuous_handle_t adc_handle = NULL;
int16_t* adc_ring_buffer = NULL;
volatile uint32_t adc_write_index = 0;

// Управление задержкой запуска записи ADC
static bool adc_capture_enabled = false;
static bool adc_capture_pending = false;
static uint32_t adc_capture_resume_ms = 0;

// Скользящее среднее по 3 сэмплам (сглаживание)
static int16_t ma_buffer[3] = {0, 0, 0};  // Кольцевой буфер на 3 элемента
static uint8_t ma_index = 0;              // Позиция записи
static float ma_avg = 0.0f;               // Текущее среднее
static uint32_t last_sign_warn_ms = 0;

// Вспомогательная функция для сброса буфера ADC в запрещенное значение
static void resetADCRingBufferInternal() {
  if (adc_ring_buffer) {
    for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
      adc_ring_buffer[i] = ADC_INVALID_VALUE;
    }
  }
  adc_write_index = 0;
  
  // Сбрасываем скользящее среднее
  ma_buffer[0] = ma_buffer[1] = ma_buffer[2] = 0;
  ma_index = 0;
  ma_avg = 0.0f;
}

// Применить скользящее среднее по 3 сэмплам (фильтр нижних частот)
// Формула: new_avg = old_avg + (new_sample - oldest_sample) / N
static inline int16_t applyMovingAverage(int16_t new_sample) {
  // Берём самый старый сэмпл (который сейчас будет перезаписан)
  int16_t oldest = ma_buffer[ma_index];
  
  // Обновляем среднее
  ma_avg = ma_avg + (new_sample - oldest) / 3.0f;
  
  // Записываем новое значение поверх самого старого
  ma_buffer[ma_index] = new_sample;
  
  // Двигаем индекс по кругу (0 -> 1 -> 2 -> 0 ...)
  ma_index = (ma_index + 1) % 3;
  
  // Возвращаем округлённое среднее
  return (int16_t)(ma_avg + 0.5f);
}

// Вычисление размера окна статистики в сэмплах (не больше размера буфера)
static uint32_t getStatsWindowSize() {
  uint32_t window = ADC_STATS_WINDOW_SAMPLES;
  if (window == 0 || window > ADC_RING_SIZE) {
    window = (ADC_RING_SIZE < 1) ? 1 : ADC_RING_SIZE;
  }
  return window;
}

// Сбор последних N сэмплов в dest (возвращает количество валидных значений)
static uint32_t collectRecentSamples(int16_t* dest, uint32_t max_samples) {
  if (dest == NULL || !adc_capture_enabled) {
    return 0;
  }
  
  uint32_t window = max_samples;
  if (window > ADC_RING_SIZE) {
    window = ADC_RING_SIZE;
  }
  
  uint32_t collected = 0;
  uint32_t start = (adc_write_index + ADC_RING_SIZE - window) % ADC_RING_SIZE;
  
  for (uint32_t i = 0; i < window; i++) {
    uint32_t idx = (start + i) % ADC_RING_SIZE;
    int16_t sample = adc_ring_buffer[idx];
    if (sample == ADC_INVALID_VALUE) {
      continue;
    }
    dest[collected++] = sample;
  }
  
  return collected;
}

// Callback вызывается когда DMA буфер заполнен (опционально)
static bool IRAM_ATTR adc_dma_conv_done_callback(
    adc_continuous_handle_t handle,
    const adc_continuous_evt_data_t *edata,
    void *user_data) {
  // DMA заполнил буфер - можем обработать
  // Но мы будем читать данные в loop() через adc_continuous_read()
  return false;  // false = не нужно yield из ISR
}

// Инициализация ADC в continuous mode (DMA!)
void initADC() {
  resetADCRingBufferInternal();
  
  // Конфигурация continuous mode
  adc_continuous_handle_cfg_t adc_config = {
    .max_store_buf_size = ADC_FRAME_SIZE * ADC_DMA_BUF_COUNT * SOC_ADC_DIGI_DATA_BYTES_PER_CONV,
    .conv_frame_size = ADC_FRAME_SIZE * SOC_ADC_DIGI_DATA_BYTES_PER_CONV,
  };
  
  ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &adc_handle));
  
  // Настройка паттерна: 2 канала (sign + magnitude) синхронно
  adc_digi_pattern_config_t adc_pattern[2];
  
  // Канал 0: знак (sign) - всегда 0-3.3V
  adc_pattern[0].atten = ADC_ATTEN_DB_11;
  adc_pattern[0].channel = ADC_SIGN_CHANNEL;
  adc_pattern[0].unit = ADC_UNIT;
  adc_pattern[0].bit_width = ADC_BITWIDTH;  // Из config.h (10 бит — меньше шума)
  
  // Канал 1: модуль (magnitude) - настраиваемый диапазон
  adc_pattern[1].atten = ADC_MOD_ATTEN;
  adc_pattern[1].channel = ADC_MOD_CHANNEL;
  adc_pattern[1].unit = ADC_UNIT;
  adc_pattern[1].bit_width = ADC_BITWIDTH;  // Из config.h (10 бит — меньше шума)
  
  adc_continuous_config_t dig_cfg = {
    .pattern_num = 2,  // 2 канала!
    .adc_pattern = adc_pattern,
    .sample_freq_hz = ADC_SAMPLE_RATE * 2,  // ×2 потому что 2 канала
    .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
  };
  
  ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));
  
  // Регистрируем callback (опционально)
  adc_continuous_evt_cbs_t cbs = {
    .on_conv_done = adc_dma_conv_done_callback,
  };
  ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, NULL));
  
  // Запускаем continuous mode!
  ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

}

// Чтение данных из ADC DMA и добавление в кольцевой буфер
void readADCFromDMA() {
  uint8_t dma_buffer[ADC_FRAME_SIZE * SOC_ADC_DIGI_DATA_BYTES_PER_CONV];
  uint32_t bytes_read = 0;
  uint32_t now_ms = millis();
  
  if (adc_capture_pending && now_ms >= adc_capture_resume_ms) {
    adc_capture_pending = false;
    adc_capture_enabled = true;
  }
  
  // Читаем из DMA (неблокирующе с timeout)
  esp_err_t ret = adc_continuous_read(adc_handle, dma_buffer, sizeof(dma_buffer), 
                                       &bytes_read, ADC_READ_TIMEOUT_MS);
  
  if (ret == ESP_OK && bytes_read > 0 && adc_capture_enabled) {
    // Парсим данные из DMA буфера
    // DMA возвращает данные чередуя каналы: sign, mag, sign, mag, ...
    uint32_t samples_read = bytes_read / SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    
    uint16_t sign_value = 0;
    uint16_t mag_value = 0;
    bool has_sign = false;
    bool has_mag = false;
    
    for (uint32_t i = 0; i < samples_read; i++) {
      adc_digi_output_data_t *p = (adc_digi_output_data_t*)&dma_buffer[i * SOC_ADC_DIGI_DATA_BYTES_PER_CONV];
      
      uint32_t chan_num = p->type1.channel;
      uint32_t data = p->type1.data;
      
      // Собираем пару (sign, magnitude)
      if (chan_num == ADC_SIGN_CHANNEL) {
        sign_value = data;
        has_sign = true;
      } else if (chan_num == ADC_MOD_CHANNEL) {
        mag_value = data;
        has_mag = true;
      }
      
      // Когда собрали пару - реконструируем знаковый сигнал
      if (has_sign && has_mag) {
        // Определяем знак: если sign > порога, то положительный
        bool is_positive = (sign_value > ADC_SIGN_THRESHOLD);

        // Применяем инверсию полярности (если электроды перепутаны)
        #if POLARITY_INVERT
          is_positive = !is_positive;
        #endif
        
        // Реконструируем знаковый сигнал
        int16_t signed_value = is_positive ? (int16_t)mag_value : -(int16_t)mag_value;
        
        // Применяем скользящее среднее (фильтр [1/3, 1/3, 1/3])
        int16_t filtered = applyMovingAverage(signed_value);
        
        // Записываем в кольцевой буфер
        adc_ring_buffer[adc_write_index] = filtered;
        adc_write_index = (adc_write_index + 1) % ADC_RING_SIZE;
        
        // Сбрасываем флаги для следующей пары
        has_sign = false;
        has_mag = false;
      }
    }
  } else if (ret == ESP_ERR_TIMEOUT) {
    // Timeout - нормально, данных просто нет пока
  } else {
    // ADC read error
  }
}

// Получить копию текущего состояния кольцевого буфера
// Вызывается по запросу от Android через USB OTG
void getADCRingBuffer(int16_t* output_buffer, uint32_t* current_write_pos) {
  // Сохраняем текущую позицию записи (для синхронизации)
  *current_write_pos = adc_write_index;
  
  // Копируем весь буфер (быстро, 80 KB)
  memcpy(output_buffer, adc_ring_buffer, ADC_RING_SIZE * sizeof(int16_t));
  
  // Android может:
  // 1. Проверить запрещенные значения (ADC_INVALID_VALUE) - если есть, буфер не полностью заполнен
  // 2. Использовать current_write_pos чтобы понять, откуда начинаются самые старые данные
  // 3. Буфер = 1× DAC луп (2 сек) → квадратное окно без растекания спектра!
  // 4. Сделать FFT (любой размер, не обязательно степень 2), FIR фильтрацию, decimation и т.д.
}

// Печать статистики ADC буфера (для отладки)
void printADCStats() {
  // Вычисляем статистику знаковых значений (пропускаем запрещенные)
  int32_t sum = 0;
  uint32_t valid_count = 0;
  int16_t min_val = ADC_MAX_VALUE;
  int16_t max_val = -ADC_MAX_VALUE;
  
  for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
    if (adc_ring_buffer[i] == ADC_INVALID_VALUE) continue;
    
    valid_count++;
    sum += adc_ring_buffer[i];
    if (adc_ring_buffer[i] < min_val) min_val = adc_ring_buffer[i];
    if (adc_ring_buffer[i] > max_val) max_val = adc_ring_buffer[i];
  }
  
  if (valid_count == 0) {
    return;
  }
  
  // Среднее и размах знакового сигнала
  int16_t avg = sum / valid_count;
  // Конвертация в напряжение: знаковый ADC → [-ADC_MAX_VOLTAGE, +ADC_MAX_VOLTAGE]
  float voltage_avg = (avg / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
  float voltage_min = (min_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
  float voltage_max = (max_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
}

// Получить минимальное и максимальное напряжение с ADC (в вольтах)
bool getADCMinMaxVoltage(float* min_voltage, float* max_voltage) {
  if (min_voltage == NULL || max_voltage == NULL) {
    return false;
  }
  
  uint32_t window = getStatsWindowSize();
  int16_t* temp_buffer = (int16_t*)malloc(window * sizeof(int16_t));
  if (temp_buffer == NULL) {
    return false;
  }
  
  uint32_t valid_count = collectRecentSamples(temp_buffer, window);
  if (valid_count == 0) {
    free(temp_buffer);
    *min_voltage = 0.0f;
    *max_voltage = 0.0f;
    return false;
  }
  
  int16_t min_val = temp_buffer[0];
  int16_t max_val = temp_buffer[0];
  for (uint32_t i = 1; i < valid_count; i++) {
    if (temp_buffer[i] < min_val) min_val = temp_buffer[i];
    if (temp_buffer[i] > max_val) max_val = temp_buffer[i];
  }
  
  free(temp_buffer);
  
  // Конвертация знакового ADC в напряжение
  *min_voltage = (min_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
  *max_voltage = (max_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
  return true;
}

// Вспомогательная функция для quickselect - разделение массива
static uint32_t partition(int16_t* arr, uint32_t left, uint32_t right, uint32_t pivot_index) {
  int16_t pivot_value = arr[pivot_index];
  // Перемещаем pivot в конец
  int16_t temp = arr[pivot_index];
  arr[pivot_index] = arr[right];
  arr[right] = temp;
  
  uint32_t store_index = left;
  for (uint32_t i = left; i < right; i++) {
    if (arr[i] < pivot_value) {
      temp = arr[store_index];
      arr[store_index] = arr[i];
      arr[i] = temp;
      store_index++;
    }
  }
  
  // Перемещаем pivot на правильную позицию
  temp = arr[right];
  arr[right] = arr[store_index];
  arr[store_index] = temp;
  
  return store_index;
}

// Quickselect - находит k-й наименьший элемент за O(n) в среднем
static int16_t quickselect(int16_t* arr, uint32_t left, uint32_t right, uint32_t k) {
  if (left == right) {
    return arr[left];
  }
  
  // Выбираем случайный pivot (для простоты используем средний)
  uint32_t pivot_index = left + (right - left) / 2;
  
  pivot_index = partition(arr, left, right, pivot_index);
  
  if (k == pivot_index) {
    return arr[k];
  } else if (k < pivot_index) {
    return quickselect(arr, left, pivot_index - 1, k);
  } else {
    return quickselect(arr, pivot_index + 1, right, k);
  }
}

// Получить перцентили (1%, 99%) и среднее напряжение с ADC (в вольтах)
bool getADCPercentiles(float* p1_voltage, float* p99_voltage, float* mean_voltage) {
  if (p1_voltage == NULL || p99_voltage == NULL || mean_voltage == NULL) {
    return false;
  }
  
  uint32_t window = getStatsWindowSize();
  int16_t* temp_buffer = (int16_t*)malloc(window * sizeof(int16_t));
  if (temp_buffer == NULL) {
    return false;
  }
  
  uint32_t valid_count = collectRecentSamples(temp_buffer, window);
  if (valid_count == 0) {
    free(temp_buffer);
    *p1_voltage = 0.0f;
    *p99_voltage = 0.0f;
    *mean_voltage = 0.0f;
    return false;
  }
  
  double sum = 0.0;
  for (uint32_t i = 0; i < valid_count; i++) {
    sum += temp_buffer[i];
  }
  // Конвертация знакового ADC в напряжение
  *mean_voltage = (float)((sum / valid_count / (double)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE);
  
  uint32_t p1_index = valid_count / 100;
  uint32_t p99_index = (valid_count * 99) / 100;
  if (p1_index >= valid_count) p1_index = 0;
  if (p99_index >= valid_count) p99_index = valid_count - 1;
  
  int16_t* temp_p1 = (int16_t*)malloc(valid_count * sizeof(int16_t));
  int16_t* temp_p99 = (int16_t*)malloc(valid_count * sizeof(int16_t));
  if (temp_p1 != NULL && temp_p99 != NULL) {
    memcpy(temp_p1, temp_buffer, valid_count * sizeof(int16_t));
    memcpy(temp_p99, temp_buffer, valid_count * sizeof(int16_t));
    
    int16_t p1_val = quickselect(temp_p1, 0, valid_count - 1, p1_index);
    int16_t p99_val = quickselect(temp_p99, 0, valid_count - 1, p99_index);
    
    // Конвертация знакового ADC в напряжение
    *p1_voltage = (p1_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
    *p99_voltage = (p99_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
    
    free(temp_p1);
    free(temp_p99);
  } else {
    // Недостаточно памяти: fallback на min/max
    int16_t min_val = temp_buffer[0];
    int16_t max_val = temp_buffer[0];
    for (uint32_t i = 1; i < valid_count; i++) {
      if (temp_buffer[i] < min_val) min_val = temp_buffer[i];
      if (temp_buffer[i] > max_val) max_val = temp_buffer[i];
    }
    // Конвертация знакового ADC в напряжение
    *p1_voltage = (min_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
    *p99_voltage = (max_val / (float)ADC_MAX_VALUE) * ADC_MAX_VOLTAGE;
    
    if (temp_p1 != NULL) free(temp_p1);
    if (temp_p99 != NULL) free(temp_p99);
  }
  
  free(temp_buffer);
  return true;
}

// Получить перцентили (1%, 99%) и среднее в RAW ADC кодах (знаковые!)
bool getADCPercentilesRaw(int16_t* p1_raw, int16_t* p99_raw, int16_t* mean_raw) {
  if (p1_raw == NULL || p99_raw == NULL || mean_raw == NULL) {
    return false;
  }
  
  uint32_t window = getStatsWindowSize();
  int16_t* temp_buffer = (int16_t*)malloc(window * sizeof(int16_t));
  if (temp_buffer == NULL) {
    return false;
  }
  
  uint32_t valid_count = collectRecentSamples(temp_buffer, window);
  if (valid_count == 0) {
    free(temp_buffer);
    *p1_raw = 0;
    *p99_raw = 0;
    *mean_raw = 0;
    return false;
  }
  
  // Среднее
  int32_t sum = 0;
  for (uint32_t i = 0; i < valid_count; i++) {
    sum += temp_buffer[i];
  }
  *mean_raw = (int16_t)(sum / (int32_t)valid_count);
  
  // Перцентили
  uint32_t p1_index = valid_count / 100;
  uint32_t p99_index = (valid_count * 99) / 100;
  if (p1_index >= valid_count) p1_index = 0;
  if (p99_index >= valid_count) p99_index = valid_count - 1;
  
  int16_t* temp_p1 = (int16_t*)malloc(valid_count * sizeof(int16_t));
  int16_t* temp_p99 = (int16_t*)malloc(valid_count * sizeof(int16_t));
  if (temp_p1 != NULL && temp_p99 != NULL) {
    memcpy(temp_p1, temp_buffer, valid_count * sizeof(int16_t));
    memcpy(temp_p99, temp_buffer, valid_count * sizeof(int16_t));
    
    *p1_raw = quickselect(temp_p1, 0, valid_count - 1, p1_index);
    *p99_raw = quickselect(temp_p99, 0, valid_count - 1, p99_index);
    
    free(temp_p1);
    free(temp_p99);
  } else {
    // Fallback на min/max
    int16_t min_val = temp_buffer[0];
    int16_t max_val = temp_buffer[0];
    for (uint32_t i = 1; i < valid_count; i++) {
      if (temp_buffer[i] < min_val) min_val = temp_buffer[i];
      if (temp_buffer[i] > max_val) max_val = temp_buffer[i];
    }
    *p1_raw = min_val;
    *p99_raw = max_val;
    
    if (temp_p1 != NULL) free(temp_p1);
    if (temp_p99 != NULL) free(temp_p99);
  }
  
  free(temp_buffer);
  return true;
}

// Построить гистограмму из ADC данных
bool buildADCHistogram(uint16_t* bins, uint8_t num_bins) {
  if (bins == NULL || num_bins == 0) {
    return false;
  }
  
  // Инициализируем bins
  for (uint8_t i = 0; i < num_bins; i++) {
    bins[i] = 0;
  }
  
  uint32_t window = getStatsWindowSize();
  int16_t* temp_buffer = (int16_t*)malloc(window * sizeof(int16_t));
  if (temp_buffer == NULL) {
    return false;
  }
  
  uint32_t valid_count = collectRecentSamples(temp_buffer, window);
  if (valid_count == 0) {
    free(temp_buffer);
    return false;
  }
  
  int16_t min_val = temp_buffer[0];
  int16_t max_val = temp_buffer[0];
  for (uint32_t i = 1; i < valid_count; i++) {
    if (temp_buffer[i] < min_val) min_val = temp_buffer[i];
    if (temp_buffer[i] > max_val) max_val = temp_buffer[i];
  }
  
  // Для sign-magnitude архитектуры гистограмма строится по реальным min/max
  if (min_val == max_val) {
    // Все значения одинаковые (например, tDCS) - все в средний столбец
    bins[num_bins / 2] = valid_count;
    free(temp_buffer);
    return true;  // Это валидные данные!
  }
  
  float range = max_val - min_val;
  for (uint32_t i = 0; i < valid_count; i++) {
    float normalized = (temp_buffer[i] - min_val) / range;
    uint8_t bin_index = (uint8_t)(normalized * num_bins);
    if (bin_index >= num_bins) bin_index = num_bins - 1;
    bins[bin_index]++;
  }
  
  free(temp_buffer);
  return true;
}

void scheduleADCCaptureStart(uint32_t delay_ms) {
  resetADCRingBufferInternal();
  adc_capture_enabled = false;
  adc_capture_pending = true;
  adc_capture_resume_ms = millis() + delay_ms;
}

// Вывод буфера в Serial для Arduino Plotter
void dumpADCToSerial(uint16_t decimation) {
  if (decimation == 0) decimation = 1;
  
  uint32_t write_idx = adc_write_index;  // Снимок текущей позиции
  
  // Выводим от старых к новым (по кругу от write_idx)
  for (uint32_t i = 0; i < ADC_RING_SIZE; i += decimation) {
    uint32_t idx = (write_idx + i) % ADC_RING_SIZE;
    int16_t val = adc_ring_buffer[idx];
    if (val != ADC_INVALID_VALUE) {
      Serial.println(val);
    }
  }
}

