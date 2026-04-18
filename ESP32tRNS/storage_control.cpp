#include "storage_control.h"
#include <Arduino.h>

bool StorageControl::_ready = false;

bool StorageControl::begin() {
    // Партиция называется "ffat" — передаём явно, иначе FFat ищет первую fat-партицию
    if (!FFat.begin(true, "/ffat", 10, "ffat")) {
        Serial.println("[STORAGE] FFat mount FAILED (format also failed)");
        _ready = false;
        return false;
    }
    _ready = true;
    Serial.printf("[STORAGE] FFat OK — %d KB free / %d KB total\n",
        (int)(FFat.freeBytes() / 1024),
        (int)(FFat.totalBytes() / 1024));
    return true;
}

void StorageControl::end() {
    if (!_ready) return;
    FFat.end();
    _ready = false;
    Serial.println("[STORAGE] FFat unmounted");
}

bool StorageControl::isReady() {
    return _ready;
}

void StorageControl::printUsage() {
    if (!_ready) { Serial.println("[STORAGE] Not mounted"); return; }
    Serial.printf("[STORAGE] %d KB used / %d KB total\n",
        (int)((FFat.totalBytes() - FFat.freeBytes()) / 1024),
        (int)(FFat.totalBytes() / 1024));
}

void StorageControl::listFiles(const char* path) {
    if (!_ready) { Serial.println("[STORAGE] Not mounted"); return; }

    File dir = FFat.open(path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("[STORAGE] Cannot open: %s\n", path);
        return;
    }

    Serial.printf("[STORAGE] %s\n", path);
    File f = dir.openNextFile();
    while (f) {
        if (f.isDirectory()) {
            Serial.printf("  [DIR]  %s\n", f.name());
        } else {
            Serial.printf("         %-30s  %d bytes\n", f.name(), (int)f.size());
        }
        f = dir.openNextFile();
    }
}

bool StorageControl::exists(const char* path) {
    return _ready && FFat.exists(path);
}

size_t StorageControl::readFile(const char* path, uint8_t* buf, size_t maxLen) {
    if (!_ready) return 0;

    File f = FFat.open(path, "r");
    if (!f) {
        Serial.printf("[STORAGE] Cannot open: %s\n", path);
        return 0;
    }

    size_t n = f.read(buf, maxLen);
    f.close();
    Serial.printf("[STORAGE] Read %s: %d bytes\n", path, (int)n);
    return n;
}
