#include "menu_control.h"
#include "session_control.h"
#include "display_control.h"
#include "esp32s2/rom/rtc.h"
#include <esp_system.h>
#include <rom/rtc.h>
#include <driver/rtc_io.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

// === FORWARD DECLARATIONS ===
void executeMainMenuChoice();
void executeSessionMenuChoice(StimMode mode);
void executeSettingsMenuChoice();
void openEditor(const char* name, float* value_ptr, float increment, float min_val, float max_val, bool is_int);

// === ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ===
// Перезагрузка в TinyUF2 bootloader режим
// Согласно документации ESP TinyUF2: https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/esp_tinyuf2.html
// Для ESP32-S2 надежный способ — удержать BOOT_UF2 pin в LOW через RTC IO hold
// (обычно это GPIO0, если bootloader UF2 собран под Lolin S2 Mini)
static void setStatusAndLog(const char* msg) {
  Serial.println(msg);
}

void esp_restart_from_tinyuf2() {
  setStatusAndLog("[UF2] Restart via BOOT_UF2 pin");
  // Удерживаем BOOT_UF2 pin в LOW через RTC IO hold
  rtc_gpio_init((gpio_num_t)BOOT_UF2_GPIO);
  rtc_gpio_set_direction((gpio_num_t)BOOT_UF2_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)BOOT_UF2_GPIO, 0);
  rtc_gpio_hold_en((gpio_num_t)BOOT_UF2_GPIO);

  // Дополнительно пишем магию в RTC_SLOW_MEM (на случай если bootloader проверяет там)
  volatile uint32_t* rtc_slow_mem = (volatile uint32_t*)0x50000000;
  *rtc_slow_mem = 0xf01669ef;
  
  // Синхронизация всех записей
  __sync_synchronize();
  
  // Задержка для гарантии записи в RTC память
  delay(100);
  
  // Перезагружаем систему
  // Bootloader должен проверить RTC_CNTL_STORE0_REG или RTC_SLOW_MEM
  // и войти в режим UF2 если значение совпадает
  ESP.restart();
}

static void rebootToUF2Partition() {
  const esp_partition_t* uf2 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "uf2");
  if (!uf2) {
    setStatusAndLog("[UF2] ERROR: uf2 partition not found");
    esp_restart_from_tinyuf2();
    return;
  }

  showUF2Instructions();
  delay(200);

  Serial.printf("[UF2] Found uf2 partition: label=%s addr=0x%08lx size=%lu\n",
                uf2->label, (unsigned long)uf2->address, (unsigned long)uf2->size);
  setStatusAndLog("[UF2] Boot to uf2 partition");

  esp_err_t err = esp_ota_set_boot_partition(uf2);
  if (err != ESP_OK) {
    Serial.printf("[UF2] ERROR: set boot partition failed (%d)\n", (int)err);
    setStatusAndLog("[UF2] ERROR: set boot failed");
    esp_restart_from_tinyuf2();
    return;
  }

  ESP.restart();
}



// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ===
ScreenType screen_stack[4] = { SCR_MAIN_MENU };  // СТАРТУЕМ С ГЛАВНОГО МЕНЮ!
uint8_t stack_depth = 0;
uint8_t menu_selected = 0;
EditorData editor_data;
float editor_temp_value = 0.0f;  // Временное значение для редактора (экспортируется)

// === ИНИЦИАЛИЗАЦИЯ ===
void initMenu() {
  stack_depth = 0;
  screen_stack[0] = SCR_MAIN_MENU;  // СТАРТУЕМ С ГЛАВНОГО МЕНЮ!
  menu_selected = 0;
}

// === НАВИГАЦИЯ ===
void pushScreen(ScreenType scr) {
  if (stack_depth < 3) {
    stack_depth++;
    screen_stack[stack_depth] = scr;
    menu_selected = 0;  // Сбрасываем выбор при входе
  }
}

void popScreen() {
  if (stack_depth > 0) {
    stack_depth--;
    menu_selected = 0;
  }
}

// === ОБРАБОТЧИК ВРАЩЕНИЯ ===
void handleRotate(int8_t delta) {
  switch (current_screen) {
    case SCR_DASHBOARD:
      // На дашборде вращение игнорируется
      break;
      
    case SCR_MAIN_MENU:
      // Выбор: tRNS, tDCS, tACS, Настройки (4 опции, 0-3)
      menu_selected = constrain(menu_selected - delta, 0, 3);
      break;
      
    case SCR_TRNS_MENU:
    case SCR_TDCS_MENU:
    case SCR_TACS_MENU:
      // Меню режима: старт, амплитуда, [частота для tACS], продолжительность, вернуться
      // tRNS/tDCS: 4 опции (0-3), tACS: 5 опций (0-4)
      // Инвертируем: по часовой - вверх
      {
        uint8_t max_choice = (current_screen == SCR_TACS_MENU) ? 4 : 3;
        menu_selected = constrain(menu_selected - delta, 0, max_choice);
      }
      break;
      
    case SCR_SETTINGS_MENU:
      // Общие настройки: 10 опций (0-9)
      menu_selected = constrain(menu_selected - delta, 0, 9);
      break;
      
    case SCR_EDITOR:
      // Изменение значения (по часовой - увеличение, оставляем как есть)
      editor_temp_value += delta * editor_data.increment;
      editor_temp_value = constrain(editor_temp_value, editor_data.min_val, editor_data.max_val);
      break;
      
    case SCR_CONFIRM:
      // Выбор да/нет (2 опции)
      // Инвертируем: по часовой - вверх
      menu_selected = constrain(menu_selected - delta, 0, 1);
      break;
      
    case SCR_FINISH:
      // На экране завершения вращение игнорируется
      break;
  }
}

// === ОБРАБОТЧИК КЛИКА ===
void handleClick() {
  // ОТЛАДКА: выводим текущий экран
  Serial.printf("[CLICK] current_screen=%d, state=%d\n", current_screen, current_state);
  
  switch (current_screen) {
    case SCR_DASHBOARD:
      // Клик на дашборде - ТОЛЬКО во время сеанса!
      if (current_state != STATE_IDLE) {
        // Показать подтверждение остановки
        Serial.println("[CLICK] Dashboard -> SCR_CONFIRM");
        pushScreen(SCR_CONFIRM);
        menu_selected = 0;  // По умолчанию "нет"
      } else {
        // Если сеанс не идет - возвращаемся в главное меню
        stack_depth = 0;
        screen_stack[0] = SCR_MAIN_MENU;
        menu_selected = 0;
      }
      break;
      
    case SCR_MAIN_MENU:
      executeMainMenuChoice();
      break;
      
    case SCR_TRNS_MENU:
      executeSessionMenuChoice(MODE_TRNS);
      break;
      
    case SCR_TDCS_MENU:
      executeSessionMenuChoice(MODE_TDCS);
      break;
      
    case SCR_TACS_MENU:
      executeSessionMenuChoice(MODE_TACS);
      break;
      
    case SCR_SETTINGS_MENU:
      executeSettingsMenuChoice();
      break;
      
    case SCR_EDITOR:
      // Сохранить значение и выйти
      *editor_data.value_ptr = editor_temp_value;
      saveSettings();  // Сохранить в EEPROM
      popScreen();
      break;
      
    case SCR_CONFIRM:
      // Если сеанс уже не идет - просто уходим в главное меню
      if (current_state == STATE_IDLE) {
        stack_depth = 0;
        screen_stack[0] = SCR_MAIN_MENU;
        menu_selected = 0;
        break;
      }
      if (menu_selected == 1) {
        // "Да, плавный стоп" - ПЕРЕВОДИМ В FADEOUT, НЕ ЗАВЕРШАЕМ!
        stopSession();  // Это переводит в STATE_FADEOUT
        // Возвращаемся на DASHBOARD чтобы показать fadeout!
        popScreen();  // Убираем SCR_CONFIRM, остаёмся на SCR_DASHBOARD
      } else {
        // "Нет" - вернуться к дашборду
        popScreen();
      }
      break;
      
    case SCR_FINISH:
      // Клик на экране завершения → главное меню
      stack_depth = 0;
      screen_stack[0] = SCR_MAIN_MENU;
      menu_selected = 0;
      break;
  }
}

// === ВЫПОЛНЕНИЕ ВЫБОРА В ГЛАВНОМ МЕНЮ ===
void executeMainMenuChoice() {
  switch (menu_selected) {
    case 0: pushScreen(SCR_TRNS_MENU); break;
    case 1: pushScreen(SCR_TDCS_MENU); break;
    case 2: pushScreen(SCR_TACS_MENU); break;
    case 3: pushScreen(SCR_SETTINGS_MENU); break;
  }
}

// === ВЫПОЛНЕНИЕ ВЫБОРА В МЕНЮ РЕЖИМА ===
void executeSessionMenuChoice(StimMode mode) {
  // Структура меню:
  // 0: старт
  // 1: амплитуда
  // 2: частота (только для tACS) / продолжительность (для tRNS/tDCS)
  // 3: продолжительность (для tACS) / вернуться (для tRNS/tDCS)
  // 4: вернуться (только для tACS)
  
  if (menu_selected == 0) {
    // СТАРТ СЕАНСА - устанавливаем режим явно!
    current_settings.mode = mode;
    startSession();
    // Переходим на DASHBOARD
    stack_depth = 0;
    screen_stack[0] = SCR_DASHBOARD;
    return;
  }
  
  if (mode == MODE_TACS) {
    // tACS меню
    switch (menu_selected) {
      case 1:  // Амплитуда
        openEditor("Амплитуда мА", &current_settings.amplitude_tACS_mA, 
                   AMPLITUDE_INCREMENT_MA, MIN_AMPLITUDE_MA, MAX_AMPLITUDE_MA, false);
        break;
      case 2:  // Частота (целое число, округление к сетке при генерации)
        openEditor("Частота Гц", &current_settings.frequency_tACS_Hz, 
                   TACS_FREQ_INCREMENT_HZ, MIN_TACS_FREQ_HZ, MAX_TACS_FREQ_HZ, true);
        break;
      case 3:  // Продолжительность
        {
          static float duration_float = current_settings.duration_tACS_min;
          openEditor("Длительность мин", &duration_float, 
                     DURATION_INCREMENT_MIN, MIN_DURATION_MIN, MAX_DURATION_MIN, true);
          current_settings.duration_tACS_min = (uint16_t)duration_float;
        }
        break;
      case 4:  // Вернуться
        popScreen();
        break;
    }
  } else if (mode == MODE_TDCS) {
    // tDCS меню
    switch (menu_selected) {
      case 1:  // Макс. ток
        openEditor("Макс. ток мА", &current_settings.amplitude_tDCS_mA, 
                   AMPLITUDE_INCREMENT_MA, MIN_AMPLITUDE_MA, MAX_AMPLITUDE_MA, false);
        break;
      case 2:  // Продолжительность
        {
          static float duration_float = current_settings.duration_tDCS_min;
          openEditor("Длительность мин", &duration_float, 
                     DURATION_INCREMENT_MIN, MIN_DURATION_MIN, MAX_DURATION_MIN, true);
          current_settings.duration_tDCS_min = (uint16_t)duration_float;
        }
        break;
      case 3:  // Вернуться
        popScreen();
        break;
    }
  } else if (mode == MODE_TRNS) {
    // tRNS меню
    switch (menu_selected) {
      case 1:  // Амплитуда
        openEditor("Амплитуда мА", &current_settings.amplitude_tRNS_mA, 
                   AMPLITUDE_INCREMENT_MA, MIN_AMPLITUDE_MA, MAX_AMPLITUDE_MA, false);
        break;
      case 2:  // Продолжительность
        {
          static float duration_float = current_settings.duration_tRNS_min;
          openEditor("Длительность мин", &duration_float, 
                     DURATION_INCREMENT_MIN, MIN_DURATION_MIN, MAX_DURATION_MIN, true);
          current_settings.duration_tRNS_min = (uint16_t)duration_float;
        }
        break;
      case 3:  // Вернуться
        popScreen();
        break;
    }
  }
}

// === ВЫПОЛНЕНИЕ ВЫБОРА В МЕНЮ ОБЩИХ НАСТРОЕК ===
void executeSettingsMenuChoice() {
  // Структура меню:
  // const char* choices[] = { "<-Назад", enc_str, pol_str, dac_str, fade_str, adc_str, trns_str, "СБРОС на заводские" };
  // 0: Назад
  // 1: Энкодер: toggle
  // 2: Полярность: toggle
  // 3: DAC коды/мА
  // 4: Плавный пуск, с
  // 5: ADC множитель (калибровка показометра)
  // 6: tRNS множитель (компенсация 3σ)
  // 7: Сбросить на заводские
  
  switch (menu_selected) {
    case 0:  // Назад
      popScreen();
      break;
    case 1:  // Энкодер: toggle
      current_settings.enc_direction_invert = !current_settings.enc_direction_invert;
      saveSettings();
      break;
    case 2:  // Полярность: toggle
      current_settings.polarity_invert = !current_settings.polarity_invert;
      saveSettings();
      break;
    case 3:  // DAC коды/мА
      openEditor("DAC_Code2mA", &current_settings.dac_code_to_mA, 
                 DAC_CODE_TO_MA_INCREMENT, MIN_DAC_CODE_TO_MA, MAX_DAC_CODE_TO_MA, false);
      break;
    case 4:  // Плавный пуск, с
      openEditor("Плавн.пуск,с", &current_settings.fade_duration_sec, 
                 FADE_DURATION_INCREMENT, MIN_FADE_DURATION_SEC, MAX_FADE_DURATION_SEC, false);
      break;
    case 5:  // ADC множитель
      openEditor("ADC mult", &current_settings.adc_multiplier, 
                 ADC_MULTIPLIER_INCREMENT, MIN_ADC_MULTIPLIER, MAX_ADC_MULTIPLIER, false);
      break;
    case 6:  // tRNS множитель
      openEditor("tRNS mult", &current_settings.trns_multiplier, 
                 TRNS_MULTIPLIER_INCREMENT, MIN_TRNS_MULTIPLIER, MAX_TRNS_MULTIPLIER, false);
      break;
    case 7:  // Сбросить на заводские
      resetToDefaults();
      popScreen();  // Вернуться в главное меню
      break;
    case 8:  // Версия (только просмотр)
      break;
    case 9:  // Обновление прошивки → TinyUF2 bootloader
      Serial.printf("[UF2] Menu update click, screen=%d, selected=%d\n", current_screen, menu_selected);
      rebootToUF2Partition();
      break;
  }
}

// === ОТКРЫТЬ РЕДАКТОР ===
void openEditor(const char* name, float* value_ptr, float increment, float min_val, float max_val, bool is_int) {
  editor_data.name = name;
  editor_data.value_ptr = value_ptr;
  editor_data.increment = increment;
  editor_data.min_val = min_val;
  editor_data.max_val = max_val;
  editor_data.is_int = is_int;
  editor_temp_value = *value_ptr;  // Локальная копия для редактирования
  pushScreen(SCR_EDITOR);
}

