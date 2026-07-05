#pragma once
#include <stdint.h>
#include <stddef.h>
#include <driver/i2s.h>

enum class DacWaveform : uint8_t {
    CONST_DC = 0,
    SIN = 1,
    CUSTOM = 3,
};

// Максимум семплов на канал в таблице волны (лимит RAM + валидации периода).
static constexpr size_t DAC_MAX_WAVE_SAMPLES = 4096;

struct DacProgram {
    DacWaveform waveform = DacWaveform::CONST_DC;
    float amp_l_ma = 1.0f;
    float amp_r_ma = 1.0f;
    float target_freq_hz = 200.0f;   // используется для SIN
    float actual_freq_hz = 200.0f;   // вычисляется после квантизации периода
    int   sample_rate_hz = 8000;     // Fs тракта DAC (из пресета/WAV), см. R.7
};

class DacControl {
public:
    static void init();
    static void start();
    static void stop();
    static void setProgram(const DacProgram& program);
    static bool setCustomWaveStereo(const int16_t* left, const int16_t* right, size_t count,
                                    float amp_l_ma, float amp_r_ma, int sample_rate_hz);
    static DacProgram program();
    static void setGain(float gain01);
    static float gain();

    static bool isPlaying() { return s_playing; }

private:
    static bool s_playing;
    static i2s_port_t s_i2s_port;
    static TaskHandle_t s_task;

    static void playerTask(void* arg);
};
