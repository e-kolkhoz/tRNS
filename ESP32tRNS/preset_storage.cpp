#include "preset_storage.h"
#include <string.h>
#include <math.h>
#include "presets_embedded.h"

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