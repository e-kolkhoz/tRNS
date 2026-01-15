#include "menu_control.h"
#include "session_control.h"

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
      // Выбор режима: tRNS, tDCS, tACS (3 опции, индексы 0-2)
      // Инвертируем: по часовой - вверх (меньший индекс)
      menu_selected = constrain(menu_selected - delta, 0, 2);
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
  switch (current_screen) {
    case SCR_DASHBOARD:
      // Клик на дашборде - ТОЛЬКО во время сеанса!
      // Показать подтверждение остановки
      pushScreen(SCR_CONFIRM);
      menu_selected = 0;  // По умолчанию "нет"
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
      
    case SCR_EDITOR:
      // Сохранить значение и выйти
      *editor_data.value_ptr = editor_temp_value;
      saveSettings();  // Сохранить в EEPROM
      popScreen();
      break;
      
    case SCR_CONFIRM:
      if (menu_selected == 1) {
        // "Да" - остановить сеанс
        stopSession();
        // Показываем экран завершения
        stack_depth = 0;
        screen_stack[0] = SCR_FINISH;
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
      current_settings.mode = MODE_TRNS;
      pushScreen(SCR_TRNS_MENU);
      break;
    case 1:  // tDCS
      current_settings.mode = MODE_TDCS;
      pushScreen(SCR_TDCS_MENU);
      break;
    case 2:  // tACS
      current_settings.mode = MODE_TACS;
      pushScreen(SCR_TACS_MENU);
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
    // СТАРТ СЕАНСА
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
        openEditor("Амплитуда мА", &current_settings.amplitude_mA, 0.1f, 0.1f, 2.0f, false);
        break;
      case 2:  // Частота
        openEditor("Частота Гц", &current_settings.frequency_Hz, 1.0f, 0.5f, 640.0f, false);
        break;
      case 3:  // Продолжительность
        {
          static float duration_float = current_settings.duration_min;
          openEditor("Длительность мин", &duration_float, 1.0f, 1.0f, 60.0f, true);
          current_settings.duration_min = (uint16_t)duration_float;
        }
        break;
      case 4:  // Вернуться
        popScreen();
        break;
    }
  } else {
    // tRNS/tDCS меню
    switch (menu_selected) {
      case 1:  // Амплитуда
        openEditor("Амплитуда мА", &current_settings.amplitude_mA, 0.1f, 0.1f, 2.0f, false);
        break;
      case 2:  // Продолжительность
        {
          static float duration_float = current_settings.duration_min;
          openEditor("Длительность мин", &duration_float, 1.0f, 1.0f, 60.0f, true);
          current_settings.duration_min = (uint16_t)duration_float;
        }
        break;
      case 3:  // Вернуться
        popScreen();
        break;
    }
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

