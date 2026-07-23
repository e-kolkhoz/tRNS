#pragma once
#include <stdint.h>
#include <stdbool.h>

struct AdcChannelStats {
    float min_v;
    float mean_v;
    float max_v;
    float rms_v;   // RMS(Vadc - Voffset), В — энергия AC относительно калибровки
    bool  valid;
};

class AdcControl {
public:
    static void init();
    static void start(float off_l_v, float off_r_v);
    static void stop();
    static bool isRunning() { return s_running; }

    static AdcChannelStats statsLeft();
    static AdcChannelStats statsRight();

    // Осциллограф (TODO #8): децимированная трасса в вольтах, out[0..width-1].
    // period_samples — целое число семплов ADC на период; n_periods=0 -> free-run.
    static bool scopeTrace(bool left, float* out, uint8_t width,
                           uint32_t period_samples, uint8_t n_periods);

private:
    static bool s_running;
    static bool s_pin_l_ok;
    static bool s_pin_r_ok;
    static bool s_configured;
};
