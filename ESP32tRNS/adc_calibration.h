#pragma once
#include <stdint.h>
#include <Arduino.h>

// Базовая LUT (встроенная в flash или из /ADC_cal.bin) × MA2MA (NVS) → runtime LUT поканально.
void adcCalibrationApplyMa2Ma(float ma2ma_l, float ma2ma_r);

// Загрузка LUT с FFat (должен быть уже смонтирован).
// Нет файла → false, *err пустой (это не ошибка).
// Битый файл → false, *err с причиной.
// Успех → true, таблица применена сразу (с текущими MA2MA).
bool adcCalibrationLoadFile(const char* path, String* err = nullptr);

bool adcCalibrationIsFromFile();

float adcCodeToMaL(uint16_t code);
float adcCodeToMaR(uint16_t code);
