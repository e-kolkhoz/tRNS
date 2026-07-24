#pragma once
#include <stdint.h>

// LUT ADC code (0..4095) -> mA, поканально.
float adcCodeToMaL(uint16_t code);
float adcCodeToMaR(uint16_t code);
