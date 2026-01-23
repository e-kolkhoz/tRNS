#ifndef PRESETS_EMBEDDED_H
#define PRESETS_EMBEDDED_H

#include <Arduino.h>
#include "config.h"

// Встроенные пресеты — хранятся в base64, декодируются при загрузке

struct EmbeddedPreset {
  const char* name;
  size_t sample_count;
};

// Base64 пресет (декодируется в signal_buffer при старте)
extern const char PRESET_B64[] PROGMEM;
extern const size_t PRESET_NOISE_100_640_SIZE;

// Декодировать пресет в указанный буфер
// Возвращает количество декодированных семплов
size_t decodePresetToBuffer(int16_t* buffer, size_t max_samples);

extern const EmbeddedPreset EMBEDDED_PRESETS[];
extern const size_t EMBEDDED_PRESETS_COUNT;

#endif // PRESETS_EMBEDDED_H

