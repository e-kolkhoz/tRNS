#pragma once
#include <FFat.h>

// Работа с FAT-разделом "ffat" (960 KB).
// В текущей гипотезе: контроллер монтирует раздел и читает бинарные файлы из него.
// USB MSC (монтирование как флешки на ПК) — отдельный шаг, пока не реализован.
class StorageControl {
public:
    static bool begin();
    static void end();   // Размонтировать (нужно перед активацией MSC)
    static bool isReady();

    static void listFiles(const char* path = "/");
    static void printUsage();

    // Проверка существования файла
    static bool exists(const char* path);

    // Чтение бинарного файла в буфер (возвращает прочитанный размер, 0 = ошибка)
    static size_t readFile(const char* path, uint8_t* buf, size_t maxLen);

private:
    static bool _ready;
};
