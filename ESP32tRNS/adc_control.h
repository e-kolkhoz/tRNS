#pragma once
#include <stdint.h>
#include <stdbool.h>

struct AdcChannelStats {
    float min_ma;
    float mean_ma;
    float max_ma;
    float rms_ma;          // AC RMS вокруг mean, mA
    uint16_t mean_code;    // среднее ADC 0..4095 (строка MC)
    bool  valid;
};

class AdcControl {
public:
    static void init();
    static void start();
    static void stop();
    static bool isRunning() { return s_running; }

    static AdcChannelStats statsLeft();
    static AdcChannelStats statsRight();

    // Осциллограф: трасса в mA, out[0..width-1].
    static bool scopeTrace(bool left, float* out, uint8_t width,
                           uint32_t period_samples, uint8_t n_periods);

private:
    static bool s_running;
    static bool s_pin_l_ok;
    static bool s_pin_r_ok;
    static bool s_configured;
};
