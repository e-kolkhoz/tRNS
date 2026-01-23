#include "menu_control.h"
#include "session_control.h"

// === FORWARD DECLARATIONS ===
void executeMainMenuChoice();
void executeSessionMenuChoice(StimMode mode);
void executeSettingsMenuChoice();
void openEditor(const char* name, float* value_ptr, float increment, float min_val, float max_val, bool is_int);

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
      // Выбор: tRNS, tDCS, tACS, Общие настройки (4 опции, 0-3)
      // Инвертируем: по часовой - вверх (меньший индекс)
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
      // Общие настройки: 5 опций (0-4)
      menu_selected = constrain(menu_selected - delta, 0, 4);
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
    case 0:  // tRNS
      pushScreen(SCR_TRNS_MENU);
      break;
    case 1:  // tDCS
      pushScreen(SCR_TDCS_MENU);
      break;
    case 2:  // tACS
      pushScreen(SCR_TACS_MENU);
      break;
    case 3:  // Общие настройки
      pushScreen(SCR_SETTINGS_MENU);
      break;
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
      case 2:  // Частота
        openEditor("Частота Гц", &current_settings.frequency_tACS_Hz, 
                   TACS_FREQ_INCREMENT_HZ, MIN_TACS_FREQ_HZ, MAX_TACS_FREQ_HZ, false);
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
  // 0: Назад
  // 1: ADC_V2mA
  // 2: DAC_Code2mA
  // 3: Плавный пуск, с
  // 4: Сбросить на заводские
  
  switch (menu_selected) {
    case 0:  // Назад
      popScreen();
      break;
    case 1:  // ADC_V2mA
      openEditor("ADC_V2mA", &current_settings.adc_v_to_mA, 
                 ADC_V_TO_MA_INCREMENT, MIN_ADC_V_TO_MA, MAX_ADC_V_TO_MA, false);
      break;
    case 2:  // DAC_Code2mA
      openEditor("DAC_Code2mA", &current_settings.dac_code_to_mA, 
                 DAC_CODE_TO_MA_INCREMENT, MIN_DAC_CODE_TO_MA, MAX_DAC_CODE_TO_MA, false);
      break;
    case 3:  // Плавный пуск, с
      openEditor("Плавн.пуск,с", &current_settings.fade_duration_sec, 
                 FADE_DURATION_INCREMENT, MIN_FADE_DURATION_SEC, MAX_FADE_DURATION_SEC, false);
      break;
    case 4:  // Сбросить на заводские
      resetToDefaults();
      popScreen();  // Вернуться в главное меню
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

