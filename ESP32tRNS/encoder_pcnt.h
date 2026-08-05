#pragma once
#include <stdint.h>

// Аппаратный квадратурный декодер ESP32 (PCNT) для A/B энкодера.
namespace EncoderPcnt {
  void init(int pin_a, int pin_b, bool reverse);
  void setReverse(bool reverse);
  // Щелчки с прошлого вызова (1 = один пункт меню / один шаг редактора).
  int32_t readDelta();
}
