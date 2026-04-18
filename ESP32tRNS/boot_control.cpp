#include "boot_control.h"
#include <Arduino.h>

void BootControl::init() {
    rtc_gpio_hold_dis(UF2_GPIO);
    rtc_gpio_deinit(UF2_GPIO);
}

void BootControl::rebootToUF2() {
    Serial.println("[BOOT] Entering UF2 mode...");

    const esp_partition_t* uf2 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "uf2");

    if (uf2) {
        Serial.printf("[BOOT] uf2 partition found @ 0x%06x\n", uf2->address);
        esp_err_t err = esp_ota_set_boot_partition(uf2);
        if (err == ESP_OK) {
            delay(50);
            esp_restart();
        }
        Serial.printf("[BOOT] set_boot_partition failed (%d), GPIO fallback\n", (int)err);
    } else {
        Serial.println("[BOOT] uf2 partition not found, GPIO fallback");
    }

    _rebootViaGPIOHold();
}

void BootControl::logPartitionInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();
    if (running) {
        Serial.printf("[BOOT] running: %-10s @ 0x%06x  (%d KB)\n",
            running->label, running->address, running->size / 1024);
    }
    if (boot && boot != running) {
        Serial.printf("[BOOT] boot:    %-10s @ 0x%06x  (%d KB)\n",
            boot->label, boot->address, boot->size / 1024);
    }
}

void BootControl::_rebootViaGPIOHold() {
    rtc_gpio_init(UF2_GPIO);
    rtc_gpio_set_direction(UF2_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(UF2_GPIO, 0);
    rtc_gpio_hold_en(UF2_GPIO);
    delay(50);
    esp_restart();
}
