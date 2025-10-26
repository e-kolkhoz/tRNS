// ============================================================================
// === tRNS/tACS Device for LOLIN S2 Mini (ESP32-S2) ===
// ============================================================================
// Transcranial Random Noise Stimulation / Transcranial Alternating Current Stimulation
// 
// Компоненты:
// - DAC: PCM5102A (I2S, DMA) - генерация tRNS/tACS сигнала
// - ADC: встроенный ESP32-S2 (DMA) - показометр тока
// - Потенциометр: X9C103S (цифровой, с EEPROM)
// - Связь: USB OTG с Android
//
// Архитектура:
// - Две DMA системы (DAC + ADC) работают параллельно
// - loop() свободен для USB OTG команд от Android
// - Кольцевые буферы для непрерывного сбора данных

#include "config.h"
#include "dac_control.h"
#include "adc_control.h"
#include "digital_pot.h"
#include "usb_commands.h"

// ============================================================================
// === SETUP ===
// ============================================================================

void setup() {
  // ВАЖНО: Serial используется только для отладки внутри usbLog (опционально)
  // Устройство работает и без Serial - весь вывод идёт через USB OTG протокол!
  Serial.begin(921600);
  delay(100);
  
  // Инициализируем протокол ПЕРВЫМ делом!
  initUSBProtocol();
  
  usbLog("=== tRNS/tACS Device Booting ===");
  usbLog("Hardware: LOLIN S2 Mini (ESP32-S2)");
  
  // --- Инициализация X9C103S ---
  initDigitalPot();
  
  // --- Инициализация ADC DMA ---
  // Выделяем память под кольцевой буфер ADC
  adc_ring_buffer = (int16_t*)malloc(ADC_RING_SIZE * sizeof(int16_t));
  if (!adc_ring_buffer) {
    usbError("Failed to allocate ADC ring buffer!");
    while(1);
  }
  
  // Заполняем буфер запрещенными значениями
  for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
    adc_ring_buffer[i] = ADC_INVALID_VALUE;
  }
  
  usbLogf("ADC ring buffer: %d samples (%.1f sec @ %d Hz), %d KB", 
          ADC_RING_SIZE, 
          (float)ADC_RING_SIZE / ADC_SAMPLE_RATE,
          ADC_SAMPLE_RATE,
          (ADC_RING_SIZE * sizeof(int16_t)) / 1024);
  usbLog("ADC buffer = 1× DAC loop (time-aligned for spectral analysis)");
  
  initADC();
  
  // --- Инициализация I2S DAC ---
  // Выделяем память под DAC сигнал (МОНО - только правый канал!)
  // Левый канал = константа, не хранится в памяти
  signal_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * sizeof(int16_t));
  if (!signal_buffer) {
    usbError("Failed to allocate signal buffer!");
    while(1);
  }
  
  usbLogf("Signal buffer: %d samples MONO (%.1f sec), %d KB", 
          SIGNAL_SAMPLES, (float)LOOP_DURATION_SEC,
          (SIGNAL_SAMPLES * sizeof(int16_t)) / 1024);
  
  // Генерируем ДЕМО-сигнал (синус 640 Гц) - для отладки
  // В реальной работе сигнал будет загружаться через USB OTG командой
  generateDemoSignal();
  
  // Инициализируем I2S и запускаем DMA
  initDAC();
  
  // --- Итоги ---
  // DAC: МОНО буфер + стерео temp = 32KB + 64KB = 96KB
  // ADC: 40000 samples = 80KB
  // Итого: 176KB (вместо 144KB, но зато экономия на USB!)
  int dac_memory = (SIGNAL_SAMPLES * sizeof(int16_t)) + (SIGNAL_SAMPLES * 2 * sizeof(int16_t));
  int adc_memory = ADC_RING_SIZE * sizeof(int16_t);
  int total_memory = dac_memory + adc_memory;
  usbLogf("Total memory: DAC=%dKB (mono+stereo temp), ADC=%dKB, Total=%dKB / 320KB SRAM", 
          dac_memory/1024, adc_memory/1024, total_memory/1024);
  
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
  
  // 4. Опционально: отправляем периодические обновления статуса
  // sendPeriodicStatus();
  
  // Минимальная задержка
  delay(1);
}
