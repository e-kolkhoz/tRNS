#include "adc_control.h"
#include "usb_commands.h"

// Глобальные переменные
adc_continuous_handle_t adc_handle = NULL;
int16_t* adc_ring_buffer = NULL;
volatile uint32_t adc_write_index = 0;

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
  usbLog("=== ADC Continuous Mode (DMA) Init ===");
  
  // Конфигурация continuous mode
  adc_continuous_handle_cfg_t adc_config = {
    .max_store_buf_size = ADC_FRAME_SIZE * ADC_DMA_BUF_COUNT * SOC_ADC_DIGI_DATA_BYTES_PER_CONV,
    .conv_frame_size = ADC_FRAME_SIZE * SOC_ADC_DIGI_DATA_BYTES_PER_CONV,
  };
  
  ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &adc_handle));
  
  // Настройка паттерна (какой канал читать)
  adc_digi_pattern_config_t adc_pattern = {
    .atten = ADC_ATTEN_DB_0,      // 0-1.1V для 0.3-0.7V сигнала
    .channel = ADC_CHANNEL,
    .unit = ADC_UNIT,
    .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,  // 12-bit
  };
  
  adc_continuous_config_t dig_cfg = {
    .pattern_num = 1,
    .adc_pattern = &adc_pattern,
    .sample_freq_hz = ADC_SAMPLE_RATE,
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
  
  usbLog("ADC: DMA started!");
  usbLog("ADC: 12-bit, 0-1.1V range (optimal for 0.3-0.7V)");
  usbLogf("ADC: Sample rate: %d Hz", ADC_SAMPLE_RATE);
  usbLogf("ADC: DMA buffers: %d × %d samples", ADC_DMA_BUF_COUNT, ADC_FRAME_SIZE);
  usbLog("ADC: Continuous acquisition via DMA (like I2S for DAC!)");
}

// Чтение данных из ADC DMA и добавление в кольцевой буфер
void readADCFromDMA() {
  uint8_t dma_buffer[ADC_FRAME_SIZE * SOC_ADC_DIGI_DATA_BYTES_PER_CONV];
  uint32_t bytes_read = 0;
  
  // Читаем из DMA (неблокирующе с timeout)
  esp_err_t ret = adc_continuous_read(adc_handle, dma_buffer, sizeof(dma_buffer), 
                                       &bytes_read, ADC_READ_TIMEOUT_MS);
  
  if (ret == ESP_OK && bytes_read > 0) {
    // Парсим данные из DMA буфера
    uint32_t samples_read = bytes_read / SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    
    for (uint32_t i = 0; i < samples_read; i++) {
      adc_digi_output_data_t *p = (adc_digi_output_data_t*)&dma_buffer[i * SOC_ADC_DIGI_DATA_BYTES_PER_CONV];
      
      // Извлекаем 12-битное значение
      uint32_t chan_num = p->type1.channel;
      uint32_t data = p->type1.data;
      
      // Проверяем что это наш канал
      if (chan_num == ADC_CHANNEL) {
        // Записываем в кольцевой буфер
        adc_ring_buffer[adc_write_index] = (int16_t)data;
        adc_write_index = (adc_write_index + 1) % ADC_RING_SIZE;
      }
    }
  } else if (ret == ESP_ERR_TIMEOUT) {
    // Timeout - нормально, данных просто нет пока
  } else {
    usbWarn("ADC read error");
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
  usbLog("=== ADC Ring Buffer Stats (DMA) ===");
  usbLogf("Write position: %u / %u", adc_write_index, ADC_RING_SIZE);
  
  // Вычисляем статистику (пропускаем запрещенные значения)
  uint32_t sum = 0;
  uint32_t valid_count = 0;
  int16_t min_val = 4095;
  int16_t max_val = 0;
  
  for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
    if (adc_ring_buffer[i] == ADC_INVALID_VALUE) continue;
    
    valid_count++;
    sum += adc_ring_buffer[i];
    if (adc_ring_buffer[i] < min_val) min_val = adc_ring_buffer[i];
    if (adc_ring_buffer[i] > max_val) max_val = adc_ring_buffer[i];
  }
  
  if (valid_count == 0) {
    usbLog("No valid data yet (DMA is starting...)");
    return;
  }
  
  int16_t avg = sum / valid_count;
  // Для ADC_ATTEN_DB_0: диапазон 0-1.1V
  float voltage_avg = (avg / 4095.0) * 1.1;
  float voltage_min = (min_val / 4095.0) * 1.1;
  float voltage_max = (max_val / 4095.0) * 1.1;
  
  usbLogf("Valid samples: %u / %u (%.1f%%)", 
          valid_count, ADC_RING_SIZE, 
          (valid_count * 100.0) / ADC_RING_SIZE);
  usbLogf("Average: %d (%.3fV)", avg, voltage_avg);
  usbLogf("Min: %d (%.3fV), Max: %d (%.3fV)", 
          min_val, voltage_min, max_val, voltage_max);
  usbLogf("Peak-to-peak: %.3fV", voltage_max - voltage_min);
}

