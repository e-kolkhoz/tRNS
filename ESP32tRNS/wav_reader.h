#pragma once
#include <stdint.h>
#include <stddef.h>

// Простой ридер WAV: 16-bit PCM, mono/stereo.
// Возвращает true только если файл валиден и формат совпал.
struct WavInfo {
    uint32_t sampleRate;
    uint16_t channels;
    uint16_t bitsPerSample;
    uint32_t totalSamples;  // всего семплов (interleaved для stereo)
    uint32_t totalFrames;   // frame = 1 mono sample или 1 stereo pair
};

class WavReader {
public:
    // Чтение WAV в поканальные буферы. FFat должен быть уже смонтирован.
    // Для mono right[i] = left[i].
    // maxFrames - лимит по количеству временных кадров.
    // expectedRate: 0 = не проверять; иначе обязан совпадать.
    static bool readFrames(const char* path,
                           int16_t* left,
                           int16_t* right,
                           size_t maxFrames,
                           size_t* outFrames,
                           WavInfo* info,
                           uint32_t expectedRate = 0);
};
