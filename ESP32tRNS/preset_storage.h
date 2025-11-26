#ifndef PRESET_STORAGE_H
#define PRESET_STORAGE_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
// === PRESET STORAGE (PROGMEM) ===
// ============================================================================


// Загрузка пресета в указанный буфер + имя пресета
bool loadPresetFromFlash(int16_t* target_buffer,
                         char* preset_name_out,
                         size_t preset_name_len);

#endif  // PRESET_STORAGE_H


