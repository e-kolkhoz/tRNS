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
    if (channels != 1)                              return false;  // mono
    if (bitsPerSample != 16)                        return false;
    if (expectedRate && sampleRate != expectedRate) return false;

    info.sampleRate    = sampleRate;
    info.channels      = channels;
    info.bitsPerSample = bitsPerSample;
    info.totalSamples  = dataSize / 2;
    return true;
}

}  // namespace

bool WavReader::read(const char* path,
                     int16_t* samples,
                     size_t maxSamples,
                     size_t* outCount,
                     WavInfo* info,
                     uint32_t expectedRate) {
    if (outCount) *outCount = 0;

    File f = FFat.open(path, "r");
    if (!f) return false;

    WavInfo  tmp = {};
    uint32_t dataSize = 0;
    if (!parseHeader(f, tmp, dataSize, expectedRate)) { f.close(); return false; }

    size_t toRead = (tmp.totalSamples < maxSamples) ? tmp.totalSamples : maxSamples;
    size_t got    = f.read((uint8_t*)samples, toRead * 2) / 2;
    f.close();

    if (info) *info = tmp;
    if (outCount) *outCount = got;
    return got > 0;
}

bool WavReader::probe(const char* path, WavInfo* info, uint32_t expectedRate) {
    File f = FFat.open(path, "r");
    if (!f) return false;

    WavInfo  tmp = {};
    uint32_t dataSize = 0;
    bool ok = parseHeader(f, tmp, dataSize, expectedRate);
    f.close();

    if (ok && info) *info = tmp;
    return ok;
}
