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

struct AdcHwStatus {
    bool l_pin_ok;
    bool r_pin_ok;
    bool configured;
};

class AdcControl {
public:
    static void init();
    static void start();
    static void stop();
    static bool isRunning() { return s_running; }

    static AdcChannelStats statsLeft();
    static AdcChannelStats statsRight();
    static AdcHwStatus    hwStatus();

private:
    static bool s_running;
    static bool s_pin_l_ok;
    static bool s_pin_r_ok;
    static bool s_configured;
};
