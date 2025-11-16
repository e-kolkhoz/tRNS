#ifndef PRESET_STORAGE_H
#define PRESET_STORAGE_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
// === PRESET STORAGE (SPI FLASH / SPIFFS) ===
// ============================================================================

// Инициализация хранения пресетов (монтирование SPIFFS)
bool initPresetStorage();

// Построить демонстрационный пресет (корректный loop)
// Возвращает фактическую частоту (Гц)
float buildDemoPreset(int16_t* target_buffer,
                      size_t sample_count,
                      char* preset_name_out,
                      size_t preset_name_len);

// Загрузка пресета в указанный буфер + имя пресета
bool loadPresetFromFlash(int16_t* target_buffer,
                         char* preset_name_out,
                         size_t preset_name_len);

// Сохранение пресета вместе с именем
bool savePresetToFlash(const int16_t* source_buffer,
                       const char* preset_name);

#endif  // PRESET_STORAGE_H


