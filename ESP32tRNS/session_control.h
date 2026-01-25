#ifndef SESSION_CONTROL_H
#define SESSION_CONTROL_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
// === SESSION CONTROL (управление режимами и сеансами) ===
// ============================================================================

// === РЕЖИМЫ РАБОТЫ ===
enum StimMode {
  MODE_TRNS = 0,  // Транскраниальная рандом шумовая стимуляция
  MODE_TDCS = 1,  // Транскраниальная постоянная стимуляция
  MODE_TACS = 2   // Транскраниальная переменная стимуляция
};

// === СОСТОЯНИЯ СЕАНСА ===
enum SessionState {
  STATE_IDLE = 0,     // Неактивно, DAC выключен
  STATE_FADEIN = 1,   // Плавный старт (рампа вверх)
  STATE_STABLE = 2,   // Рабочий режим (стабильная амплитуда)
  STATE_FADEOUT = 3   // Плавный стоп (рампа вниз)
};

// === НАСТРОЙКИ РЕЖИМОВ ===
struct SessionSettings {
  StimMode mode;                   // Текущий режим
  
  // Настройки режимов
  float amplitude_tDCS_mA;         // Амплитуда тока в мА
  uint16_t duration_tDCS_min;      // Продолжительность в минутах
  float amplitude_tRNS_mA;         // Амплитуда тока в мА
  uint16_t duration_tRNS_min;      // Продолжительность в минутах
  float amplitude_tACS_mA;         // Амплитуда тока в мА
  uint16_t duration_tACS_min;      // Продолжительность в минутах
  float frequency_tACS_Hz;         // Частота для tACS 
  
  // Общие настройки (калибровка)
  // ADC калибровка теперь через таблицу в adc_calibration.cpp
  float dac_code_to_mA;            // DAC: код → мА (по умолчанию 0.375)
  float fade_duration_sec;         // Длительность fadein/fadeout (секунды)
};

// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ===
extern SessionSettings current_settings;  // Текущие настройки
extern SessionState current_state;        // Текущее состояние сеанса
extern uint32_t session_elapsed_sec;      // Фактическое время сеанса (секунды)
extern uint32_t session_timer_start_ms;   // Время старта сеанса для отображения таймера

// === ФУНКЦИИ ===

// Инициализация системы сеансов
void initSession();

// Загрузить настройки из EEPROM (или дефолтные)
void loadSettings();

// Сохранить текущие настройки в EEPROM
void saveSettings();

// Сбросить настройки на заводские
void resetToDefaults();

// Сгенерировать сигнал для текущего режима
// Заполняет signal_buffer согласно режиму и настройкам
void generateSignal();

// Старт сеанса (переход в STATE_FADEIN)
void startSession();

// Остановка сеанса (переход в STATE_FADEOUT)
void stopSession();

// Обновление состояния сеанса (вызывать в loop)
// Управляет переходами между состояниями и таймерами
void updateSession();

// Получить строковое название режима
const char* getModeName(StimMode mode);

// Получить ближайшую допустимую частоту для tACS
// Возвращает частоту, период которой кратен длине пресета
float getValidTACSFrequency(float target_Hz);

// Проверка завершения сеанса (для автоперехода на SCR_FINISH)
bool isSessionJustFinished();

#endif // SESSION_CONTROL_H

