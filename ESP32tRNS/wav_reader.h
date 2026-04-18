#pragma once
#include <stdint.h>
#include <stddef.h>

// Простой ридер WAV: 16-bit PCM, mono, заданная частота дискретизации.
// Возвращает true только если файл валиден и формат совпал.
// Читает до maxSamples семплов. Фактическое количество — в *outCount.
struct WavInfo {
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint32_t totalSamples;  // всего семплов в файле
};

class WavReader {
public:
    // Прочитать WAV и скопировать семплы. FFat должен быть уже смонтирован.
    // expectedRate: 0 = не проверять; иначе обязан совпадать.
    static bool read(const char* path,
                     int16_t* samples,
                     size_t maxSamples,
                     size_t* outCount,
                     WavInfo* info,
                     uint32_t expectedRate = 0);

    // Только заголовок — быстрая проверка что файл валидный WAV нужного формата.
    // Семплы не читает.
    static bool probe(const char* path,
                      WavInfo* info,
                      uint32_t expectedRate = 0);
};
