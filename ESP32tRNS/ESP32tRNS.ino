// ============================================================================
// === ESP32-S3FH4R2 ===
// ============================================================================


#include <EEPROM.h>
#include "config.h"
#include "dac_control.h"
#include "adc_control.h"
#include "adc_calibration.h"
#include "display_control.h"
#include "preset_storage.h"
#include "encoder_control.h"
#include "session_control.h"
#include "menu_control.h"
#include <driver/rtc_io.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Снимаем удержание BOOT_UF2 pin после перезагрузки,
// чтобы плата нормально загружалась в приложение
static void releaseBootUfh2Hold() {
  rtc_gpio_hold_dis((gpio_num_t)BOOT_UF2_GPIO);
  rtc_gpio_deinit((gpio_num_t)BOOT_UF2_GPIO);
}

// ============================================================================
// === SETUP ===
// ============================================================================

void setup() {
  // КРИТИЧНО: Задержка перед инициализацией Serial для esptool!
  delay(1000);

  // Снимаем удержание BOOT_UF2 pin, если оно было включено
  releaseBootUfh2Hold();

  // Логи разметки/загрузки
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  if (running) {
    Serial.printf("[BOOT] running: %s addr=0x%08lx size=%lu\n",
                  running->label, (unsigned long)running->address, (unsigned long)running->size);
  }
  if (boot) {
    Serial.printf("[BOOT] boot: %s addr=0x%08lx size=%lu\n",
                  boot->label, (unsigned long)boot->address, (unsigned long)boot->size);
  }
  
  // Диагностика: LED для проверки старта
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  
  // Инициализируем Serial БЕЗ ожидания
  Serial.begin(921600);
  Serial.setRxBufferSize(40960);  // Увеличиваем RX буфер до 40KB (для CMD_SET_DAC)
  delay(100);
  
  // EEPROM.begin() ПЕРВЫМ — до любых malloc, иначе NVS может зависнуть!
  Serial.println("[BOOT] EEPROM.begin()");
  if (!EEPROM.begin(512)) {
    Serial.println("[BOOT] EEPROM FAILED!");
  } else {
    Serial.println("[BOOT] EEPROM OK");
  }
  
  // Инициализация LUT для калибровки ADC
  Serial.println("[BOOT] initADCCalibration()");
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
  
  /*
  //DEBUG: 50 отсчётов ADC в mA (раз в 2 сек во время сеанса)
  static uint32_t last_dump = 0;
  if (current_state != STATE_IDLE && millis() - last_dump > 2000) {
   Serial.println("--- ADC mA ---");
   uint32_t idx = adc_write_index;
   for (int i = 0; i < 50; i++) {
     int16_t raw = adc_ring_buffer[(idx + i) % ADC_RING_SIZE];
     if (raw != ADC_INVALID_VALUE) {
       float mA = adcSignedToMilliamps(raw);
       //Serial.println(mA, 3);
       Serial.println(raw);
     }
   }
   last_dump = millis();
  }
  */
  
  // БЕЗ DELAY - loop должен крутиться максимально быстро для отзывчивости!
}
