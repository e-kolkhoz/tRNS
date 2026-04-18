// ============================================================================
// === ESP32-S3FH4R2  ===
// ============================================================================

#include <EEPROM.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "config.h"
#include "version.h"
#include "boot_control.h"
#include "serial_console.h"
#include "storage_control.h"

// ============================================================================
// === SETUP ===
// ============================================================================

void setup() {
    // Задержка для стабилизации USB CDC после перезагрузки
    delay(500);

    // Снимаем GPIO hold, если перед перезагрузкой был выставлен fallback-путь в UF2
    BootControl::init();

    Serial.begin(921600);
    delay(100);

    Serial.printf("\n[BOOT] Firmware %s\n", FIRMWARE_VERSION);
    BootControl::logPartitionInfo();

    // NVS (через EEPROM-обёртку)
    if (!EEPROM.begin(512)) {
        Serial.println("[BOOT] EEPROM FAILED");
    }

    // Монтируем FAT-раздел для хранения данных
    StorageControl::begin();

    Serial.println("[BOOT] Ready.");
#if SERIAL_CONSOLE
    Serial.println("[BOOT] Serial console active. Commands: UF2  RESET  INFO  LS  DF");
#endif
}

// ============================================================================
// === LOOP ===
// ============================================================================

void loop() {
#if SERIAL_CONSOLE
    SerialConsole::update();
#endif
}
