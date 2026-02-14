#ifndef ENCODER_CONTROL_H
#define ENCODER_CONTROL_H

#include <Arduino.h>
#include "config.h"
#include <EncButton.h>  // Библиотека AlexGyver (установить через Library Manager)

// ============================================================================
// === ENCODER CONTROL (тестовый опрос через EncButton) ===
// ============================================================================

// ВАЖНО: Установи библиотеку "EncButton" через Library Manager в Arduino IDE!
// Автор: AlexGyver, надёжная обработка энкодера

// Инициализация энкодера
void initEncoder();

// Опрос энкодера (вызывать в loop)
// Вызывает обработчики из menu_control
void updateEncoder();

#endif // ENCODER_CONTROL_H

