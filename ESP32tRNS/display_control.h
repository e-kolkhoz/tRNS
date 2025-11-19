#ifndef DISPLAY_CONTROL_H
#define DISPLAY_CONTROL_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
// === OLED DISPLAY CONTROL (128x64, I2C) ===
// ============================================================================
// Управление OLED дисплеем для отображения метрик и статуса устройства

// Инициализация дисплея
void initDisplay();

// Обновление дисплея с текущими метриками
// Вызывать периодически в loop() (не слишком часто, чтобы не нагружать I2C)
void updateDisplay();

// Установить статус устройства (например, "Ready", "Playing", "Error")
void setDisplayStatus(const char* status);

// Принудительное обновление дисплея (для критичных изменений)
void refreshDisplay();

// Сбросить таймер отображения (отсчёт времени на экране)
void resetDisplayTimer();

// Показать экран загрузки с шагом инициализации
void showBootScreen(const char* step);

#endif // DISPLAY_CONTROL_H

