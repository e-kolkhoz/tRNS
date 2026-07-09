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
    float target_freq_hz = 200.0f;     // целевая частота L (SIN)
    float target_freq_r_hz = 200.0f;   // целевая частота R (SIN, TODO #5)
    float actual_freq_hz = 200.0f;     // фактическая частота L после квантизации
    float actual_freq_r_hz = 200.0f;   // фактическая частота R
    int   period_samples_l = 0;        // целый период L в семплах (для осциллографа)
    int   period_samples_r = 0;
    int   sample_rate_hz = 8000;       // Fs тракта DAC (из пресета/WAV), см. R.7
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

    // Калибровка: кодов ЦАП на 1 мА, поканально (TODO #1). Дефолты DEF_DAC_CODE_TO_MA_L/R.
    static void setCodeToMa(float codes_per_ma_l, float codes_per_ma_r);

    static bool isPlaying() { return s_playing; }

private:
    static bool s_playing;
    static i2s_port_t s_i2s_port;
    static TaskHandle_t s_task;

    static void playerTask(void* arg);
};
