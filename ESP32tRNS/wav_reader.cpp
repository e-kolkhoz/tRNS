#include "wav_reader.h"
#include <Arduino.h>
#include <FFat.h>

namespace {

bool readExact(File& f, void* buf, size_t n) {
    return f.read((uint8_t*)buf, n) == (int)n;
}

bool matches(const char tag[4], const char* want) {
    return tag[0]==want[0] && tag[1]==want[1] && tag[2]==want[2] && tag[3]==want[3];
}

// Разбирает RIFF/WAVE + fmt + находит начало data.
// На выходе: file спозиционирован в начале data-чанка, info заполнена, dataSize — байт семплов.
bool parseHeader(File& f, WavInfo& info, uint32_t& dataSize, uint32_t expectedRate) {
    char     riff[4], wave[4];
    uint32_t riffSize;
    if (!readExact(f, riff, 4))      return false;
    if (!matches(riff, "RIFF"))      return false;
    if (!readExact(f, &riffSize, 4)) return false;
    if (!readExact(f, wave, 4))      return false;
    if (!matches(wave, "WAVE"))      return false;

    bool     haveFmt = false, haveData = false;
    uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate  = 0;
    dataSize = 0;

    while (f.available() >= 8) {
        char     id[4];
        uint32_t sz;
        if (!readExact(f, id, 4))  return false;
        if (!readExact(f, &sz, 4)) return false;

        if (matches(id, "fmt ")) {
            if (sz < 16) return false;
            if (!readExact(f, &audioFormat, 2)) return false;
            if (!readExact(f, &channels, 2))    return false;
            if (!readExact(f, &sampleRate, 4))  return false;
            f.seek(f.position() + 6);  // byteRate + blockAlign
            if (!readExact(f, &bitsPerSample, 2)) return false;
            if (sz > 16) f.seek(f.position() + (sz - 16));
            haveFmt = true;
        } else if (matches(id, "data")) {
            dataSize = sz;
            haveData = true;
            break;  // f теперь стоит на первом семпле
        } else {
            f.seek(f.position() + sz);
            if (sz & 1) f.seek(f.position() + 1);  // выравнивание
        }
    }

    if (!haveFmt || !haveData)                      return false;
    if (audioFormat != 1)                           return false;  // PCM
    if (!(channels == 1 || channels == 2))         return false;  // mono/stereo
    if (bitsPerSample != 16)                        return false;
    if (expectedRate && sampleRate != expectedRate) return false;

    info.sampleRate    = sampleRate;
    info.channels      = channels;
    info.bitsPerSample = bitsPerSample;
    info.totalSamples  = dataSize / 2;
    info.totalFrames   = (channels > 0) ? (info.totalSamples / channels) : 0;
    return true;
}

}  // namespace

bool WavReader::readFrames(const char* path,
                           int16_t* left,
                           int16_t* right,
                           size_t maxFrames,
                           size_t* outFrames,
                           WavInfo* info,
                           uint32_t expectedRate) {
    if (outFrames) *outFrames = 0;
    if (!left || !right || maxFrames == 0) return false;

    File f = FFat.open(path, "r");
    if (!f) return false;

    WavInfo tmp = {};
    uint32_t dataSize = 0;
    if (!parseHeader(f, tmp, dataSize, expectedRate)) { f.close(); return false; }

    size_t frames = (tmp.totalFrames < maxFrames) ? tmp.totalFrames : maxFrames;
    size_t got = 0;
    if (tmp.channels == 1) {
        for (size_t i = 0; i < frames; i++) {
            int16_t s = 0;
            if (f.read((uint8_t*)&s, sizeof(s)) != (int)sizeof(s)) break;
            left[i] = s;
            right[i] = s;
            got++;
        }
    } else {
        for (size_t i = 0; i < frames; i++) {
            int16_t lr[2];
            if (f.read((uint8_t*)lr, sizeof(lr)) != (int)sizeof(lr)) break;
            left[i] = lr[0];
            right[i] = lr[1];
            got++;
        }
    }
    f.close();

    if (info) *info = tmp;
    if (outFrames) *outFrames = got;
    return got > 0;
}
