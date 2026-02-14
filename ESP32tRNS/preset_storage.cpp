#include "preset_storage.h"
#include <string.h>
#include <math.h>
#include "presets_embedded.h"

bool loadPresetFromFlash(int16_t* target_buffer,
                         char* preset_name_out,
                         size_t preset_name_len) {
  // Загружаем первый встроенный пресет (декодируем из base64)
  if (EMBEDDED_PRESETS_COUNT == 0) {
    Serial.println("[PRESET] No embedded presets");
    return false;
  }
  
  const EmbeddedPreset* preset = &EMBEDDED_PRESETS[0];
  
  // Декодируем base64 прямо в target_buffer
  size_t decoded_samples = decodePresetToBuffer(target_buffer, SIGNAL_SAMPLES);
  
  if (decoded_samples == 0) {
    Serial.println("[PRESET] Decode failed");
    return false;
  }
  
  if (decoded_samples != preset->sample_count) {
    Serial.printf("[PRESET] Sample count mismatch: got %zu, expected %zu\n", 
                  decoded_samples, preset->sample_count);
    // Продолжаем, если хотя бы что-то декодировалось
  }
  
  if (preset_name_out && preset_name_len > 0) {
    strncpy(preset_name_out, preset->name, preset_name_len);
    preset_name_out[preset_name_len - 1] = '\0';
  }
  
  Serial.printf("[PRESET] Loaded '%s' (%zu samples)\n", preset->name, decoded_samples);
  return true;
}