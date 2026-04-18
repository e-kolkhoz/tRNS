// ============================================================================
// === ESP32-S3FH4R2 / Waveshare ESP32-S3-Zero ===
// ============================================================================
//
// Цикл загрузки:
//   1. CustomPresets::checkAll() — монтирует FFat, сканирует, даёт список
//      валидных WAV-пресетов, размонтирует FFat.
//   2. USBFlash::mount() — поднимает USB MSC, раздел виден как флешка на ПК.
//
// Neopixel:
//   синий пульс — жив
//   красный     — MSC не поднялся
//   жёлтый      — MSC ок, валидных пресетов нет
//   сиреневый   — MSC ок + есть хотя бы один валидный пресет

#include "config.h"
#include "version.h"
#include "custom_presets.h"
#include "usb_flash.h"

static std::vector<PresetInfo> g_presets;

void setup() {
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 40);
    delay(300);
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);

    // Даём USB-OTG отойти после TinyUF2
    delay(1500);

    g_presets = CustomPresets::checkAll(CustomPresets::DEFAULT_RATE);

    if (!USBFlash::mount()) {
        rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);
        return;
    }

    if (!g_presets.empty()) {
        rgbLedWrite(NEOPIXEL_PIN, 30, 0, 30);
    } else {
        rgbLedWrite(NEOPIXEL_PIN, 40, 25, 0);
    }
}

void loop() {
}
