#ifndef MENU_CONTROL_H
#define MENU_CONTROL_H

#include <Arduino.h>
#include "config.h"
#include "session_control.h"

// ============================================================================
// === MENU CONTROL (навигация по экранам и меню) ===
// ============================================================================

// === ТИПЫ ЭКРАНОВ ===
enum ScreenType {
  SCR_MAIN_MENU,      // Главное меню (выбор режима) - СТАРТОВЫЙ ЭКРАН
  SCR_TRNS_MENU,      // Меню tRNS (старт/настройки)
  SCR_TDCS_MENU,      // Меню tDCS (старт/настройки)
  SCR_TACS_MENU,      // Меню tACS (старт/настройки)
  SCR_SETTINGS_MENU,  // Общие настройки (калибровка)
  SCR_EDITOR,         // Редактор параметра
  SCR_DASHBOARD,      // Дашборд (ТОЛЬКО во время сеанса!)
  SCR_CONFIRM,        // Подтверждение остановки
  SCR_FINISH          // Экран завершения сеанса
};

// === ДАННЫЕ ДЛЯ РЕДАКТОРА ===
struct EditorData {
  const char* name;       // Название параметра
  float* value_ptr;       // Указатель на значение
  float increment;        // Шаг изменения
  float min_val;          // Минимум
  float max_val;          // Максимум
  bool is_int;            // true = целое число
};

// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ===
extern ScreenType screen_stack[4];  // Стек экранов (макс 4 уровня)
extern uint8_t stack_depth;         // Текущая глубина стека
extern uint8_t menu_selected;       // Выбранный пункт меню (0-based)
extern EditorData editor_data;      // Данные редактора
extern float editor_temp_value;     // Временное значение в редакторе

// Макрос для текущего экрана
#define current_screen screen_stack[stack_depth]

// === ФУНКЦИИ НАВИГАЦИИ ===
void initMenu();                    // Инициализация (стартовый экран)
void pushScreen(ScreenType scr);    // Переход на новый экран
void popScreen();                   // Возврат назад

// === ОБРАБОТЧИКИ СОБЫТИЙ ===
void handleRotate(int8_t delta);    // Вращение энкодера
void handleClick();                 // Клик энкодера

// === ВСПОМОГАТЕЛЬНЫЕ ===
void openEditor(const char* name, float* value_ptr, float increment, float min_val, float max_val, bool is_int);
void executeMainMenuChoice();       // Выполнить выбор в главном меню
void executeSessionMenuChoice(StimMode mode);  // Выполнить выбор в меню режима

#endif // MENU_CONTROL_H

