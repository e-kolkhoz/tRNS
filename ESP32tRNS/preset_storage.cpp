#include "preset_storage.h"
#include <FS.h>
#include <SPIFFS.h>
#include <string.h>
#include <math.h>
#include "usb_commands.h"

static bool preset_storage_initialized = false;
static const char* PRESET_FILE_PATH = "/preset.bin";
static const uint32_t PRESET_FILE_MAGIC = 0x54535250;  // 'PRST'
static const uint32_t PRESET_FILE_VERSION = 1;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t sample_count;
  uint32_t loop_duration_ms;
  char preset_name[PRESET_NAME_MAX_LEN];
} PresetFileHeader;

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
  // Выбираем количество целых циклов, чтобы петля замыкалась идеально
  uint32_t cycles = (uint32_t)((desired_freq * sample_count) / SAMPLE_RATE + 0.5f);
  if (cycles == 0) {
    cycles = 1;
  }
  
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

static bool ensurePresetStorageMounted() {
  if (preset_storage_initialized) {
    return true;
  }
  
  if (!SPIFFS.begin(true)) {
    usbWarn("SPIFFS mount failed (preset storage unavailable)");
    return false;
  }
  
  preset_storage_initialized = true;
  usbLog("Preset storage initialized (SPIFFS)");
  return true;
}

bool initPresetStorage() {
  return ensurePresetStorageMounted();
}

bool loadPresetFromFlash(int16_t* target_buffer,
                         char* preset_name_out,
                         size_t preset_name_len) {
  if (target_buffer == NULL || preset_name_out == NULL || preset_name_len == 0) {
    return false;
  }
  
  if (!ensurePresetStorageMounted()) {
    return false;
  }
  
  if (!SPIFFS.exists(PRESET_FILE_PATH)) {
    usbLog("Preset storage: no preset file found, using default waveform");
    return false;
  }
  
  File presetFile = SPIFFS.open(PRESET_FILE_PATH, FILE_READ);
  if (!presetFile) {
    usbWarn("Preset storage: failed to open preset file");
    return false;
  }
  
  PresetFileHeader header;
  size_t header_bytes = presetFile.readBytes((char*)&header, sizeof(header));
  if (header_bytes != sizeof(header)) {
    usbWarn("Preset storage: failed to read preset header");
    presetFile.close();
    return false;
  }
  
  if (header.magic != PRESET_FILE_MAGIC || header.version != PRESET_FILE_VERSION) {
    usbWarn("Preset storage: incompatible preset header");
    presetFile.close();
    return false;
  }
  
  if (header.sample_count != SIGNAL_SAMPLES || header.sample_rate != SAMPLE_RATE) {
    usbWarn("Preset storage: preset size mismatch, skipping");
    presetFile.close();
    return false;
  }
  
  size_t data_bytes = SIGNAL_SAMPLES * sizeof(int16_t);
  size_t read_bytes = presetFile.readBytes((char*)target_buffer, data_bytes);
  presetFile.close();
  
  if (read_bytes != data_bytes) {
    usbWarn("Preset storage: failed to read preset data");
    return false;
  }
  
  strncpy(preset_name_out, header.preset_name, preset_name_len);
  preset_name_out[preset_name_len - 1] = '\0';
  
  usbLogf("Preset loaded from flash: '%s'", preset_name_out);
  return true;
}

bool savePresetToFlash(const int16_t* source_buffer,
                       const char* preset_name) {
  if (source_buffer == NULL || preset_name == NULL) {
    return false;
  }
  
  if (!ensurePresetStorageMounted()) {
    return false;
  }
  
  File presetFile = SPIFFS.open(PRESET_FILE_PATH, FILE_WRITE);
  if (!presetFile) {
    usbWarn("Preset storage: failed to open preset file for writing");
    return false;
  }
  
  PresetFileHeader header = {};
  header.magic = PRESET_FILE_MAGIC;
  header.version = PRESET_FILE_VERSION;
  header.sample_rate = SAMPLE_RATE;
  header.sample_count = SIGNAL_SAMPLES;
  header.loop_duration_ms = LOOP_DURATION_SEC * 1000;
  strncpy(header.preset_name, preset_name, PRESET_NAME_MAX_LEN);
  header.preset_name[PRESET_NAME_MAX_LEN - 1] = '\0';
  
  size_t header_written = presetFile.write((const uint8_t*)&header, sizeof(header));
  if (header_written != sizeof(header)) {
    usbWarn("Preset storage: failed to write header");
    presetFile.close();
    return false;
  }
  
  size_t data_bytes = SIGNAL_SAMPLES * sizeof(int16_t);
  size_t data_written = presetFile.write((const uint8_t*)source_buffer, data_bytes);
  presetFile.close();
  
  if (data_written != data_bytes) {
    usbWarn("Preset storage: failed to write data");
    return false;
  }
  
  usbLogf("Preset saved to flash: '%s' (%d bytes)", preset_name, (int)data_bytes);
  return true;
}


