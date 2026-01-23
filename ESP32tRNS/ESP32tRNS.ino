// ============================================================================
// === tRNS/tACS Device for LOLIN S2 Mini (ESP32-S2) ===
// ============================================================================
// Transcranial Random Noise Stimulation / Transcranial Alternating Current Stimulation
//
// Компоненты:
// - DAC: PCM5102A (I2S, DMA) - генерация tRNS/tACS сигнала
// - ADC: встроенный ESP32-S2 (DMA) - показометр тока
// - Потенциометр: ручной (аналоговый)
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
#include "display_control.h"
#include "preset_storage.h"
#include "encoder_control.h"
#include "session_control.h"
#include "menu_control.h"

// ============================================================================
// === SETUP ===
// ============================================================================

void setup() {
  // КРИТИЧНО: Задержка перед инициализацией Serial для esptool!
  delay(1000);
  
  // Диагностика: LED для проверки старта
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  
  // Инициализируем Serial БЕЗ ожидания
  Serial.begin(921600);
  Serial.setRxBufferSize(40960);  // Увеличиваем RX буфер до 40KB (для CMD_SET_DAC)
  delay(100);
  // ============================================================
  // === BOOT SEQUENCE с отображением на OLED ===
  // ============================================================
  
  // Шаг 1: Инициализация OLED (первым делом, чтобы показывать прогресс)
  Serial.println("[BOOT] initDisplay()");
  initDisplay();
  
  // Шаг 2: Выделение памяти под ADC буфер
  showBootScreen("Allocate ADC...");
  Serial.println("[BOOT] allocate ADC ring buffer");
  adc_ring_buffer = (int16_t*)malloc(ADC_RING_SIZE * sizeof(int16_t));
  if (!adc_ring_buffer) {
    showBootScreen("ADC alloc FAIL!");
    while (1) { delay(1000); }
  }
  for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
    adc_ring_buffer[i] = ADC_INVALID_VALUE;
  }

  // Шаг 3: Инициализация ADC DMA
  showBootScreen("Init ADC DMA...");
  Serial.println("[BOOT] initADC()");
  initADC();

  // Шаг 4: Выделение памяти под DAC сигнал
  showBootScreen("Allocate DAC...");
  Serial.println("[BOOT] allocate signal buffer");
  signal_buffer = (int16_t*)malloc(SIGNAL_SAMPLES * sizeof(int16_t));
  if (!signal_buffer) {
    showBootScreen("DAC alloc FAIL!");
    while (1) { delay(1000); }
  }

  // Шаг 5: Загрузка пресета из PROGMEM (обязательно!)
  Serial.println("[BOOT] loadPresetFromFlash()");
  bool preset_loaded = loadPresetFromFlash(signal_buffer, current_preset_name, PRESET_NAME_MAX_LEN);
  if (!preset_loaded) {
    showBootScreen("ERROR: No preset!");
    while (1) { delay(1000); }  // Зависаем, без пресета работать нельзя
  }

  // Шаг 6: Инициализация I2S DAC
  showBootScreen("Init DAC DMA...");
  Serial.println("[BOOT] initDAC()");
  initDAC();
  
  // Шаг 7: Инициализация энкодера
  showBootScreen("Init Encoder...");
  Serial.println("[BOOT] initEncoder()");
  initEncoder();
  
  // Шаг 8: Инициализация настроек и меню
  showBootScreen("Init Settings...");
  Serial.println("[BOOT] initSession()");
  initSession();
  showBootScreen("Init Menu...");
  Serial.println("[BOOT] initMenu()");
  initMenu();
  // Гарантируем старт с главного меню
  stack_depth = 0;
  screen_stack[0] = SCR_MAIN_MENU;

  // Финальный экран
  showBootScreen("Starting...");
  Serial.println("[BOOT] renderCurrentScreen()");
  delay(500);
  
  // Переключаемся на главный экран меню
  renderCurrentScreen();
}

// ============================================================================
// === LOOP ===
// ============================================================================

void loop() {
  // ============================================================
  // СТРАТЕГИЯ: Неблокирующий loop для работы в реальном времени
  // - ADC DMA: читается железом, мы забираем из DMA буфера
  // - DAC DMA: гоняется железом по кругу
  // ============================================================

  // 1. Подкладываем данные в I2S DMA для DAC (неблокирующе, ~1мс)
  keepDMAFilled();

  // 2. Читаем данные из ADC DMA и складываем в кольцевой буфер
  readADCFromDMA();

  // 3. Обновление состояния сеанса (fadein/stable/fadeout)
  updateSession();
  
  // Проверка автозавершения сеанса (независимо от текущего экрана)
  if (isSessionJustFinished()) {
    // Сеанс завершился автоматически → показываем SCR_FINISH
    stack_depth = 0;
    screen_stack[0] = SCR_FINISH;
    menu_selected = 0;
  }

  // 4. Опрос энкодера (вызывает handleRotate/handleClick)
  updateEncoder();

  // 5. Обновляем OLED дисплей (неблокирующе, с ограничением частоты)
  updateDisplay();
  
  // БЕЗ DELAY - loop должен крутиться максимально быстро для отзывчивости!
}
