#pragma once

#include <Arduino.h>
#include <vector>

enum class PresetType : uint8_t {
    CONST_DC = 0,
    SIN = 1,
    WAV = 2,
};

enum class FeedbackBase : uint8_t {
    MEAN = 0,
    RMS = 1,
    AUTO_RMS = 2,
};

enum class ScopeSyncMode : uint8_t {
    NONE = 0,
    TWO_PERIODS = 1,
    ONE_PERIOD = 2,
    NO_SYNC = 3,
};

struct ParamRange {
    float min_v = 0.0f;
    float max_v = 0.0f;
    float step = 0.0f;
    float default_v = 0.0f;
    bool valid = false;
};

struct PresetDefinition {
    String file_name;
    String id;
    String name;

    PresetType type = PresetType::CONST_DC;
    bool channels_both = false;

    FeedbackBase feedback_base = FeedbackBase::MEAN;
    float feedback_coeff = 1.0f;

    ScopeSyncMode scope_sync = ScopeSyncMode::NONE;
    String wave_file;
    uint16_t wav_channels = 0;
    std::vector<int16_t> wav_left_samples;
    std::vector<int16_t> wav_right_samples;

    int sample_rate_hz = 0;

    ParamRange amplitude_mA;
    ParamRange frequency_hz;
    ParamRange fade_in_sec;
    ParamRange fade_out_sec;
    ParamRange duration_min;
};

class PresetDsl {
public:
    // Сканирует YAML-пресеты в корне FFat, валидирует и возвращает только валидные.
    // Одновременно пересоздает /errors.log с ошибками невалидных пресетов.
    static std::vector<PresetDefinition> scanAll();
};

