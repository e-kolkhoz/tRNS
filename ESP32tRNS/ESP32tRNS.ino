// ============================================================================
// === ESP32-S3FH4R2 / Waveshare ESP32-S3-Zero ===
// ============================================================================
//
// ТЕСТ: голый minimal для проверки MSC после TinyUF2 auto-reboot.
// Нет Serial, нет EEPROM, нет FFat — только неопиксель-маячок и USBFlash.

#include "config.h"
#include "version.h"
#include "usb_flash.h"

void setup() {
    // Маячок: синий — красный, жёлтый — MSC поднят.
    rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);
    delay(500);
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);

    // Даём USB-OTG окончательно "отойти" после TinyUF2.
    delay(500);

    if (USBFlash::mount()) {
        rgbLedWrite(NEOPIXEL_PIN, 0, 0, 40);  // синий = MSC готов
    } else {
        rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);   // красный = ошибка
    }
}

void loop() {
}
