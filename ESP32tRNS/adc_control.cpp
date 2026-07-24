#include "adc_control.h"
#include "adc_calibration.h"
#include "config.h"

#include <Arduino.h>
#include <esp_adc/adc_continuous.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

namespace {

adc_continuous_handle_t s_adc = nullptr;
TaskHandle_t            s_task = nullptr;

float    s_ring_l[ADC_RING_SIZE];
float    s_ring_r[ADC_RING_SIZE];
uint16_t s_code_l[ADC_RING_SIZE];
uint16_t s_code_r[ADC_RING_SIZE];
uint32_t s_wr_idx = 0;

adc_channel_t s_ch_l = ADC_CHANNEL_0;
adc_channel_t s_ch_r = ADC_CHANNEL_0;

struct DecimState {
    int32_t acc;
    uint8_t n;
};
DecimState s_dec_l{};
DecimState s_dec_r{};

uint32_t s_lfsr = 0xACE1u;

static inline int32_t dither_lsb() {
    s_lfsr ^= s_lfsr << 13;
    s_lfsr ^= s_lfsr >> 17;
    s_lfsr ^= s_lfsr << 5;
    return (s_lfsr & 1u) ? 1 : -1;
}

static bool decimPush(DecimState& st, uint16_t raw, bool is_left,
                      float* ring_ma, uint16_t* ring_code, uint32_t idx) {
    st.acc += (int32_t)raw + dither_lsb();
    if (++st.n < ADC_DECIM) return false;

    uint16_t avg = (uint16_t)(st.acc / (int32_t)ADC_DECIM);
    st.acc = 0;
    st.n   = 0;
    ring_code[idx] = avg;
    ring_ma[idx]   = is_left ? adcCodeToMaL(avg) : adcCodeToMaR(avg);
    return true;
}

static AdcChannelStats computeStats(const float* ring_ma, const uint16_t* ring_code) {
    AdcChannelStats out{};
    uint32_t count = s_wr_idx;
    if (count > ADC_RING_SIZE) count = ADC_RING_SIZE;
    if (count < (uint32_t)(ADC_OUT_RATE_HZ / 10)) return out;

    const uint32_t n = count;
    const uint32_t start = (s_wr_idx + ADC_RING_SIZE - n) % ADC_RING_SIZE;

    double sum = 0.0, sum2_ac = 0.0, sum_code = 0.0;
    float mn = ring_ma[start];
    float mx = mn;

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t idx = (start + i) % ADC_RING_SIZE;
        const float ma = ring_ma[idx];
        sum      += ma;
        sum_code += ring_code[idx];
        if (ma < mn) mn = ma;
        if (ma > mx) mx = ma;
    }

    const float mean = (float)(sum / (double)n);
    for (uint32_t i = 0; i < n; ++i) {
        const float d = ring_ma[(start + i) % ADC_RING_SIZE] - mean;
        sum2_ac += (double)d * (double)d;
    }

    out.min_ma  = mn;
    out.mean_ma = mean;
    out.max_ma  = mx;
    out.rms_ma  = sqrtf((float)(sum2_ac / (double)n));
    {
        const float code = (float)(sum_code / (double)n);
        if (code < 0.0f)      out.mean_code = 0;
        else if (code > 4095.0f) out.mean_code = 4095;
        else out.mean_code = (uint16_t)lroundf(code);
    }
    out.valid = true;
    return out;
}

static bool scopeTraceImpl(const float* ring_ma, float* out, uint8_t width,
                           uint32_t period_samples, uint8_t n_periods) {
    uint32_t count = s_wr_idx;
    if (count > ADC_RING_SIZE) count = ADC_RING_SIZE;
    if (count < (uint32_t)(ADC_OUT_RATE_HZ / 10)) return false;

    const uint32_t newest = s_wr_idx;
    uint32_t start_abs, span, hi;

    const uint32_t T = period_samples;
    const uint32_t m = (T >= 2) ? (newest / T) : 0;
    const bool synced = (T >= 2) && (n_periods > 0) && (m >= n_periods) &&
                        ((n_periods + 1) * T <= count);
    if (synced) {
        span      = (uint32_t)n_periods * T;
        start_abs = m * T - span;
        hi        = m * T - 1;
    } else {
        span = (T >= 2) ? count : (uint32_t)(0.2f * ADC_OUT_RATE_HZ);
        if (span > count) span = count;
        if (span < width) span = width;
        start_abs = (newest > span) ? (newest - span) : 0;
        hi        = (newest > 0) ? (newest - 1) : 0;
    }

    const uint32_t lo = (newest > count) ? (newest - count) : 0;
    for (uint8_t x = 0; x < width; x++) {
        uint32_t abs_idx = start_abs + (uint32_t)(((uint64_t)x * span) / width);
        if (abs_idx < lo) abs_idx = lo;
        if (abs_idx > hi) abs_idx = hi;
        out[x] = ring_ma[abs_idx % ADC_RING_SIZE];
    }
    return true;
}

static bool setupAdcPattern(adc_digi_pattern_config_t* pat, int gpio, adc_channel_t* out_ch) {
    adc_unit_t unit;
    if (adc_continuous_io_to_channel(gpio, &unit, out_ch) != ESP_OK) {
        return false;
    }
    if (unit != ADC_UNIT_1) return false;
    if (pat) {
        pat->atten     = ADC_MOD_ATTEN;
        pat->channel   = *out_ch;
        pat->unit      = ADC_UNIT_1;
        pat->bit_width = ADC_BITWIDTH_12;
    }
    return true;
}

void adcTask(void*) {
    delay(ADC_CAPTURE_DELAY_MS);

    uint8_t dma_buf[ADC_FRAME_SIZE * SOC_ADC_DIGI_RESULT_BYTES];
    bool have_l = false, have_r = false;
    uint16_t raw_l = 0, raw_r = 0;

    while (AdcControl::isRunning()) {
        uint32_t bytes_read = 0;
        esp_err_t err = adc_continuous_read(s_adc, dma_buf, sizeof(dma_buf),
                                            &bytes_read, ADC_READ_TIMEOUT_MS);
        if (err != ESP_OK || bytes_read == 0) continue;

        uint32_t n = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
        for (uint32_t i = 0; i < n; ++i) {
            adc_digi_output_data_t* p =
                (adc_digi_output_data_t*)&dma_buf[i * SOC_ADC_DIGI_RESULT_BYTES];

            adc_channel_t ch = (adc_channel_t)p->type2.channel;
            uint16_t data = p->type2.data;
            if (data > 4095) data &= 0x0FFF;

            if (ch == s_ch_l) {
                raw_l = data;
                have_l = true;
            } else if (ch == s_ch_r) {
                raw_r = data;
                have_r = true;
            }

            if (have_l && have_r) {
                uint32_t idx = s_wr_idx % ADC_RING_SIZE;
                bool dl = decimPush(s_dec_l, raw_l, true,  s_ring_l, s_code_l, idx);
                bool dr = decimPush(s_dec_r, raw_r, false, s_ring_r, s_code_r, idx);
                if (dl && dr) {
                    s_wr_idx++;
                }
                have_l = have_r = false;
            }
        }
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool AdcControl::s_running    = false;
bool AdcControl::s_pin_l_ok   = false;
bool AdcControl::s_pin_r_ok   = false;
bool AdcControl::s_configured = false;

void AdcControl::init() {
    if (s_adc) return;

    s_pin_l_ok = setupAdcPattern(nullptr, ADC_SENSE1, &s_ch_l);
    s_pin_r_ok = setupAdcPattern(nullptr, ADC_SENSE2, &s_ch_r);
    if (!s_pin_l_ok || !s_pin_r_ok) {
        return;
    }

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = ADC_FRAME_SIZE * ADC_DMA_BUF_COUNT * SOC_ADC_DIGI_RESULT_BYTES,
        .conv_frame_size    = ADC_FRAME_SIZE * SOC_ADC_DIGI_RESULT_BYTES,
    };
    if (adc_continuous_new_handle(&hcfg, &s_adc) != ESP_OK) {
        return;
    }

    adc_digi_pattern_config_t pattern[2]{};
    setupAdcPattern(&pattern[0], ADC_SENSE1, &s_ch_l);
    setupAdcPattern(&pattern[1], ADC_SENSE2, &s_ch_r);

    adc_continuous_config_t cfg = {
        .pattern_num     = 2,
        .adc_pattern     = pattern,
        .sample_freq_hz  = ADC_RAW_RATE_HZ * 2,
        .conv_mode       = ADC_CONV_SINGLE_UNIT_1,
        .format          = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    s_configured = (adc_continuous_config(s_adc, &cfg) == ESP_OK);
}

void AdcControl::start() {
    if (s_running || !s_adc || !s_configured) return;

    s_wr_idx = 0;
    s_dec_l = {};
    s_dec_r = {};
    for (uint32_t i = 0; i < ADC_RING_SIZE; ++i) {
        s_ring_l[i] = 0.0f;
        s_ring_r[i] = 0.0f;
        s_code_l[i] = 0;
        s_code_r[i] = 0;
    }

    if (adc_continuous_start(s_adc) != ESP_OK) {
        return;
    }

    s_running = true;
    xTaskCreatePinnedToCore(adcTask, "adc_cap", 4096, nullptr, 3, &s_task, 0);
}

void AdcControl::stop() {
    if (!s_running) return;
    s_running = false;

    for (int i = 0; i < 100 && s_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    if (s_adc) {
        adc_continuous_stop(s_adc);
    }
}

AdcChannelStats AdcControl::statsLeft() {
    return computeStats(s_ring_l, s_code_l);
}

AdcChannelStats AdcControl::statsRight() {
    return computeStats(s_ring_r, s_code_r);
}

bool AdcControl::scopeTrace(bool left, float* out, uint8_t width,
                            uint32_t period_samples, uint8_t n_periods) {
    return scopeTraceImpl(left ? s_ring_l : s_ring_r, out, width, period_samples, n_periods);
}
