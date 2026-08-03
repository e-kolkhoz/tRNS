#pragma once
#include <stdint.h>

// Базовая LUT (flash) × MA2MA (NVS) -> runtime LUT поканально.
void adcCalibrationApplyMa2Ma(float ma2ma_l, float ma2ma_r);

float adcCodeToMaL(uint16_t code);
float adcCodeToMaR(uint16_t code);
