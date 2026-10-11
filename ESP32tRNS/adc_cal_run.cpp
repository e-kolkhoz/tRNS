#include "adc_cal_run.h"
#include "adc_control.h"
#include "dac_control.h"
#include "usb_flash.h"
#include "config.h"

#include <Arduino.h>
#include <FFat.h>
#include <esp_system.h>
#include <cmath>
#include <cstring>
#include <stdlib.h>

namespace {

constexpr uint32_t kSettleMs = 3000;
constexpr uint32_t kWindowSamples = (ADC_OUT_RATE_HZ * 2) / 10;  // 0.2 с
constexpr float kMaxDevMa = 0.30f;
constexpr int kNcodes = 4096;

// Токи, мА. Уже по возрастанию — коды ADC должны строго расти в том же порядке.
constexpr float kMa[] = {
    -3.0f, -2.0f, -1.0f,
    -0.9f, -0.8f, -0.7f, -0.6f, -0.5f, -0.4f, -0.3f, -0.2f, -0.1f,
     0.1f,  0.2f,  0.3f,  0.4f,  0.5f,  0.6f,  0.7f,  0.8f,  0.9f,
     1.0f,  2.0f,  3.0f,  4.0f,
};
constexpr int kN = (int)(sizeof(kMa) / sizeof(kMa[0]));

enum class Phase : uint8_t { Ask, Settle, Build, Save, Fail };

Phase s_phase = Phase::Ask;
int s_step = 0;
uint32_t s_t0 = 0;
uint16_t s_code_l[kN];
uint16_t s_code_r[kN];
float s_max_dev = 0.0f;
AdcCalView s_view{};

void setMsg(const char* s) {
    strncpy(s_view.msg, s, sizeof(s_view.msg) - 1);
    s_view.msg[sizeof(s_view.msg) - 1] = 0;
}

void refreshView() {
    s_view.nsteps = kN;
    s_view.step = (s_phase == Phase::Ask) ? 0 : (s_step + 1);
    if (s_view.step > kN) s_view.step = kN;
    s_view.ma = (s_step >= 0 && s_step < kN) ? kMa[s_step] : 0.0f;
    if (s_phase == Phase::Ask) s_view.ui = AdcCalUi::ASK;
    else if (s_phase == Phase::Fail) s_view.ui = AdcCalUi::FAIL;
    else s_view.ui = AdcCalUi::RUN;
}

void hardwareOff() {
    DacControl::stop();
    AdcControl::stop();
    digitalWrite(EN_WAKEUP, LOW);
}

void applyCurrent(float ma) {
    DacControl::stop();
    DacProgram p{};
    p.waveform = DacWaveform::CONST_DC;
    p.amp_l_ma = ma;
    p.amp_r_ma = ma;
    p.sample_rate_hz = DAC_SAMPLE_RATE;
    DacControl::setProgram(p);
    DacControl::setGain(1.0f);
    DacControl::start();
}

void pollLiveCodes() {
    uint16_t cl = 0, cr = 0;
    if (AdcControl::meanCodes(&cl, &cr, kWindowSamples)) {
        s_view.code_l = (int)cl;
        s_view.code_r = (int)cr;
    }
}

bool codesStrictlyRising() {
    for (int i = 1; i < kN; i++) {
        if (!(s_code_l[i] > s_code_l[i - 1])) {
            s_step = i;
            setMsg("коды L не растут");
            return false;
        }
        if (!(s_code_r[i] > s_code_r[i - 1])) {
            s_step = i;
            setMsg("коды R не растут");
            return false;
        }
    }
    return true;
}

// mA = f(code), линейно, с экстраполяцией крайним отрезком — как interp1d в ноутбуке.
float interpMa(const float* x, const float* y, int n, float xq) {
    if (xq <= x[0]) {
        const float dx = x[1] - x[0];
        return y[0] + (xq - x[0]) * (y[1] - y[0]) / dx;
    }
    if (xq >= x[n - 1]) {
        const float dx = x[n - 1] - x[n - 2];
        return y[n - 1] + (xq - x[n - 1]) * (y[n - 1] - y[n - 2]) / dx;
    }
    int lo = 0;
    int hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (x[mid] <= xq) lo = mid;
        else hi = mid;
    }
    const float dx = x[hi] - x[lo];
    const float t = (xq - x[lo]) / dx;
    return y[lo] + t * (y[hi] - y[lo]);
}

bool buildAndCheck(float* lut) {
    float x_l[kN], x_r[kN], x_m[kN];
    for (int i = 0; i < kN; i++) {
        x_l[i] = (float)s_code_l[i];
        x_r[i] = (float)s_code_r[i];
        x_m[i] = 0.5f * (x_l[i] + x_r[i]);
        if (i > 0 && !(x_m[i] > x_m[i - 1])) {
            setMsg("средние коды");
            return false;
        }
    }

    s_max_dev = 0.0f;
    for (int code = 0; code < kNcodes; code++) {
        const float xq = (float)code;
        const float ml = interpMa(x_l, kMa, kN, xq);
        const float mr = interpMa(x_r, kMa, kN, xq);
        const float mm = interpMa(x_m, kMa, kN, xq);
        const float dl = fabsf(ml - mm);
        const float dr = fabsf(mr - mm);
        if (dl > s_max_dev) s_max_dev = dl;
        if (dr > s_max_dev) s_max_dev = dr;
        lut[code] = mm;
    }
    if (s_max_dev > kMaxDevMa) {
        char b[48];
        snprintf(b, sizeof(b), "разъезд %.3f мА", s_max_dev);
        setMsg(b);
        return false;
    }
    return true;
}

bool writeLut(const float* lut) {
    USBFlash::end();
    if (!FFat.begin(false, "/ffat", 10, "ffat")) {
        setMsg("FFat не смонтирован");
        return false;
    }
    File f = FFat.open("/ADC_cal.bin", "w");
    if (!f) {
        FFat.end();
        setMsg("не открыть файл");
        return false;
    }
    const size_t nbytes = (size_t)kNcodes * sizeof(float);
    const size_t n = f.write((const uint8_t*)lut, nbytes);
    f.close();
    FFat.end();
    if (n != nbytes) {
        setMsg("запись обрезана");
        return false;
    }
    return true;
}

void failNow() {
    hardwareOff();
    s_phase = Phase::Fail;
    s_view.ui = AdcCalUi::FAIL;
    refreshView();
}

}  // namespace

void adcAutoCalEnter() {
    hardwareOff();
    s_phase = Phase::Ask;
    s_step = 0;
    s_view = {};
    s_view.code_l = -1;
    s_view.code_r = -1;
    s_view.ui = AdcCalUi::ASK;
    s_view.nsteps = kN;
    setMsg("нагрузка в L и R");
}

void adcAutoCalStart() {
    if (s_phase != Phase::Ask) return;
    s_view.code_l = -1;
    s_view.code_r = -1;
    digitalWrite(EN_WAKEUP, HIGH);
    AdcControl::start();
    if (!AdcControl::isRunning()) {
        setMsg("ADC не стартовал");
        failNow();
        return;
    }
    s_step = 0;
    applyCurrent(kMa[0]);
    s_t0 = millis();
    s_phase = Phase::Settle;
    setMsg("установка");
    refreshView();
}

void adcAutoCalAbort() {
    if (s_phase == Phase::Save) return;
    hardwareOff();
    s_phase = Phase::Ask;
}

void adcAutoCalTick() {
    if (s_phase == Phase::Settle) {
        pollLiveCodes();
        refreshView();
        if ((uint32_t)(millis() - s_t0) < kSettleMs) return;

        uint16_t cl = 0, cr = 0;
        if (!AdcControl::meanCodes(&cl, &cr, kWindowSamples)) {
            if ((uint32_t)(millis() - s_t0) > kSettleMs + 2000) {
                setMsg("нет отсчётов ADC");
                failNow();
            }
            return;
        }
        s_code_l[s_step] = cl;
        s_code_r[s_step] = cr;
        s_view.code_l = (int)cl;
        s_view.code_r = (int)cr;

        if (s_step > 0) {
            if (!(cl > s_code_l[s_step - 1])) {
                setMsg("коды L не растут");
                failNow();
                return;
            }
            if (!(cr > s_code_r[s_step - 1])) {
                setMsg("коды R не растут");
                failNow();
                return;
            }
        }

        s_step++;
        if (s_step >= kN) {
            s_phase = Phase::Build;
            setMsg("проверка");
            refreshView();
            return;
        }
        applyCurrent(kMa[s_step]);
        s_t0 = millis();
        s_view.code_l = -1;
        s_view.code_r = -1;
        setMsg("установка");
        refreshView();
        return;
    }

    if (s_phase == Phase::Build) {
        if (!codesStrictlyRising()) {
            failNow();
            return;
        }
        float* lut = (float*)malloc((size_t)kNcodes * sizeof(float));
        if (!lut) {
            setMsg("нет памяти");
            failNow();
            return;
        }
        const bool ok = buildAndCheck(lut);
        if (!ok) {
            free(lut);
            failNow();
            return;
        }
        hardwareOff();
        s_phase = Phase::Save;
        setMsg("запись");
        refreshView();
        if (!writeLut(lut)) {
            free(lut);
            USBFlash::mount();
            s_phase = Phase::Fail;
            s_view.ui = AdcCalUi::FAIL;
            refreshView();
            return;
        }
        free(lut);
        esp_restart();
        return;
    }
}

const AdcCalView& adcAutoCalView() {
    return s_view;
}
