#pragma once
#include <stdint.h>

// Простой OLED-логгер для отладки.
// 128x64, шрифт 6x8 → 21 символ × 8 строк.
class OLED {
public:
    static bool begin();
    static bool isReady();

    // Очистить дисплей и сбросить курсор.
    static void clear();

    // Напечатать строку (с переводом строки) и сразу обновить экран.
    static void printLine(const char* fmt, ...);

    // Принудительно обновить (если использовали несколько printLine подряд
    // и хотим избежать flush'а после каждой — пока не нужно, оставлено на потом).
    static void flush();
};
