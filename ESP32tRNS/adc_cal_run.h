#pragma once
#include <stdint.h>

// Автокалибровка ADC: те же точки и та же усреднённая LUT, что в calib_table.ipynb.
// Порог разъезда каналов — 0.30 мА (в ноутбуке это |cal_ch - cal_mean|).

enum class AdcCalUi : uint8_t {
    ASK = 0,
    RUN,
    FAIL,
};

struct AdcCalView {
    AdcCalUi ui;
    int step;          // 1-based, 0 пока не начато
    int nsteps;
    float ma;
    int code_l;        // -1, если ещё нет отсчёта
    int code_r;
    char msg[48];
};

void adcAutoCalEnter();
void adcAutoCalStart();   // пользователь подтвердил нагрузку в оба канала
void adcAutoCalAbort();   // остановить ток и ADC
void adcAutoCalTick();
const AdcCalView& adcAutoCalView();
