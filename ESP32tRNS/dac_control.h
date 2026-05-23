#pragma once
#include <stdint.h>
#include <driver/i2s.h>

class DacControl {
public:
    static void init();
    static void start();
    static void stop();

    static bool isPlaying() { return s_playing; }

private:
    static bool s_playing;
    static i2s_port_t s_i2s_port;
    static TaskHandle_t s_task;

    static void playerTask(void* arg);
};
