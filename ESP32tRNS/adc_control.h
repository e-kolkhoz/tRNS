#pragma once
#include <stdint.h>
#include <stdbool.h>

struct AdcChannelStats {
    float min_v;
    float mean_v;
    float max_v;
    float std_v;
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

    // Осциллограф (R.6/R.8): децимированная трасса окна в вольтах.
    // out[0..width-1] — окно длиной window_samples (в семплах ADC, 0 = весь буфер),
    // заканчивающееся start_offset семплов от конца. Возвращает false если данных мало.
    static bool scopeTrace(bool left, float* out, uint8_t width,
                           uint32_t window_samples, uint32_t start_offset);

private:
    static bool s_running;
    static bool s_pin_l_ok;
    static bool s_pin_r_ok;
    static bool s_configured;
};
