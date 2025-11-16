// ============================================================================
// === tRNS/tACS Device for LOLIN S2 Mini (ESP32-S2) ===
// ============================================================================
// Transcranial Random Noise Stimulation / Transcranial Alternating Current Stimulation
//
// Компоненты:
// - DAC: PCM5102A (I2S, DMA) - генерация tRNS/tACS сигнала
// - ADC: встроенный ESP32-S2 (DMA) - показометр тока
// - Потенциометр: ручной многооборотный (аналоговый)
// - OLED Display: 128x64 (I2C, SSD1306/SH1106) - отображение метрик
// - Связь: USB OTG с Android
//
// Требуемые библиотеки:
// - U8g2 (для OLED дисплея): установить через Library Manager в Arduino IDE
//
// Архитектура:
// - Две DMA системы (DAC + ADC) работают параллельно
// - loop() свободен для USB OTG команд от Android
// - Кольцевые буферы для непрерывного сбора данных

#include "config.h"
#include "dac_control.h"
#include "adc_control.h"
#include "usb_commands.h"
#include "display_control.h"
#include "preset_storage.h"

// ============================================================================
// === SETUP ===
// ============================================================================

void setup() {
  // КРИТИЧНО: Задержка перед инициализацией Serial для esptool!
  // Без этого Arduino IDE не сможет прошить без нажатия кнопок
  delay(1000);
  
  // Диагностика: LED для проверки старта (одна вспышка)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);  // Выключаем после вспышки
  
  // Инициализируем Serial БЕЗ ожидания
  Serial.begin(921600);
  delay(100);
  
  initUSBProtocol();

  usbLog("=== tRNS/tACS Device Booting ===");
  usbLog("Hardware: LOLIN S2 Mini (ESP32-S2)");

  // Шаг 2: Выделение памяти под ADC буфер
  adc_ring_buffer = (int16_t*)malloc(ADC_RING_SIZE * sizeof(int16_t));
  if (!adc_ring_buffer) {
    usbError("Failed to allocate ADC ring buffer!");
    while (1) { delay(1000); }  // Зависаем с постоянно горящим LED
  }

  for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
    adc_ring_buffer[i] = ADC_INVALID_VALUE;
  }

  usbLogf("ADC ring buffer: %d samples (%.1f sec @ %d Hz), %d KB",
          ADC_RING_SIZE,
          (float)ADC_RING_SIZE / ADC_SAMPLE_RATE,
          ADC_SAMPLE_RATE,
          (ADC_RING_SIZE * sizeof(int16_t)) / 1024);
  usbLog("ADC buffer = 1× DAC loop (time-aligned for spectral analysis)");

  // Шаг 3: Инициализация ADC DMA
  initADC();

  // Шаг 4: Выделение памяти под DAC сигнал
  signal_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * sizeof(int16_t));
  if (!signal_buffer) {
    usbError("Failed to allocate signal buffer!");
    while (1) { delay(1000); }
  }

  usbLogf("Signal buffer: %d samples MONO (%.1f sec), %d KB",
          SIGNAL_SAMPLES, (float)LOOP_DURATION_SEC,
          (SIGNAL_SAMPLES * sizeof(int16_t)) / 1024);

  // Шаг 5: Загрузка пресета из SPIFFS
  // ВРЕМЕННО ОТКЛЮЧЕНО для диагностики
  // bool preset_loaded = false;
  // if (initPresetStorage()) {
  //   preset_loaded = loadPresetFromFlash(signal_buffer,
  //                                       current_preset_name,
  //                                       PRESET_NAME_MAX_LEN);
  // }
  // if (!preset_loaded) {
  generateDemoSignal();
  // }

  // Шаг 6: Инициализация I2S DAC
  initDAC();

  // Шаг 7: Инициализация OLED дисплея
  initDisplay();
  setDisplayStatus("Ready");

  // --- Итоги ---
  // DAC: МОНО буфер + стерео temp = 32KB + 64KB = 96KB
  // ADC: 40000 samples = 80KB
  // Итого: 176KB (вместо 144KB, но зато экономия на USB!)
  int dac_memory = (SIGNAL_SAMPLES * sizeof(int16_t)) + (SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  int adc_memory = ADC_RING_SIZE * sizeof(int16_t);
  int total_memory = dac_memory + adc_memory;
  usbLogf("Total memory: DAC=%dKB (mono+stereo temp), ADC=%dKB, Total=%dKB / 320KB SRAM",
          dac_memory / 1024, adc_memory / 1024, total_memory / 1024);

  usbLog("=== Ready! ===");
}

// ============================================================================
// === LOOP ===
// ============================================================================

void loop() {
  // ============================================================
  // СТРАТЕГИЯ: Неблокирующий loop для работы в реальном времени
  // - ADC DMA: читается железом, мы забираем из DMA буфера
  // - DAC DMA: гоняется железом по кругу
  // - Loop свободен для USB OTG команд от Android!
  // ============================================================

  // 1. Подкладываем данные в I2S DMA для DAC (неблокирующе, ~1мс)
  keepDMAFilled();

  // 2. Читаем данные из ADC DMA и складываем в кольцевой буфер
  readADCFromDMA();

  // 3. Обрабатываем USB OTG команды от Android/PC (неблокирующе!)
  processUSBCommands();

  // 4. Обновляем OLED дисплей (неблокирующе, с ограничением частоты)
  updateDisplay();

  // 5. Опционально: отправляем периодические обновления статуса
  // sendPeriodicStatus();

  // Минимальная задержка
  delay(1);
}
