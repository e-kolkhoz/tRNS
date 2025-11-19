#ifndef PRESETS_EMBEDDED_H
#define PRESETS_EMBEDDED_H

#include <Arduino.h>
#include "config.h"

// Встроенные пресеты — компилируются прямо в прошивку
// Генерируются из WAV файлов с помощью скрипта

struct EmbeddedPreset {
  const char* name;
  const int16_t* samples;
  size_t sample_count;
};

// Пресет будет здесь после генерации
// Пока пустой массив
extern const int16_t PRESET_NOISE_100_640[];
extern const size_t PRESET_NOISE_100_640_SIZE;

extern const EmbeddedPreset EMBEDDED_PRESETS[];
extern const size_t EMBEDDED_PRESETS_COUNT;

#endif // PRESETS_EMBEDDED_H

