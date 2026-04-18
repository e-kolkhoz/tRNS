#pragma once
#include <Arduino.h>
#include <vector>
#include "wav_reader.h"

// Пользовательские пресеты на FFat-разделе.
// Пресет = WAV-файл требуемого формата (16-bit PCM, mono, заданная частота).
// Файлы с другим форматом или битые игнорируются.

struct PresetInfo {
    String   path;
    WavInfo  wav;
};

class CustomPresets {
public:
    // Сканирует корень FFat и возвращает список валидных пресетов.
    // FFat монтируется и размонтируется внутри — вызывать только когда USB MSC
    // ещё не захватил раздел.
    static std::vector<PresetInfo> checkAll(uint32_t expectedRate);

    // Ожидаемая частота дискретизации пресета (Гц).
    static constexpr uint32_t DEFAULT_RATE = 8000;
};
