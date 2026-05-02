// ============================================================================
// === ESP32-S3FH4R2 / Waveshare ESP32-S3-Zero ===
// ============================================================================
//
// Цикл загрузки:
//   1. ToneGen::begin() — поднимает I2S PDM TX и фоновую writer-task,
//      сразу запускает два тестовых синуса на GPIO1 / GPIO2.
//   2. CustomPresets::checkAll() — сканирует FFat, отдаёт список WAV.
//   3. USBFlash::mount() — поднимает USB MSC, раздел виден как флешка на ПК.
//
// Neopixel (после стартового синего пульса):
//   красный   — генератор не поднялся (плохо!)
//   зелёный   — генератор ок, но MSC не поднялся
//   жёлтый    — всё ок, валидных WAV нет
//   сиреневый — всё ок, есть валидный WAV

#include "config.h"
#include "version.h"
#include "custom_presets.h"
#include "tone_gen.h"
#include "usb_flash.h"

static std::vector<PresetInfo> g_presets;

void setup() {
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 40);
    delay(300);
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);

    // USB-OTG settle после TinyUF2
    delay(1500);

    bool toneOk = ToneGen::begin(384000);
    if (toneOk) {
        ToneGen::setSineLoop(0,   640.0f, ToneGen::ampToGainQ15(1.0f));
        ToneGen::setSineLoop(1, 64000.0f, ToneGen::ampToGainQ15(1.5f));
    }

    g_presets = CustomPresets::checkAll(CustomPresets::DEFAULT_RATE);
    bool mscOk = USBFlash::mount();

    if (!toneOk) {
        rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);
    } else if (!mscOk) {
        rgbLedWrite(NEOPIXEL_PIN, 0, 40, 0);
    } else if (!g_presets.empty()) {
        rgbLedWrite(NEOPIXEL_PIN, 30, 0, 30);
    } else {
        rgbLedWrite(NEOPIXEL_PIN, 40, 25, 0);
    }
}

void loop() {
}
