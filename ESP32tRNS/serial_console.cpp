#include "serial_console.h"

#if SERIAL_CONSOLE

#include <Arduino.h>
#include <esp_partition.h>
#include "boot_control.h"
#include "storage_control.h"
#include "usb_flash.h"

String SerialConsole::_buf;

void SerialConsole::update() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            _buf.trim();
            if (_buf.length() > 0) {
                processCommand(_buf);
                _buf = "";
            }
        } else {
            _buf += c;
        }
    }
}

void SerialConsole::processCommand(const String& cmd) {
    Serial.printf("> %s\n", cmd.c_str());
    if (cmd == "UF2") {
        BootControl::rebootToUF2();

    } else if (cmd == "RESET") {
        Serial.println("[CMD] Restarting...");
        delay(50);
        esp_restart();

    } else if (cmd == "INFO") {
        BootControl::logPartitionInfo();
        esp_partition_iterator_t it = esp_partition_find(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
        Serial.println("[CMD] Partition table:");
        while (it) {
            const esp_partition_t* p = esp_partition_get(it);
            Serial.printf("  %-12s  type=%d sub=0x%02x  @ 0x%06x  %d KB\n",
                p->label, p->type, p->subtype, p->address, p->size / 1024);
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);

    } else if (cmd == "LS") {
        StorageControl::listFiles("/");

    } else if (cmd == "DF") {
        StorageControl::printUsage();

    } else if (cmd == "MSC") {
        Serial.println("[CMD] MSC mode temporarily disabled.");
        // USBFlash::rebootToMSC();

    } else {
        Serial.println("[CMD] Commands: UF2  RESET  INFO  LS  DF");
    }
}

#endif // SERIAL_CONSOLE
