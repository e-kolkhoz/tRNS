#include "preset_storage.h"
#include <string.h>
#include <math.h>
#include "presets_embedded.h"

// Preset storage работает только в RAM
// При старте загружается встроенный пресет из PROGMEM
// Через USB можно загрузить новый пресет, он живёт до reset

float buildDemoPreset(int16_t* target_buffer,
                      size_t sample_count,
                      char* preset_name_out,
                      size_t preset_name_len) {
  if (target_buffer == NULL || sample_count == 0) {
    if (preset_name_out && preset_name_len > 0) {
      preset_name_out[0] = '\0';
    }
    return 0.0f;
  }
  
  const float desired_freq = 640.0f;
  uint32_t cycles = (uint32_t)((desired_freq * sample_count) / SAMPLE_RATE + 0.5f);
  if (cycles == 0) cycles = 1;
  
  float actual_freq = ((float)cycles * SAMPLE_RATE) / sample_count;
  int16_t amplitude = (int16_t)(MAX_VAL * DAC_RIGHT_AMPL_VOLTS / MAX_VOLT);
  
  for (size_t i = 0; i < sample_count; i++) {
    float phase = (2.0f * (float)M_PI * cycles * i) / sample_count;
    float sample = sinf(phase) * amplitude;
    if (sample > 32767.0f) sample = 32767.0f;
    if (sample < -32768.0f) sample = -32768.0f;
    target_buffer[i] = (int16_t)sample;
  }
  
  if (preset_name_out && preset_name_len > 0) {
    snprintf(preset_name_out, preset_name_len, "tACS %.2fHz demo", actual_freq);
  }
  
  return actual_freq;
}

bool initPresetStorage() {
  // Инициализация не требуется для embedded пресетов
  return true;
}

bool loadPresetFromFlash(int16_t* target_buffer,
                         char* preset_name_out,
                         size_t preset_name_len) {
  // Загружаем первый встроенный пресет из PROGMEM
  if (EMBEDDED_PRESETS_COUNT == 0) {
    return false;
  }
  
  const EmbeddedPreset* preset = &EMBEDDED_PRESETS[0];
  
  if (preset->sample_count != SIGNAL_SAMPLES) {
    return false;
  }
  
  // Копируем из PROGMEM в RAM
  memcpy_P(target_buffer, preset->samples, preset->sample_count * sizeof(int16_t));
  
  if (preset_name_out && preset_name_len > 0) {
    strncpy(preset_name_out, preset->name, preset_name_len);
    preset_name_out[preset_name_len - 1] = '\0';
  }
  
  return true;
}

bool savePresetToFlash(const int16_t* source_buffer,
                       const char* preset_name) {
  // Сохранение в flash отключено — пресет живёт только в RAM
  // После reset вернётся встроенный пресет
  return false;
}
