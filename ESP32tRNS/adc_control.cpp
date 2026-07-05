#include "adc_control.h"
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

static inline float codeToVolts(uint16_t code) {
    return (float)code * ADC_MAX_VOLTAGE / 4095.0f;
}

static bool decimPush(DecimState& st, uint16_t raw, float* ring, uint32_t idx) {
    st.acc += (int32_t)raw + dither_lsb();
    if (++st.n < ADC_DECIM) return false;

    uint16_t avg = (uint16_t)(st.acc / (int32_t)ADC_DECIM);
    st.acc = 0;
    st.n   = 0;
    ring[idx] = codeToVolts(avg);
    return true;
}

static AdcChannelStats computeStats(const float* ring) {
    AdcChannelStats out{};
    uint32_t count = s_wr_idx;
    if (count > ADC_RING_SIZE) count = ADC_RING_SIZE;
    if (count < (uint32_t)(ADC_OUT_RATE_HZ / 10)) return out;

    uint32_t n = ADC_STATS_WINDOW_SAMPLES;
    if (n > count) n = count;

    uint32_t start = (s_wr_idx + ADC_RING_SIZE - n) % ADC_RING_SIZE;
    double sum = 0.0, sum2 = 0.0;
    float mn = ring[start];
    float mx = mn;

    for (uint32_t i = 0; i < n; ++i) {
        float v = ring[(start + i) % ADC_RING_SIZE];
        sum  += v;
        sum2 += (double)v * (double)v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }

    float mean = (float)(sum / (double)n);
    float var  = (float)(sum2 / (double)n - (double)mean * (double)mean);
    if (var < 0.0f) var = 0.0f;

    out.min_v  = mn;
    out.mean_v = mean;
    out.max_v  = mx;
    out.std_v  = sqrtf(var);
    out.valid  = true;
    return out;
}

static bool scopeTraceImpl(const float* ring, float* out, uint8_t width,
                           uint32_t window_samples, uint32_t start_offset) {
    uint32_t count = s_wr_idx;
    if (count > ADC_RING_SIZE) count = ADC_RING_SIZE;
    if (count < (uint32_t)(ADC_OUT_RATE_HZ / 10)) return false;  // < 100 мс данных

    if (window_samples == 0 || window_samples > count) window_samples = count;
    uint32_t decim = window_samples / width;
    if (decim < 1) decim = 1;
    uint32_t span = decim * width;
    if (span > count) span = count;

    uint32_t start = (s_wr_idx + ADC_RING_SIZE - span - start_offset) % ADC_RING_SIZE;
    for (uint8_t x = 0; x < width; x++) {
        uint32_t idx = (start + (uint32_t)x * decim) % ADC_RING_SIZE;
        out[x] = ring[idx];
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
                bool dl = decimPush(s_dec_l, raw_l, s_ring_l, idx);
                bool dr = decimPush(s_dec_r, raw_r, s_ring_r, idx);
                if (dl && dr) {
                    s_wr_idx++;
                }
                have_l = have_r = false;
            }
        }
    }

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
        s_ring_l[i] = DEF_ADC_OFFSET_L_V;
        s_ring_r[i] = DEF_ADC_OFFSET_R_V;
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

    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    if (s_adc) {
        adc_continuous_stop(s_adc);
    }
}

AdcChannelStats AdcControl::statsLeft()  { return computeStats(s_ring_l); }
AdcChannelStats AdcControl::statsRight() { return computeStats(s_ring_r); }

bool AdcControl::scopeTrace(bool left, float* out, uint8_t width,
                            uint32_t window_samples, uint32_t start_offset) {
    return scopeTraceImpl(left ? s_ring_l : s_ring_r, out, width, window_samples, start_offset);
}
