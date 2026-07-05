#include "dac_control.h"
#include "config.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>
#include <cmath>

namespace {

constexpr int      MAX_WAVE_LEN = (int)DAC_MAX_WAVE_SAMPLES;
constexpr int      MIN_SIN_PERIOD = 4;
constexpr float    DEFAULT_SIN_HZ = 200.0f;
constexpr float    DEFAULT_AMP_MA = 1.0f;

int16_t g_wave[MAX_WAVE_LEN * 2];
size_t g_wave_len = 0;

static int16_t maToPeakCodes(float ma) {
    int32_t c = (int32_t)lroundf(ma * DEF_DAC_CODE_TO_MA);
    if (c > 32767) c = 32767;
    if (c < 0)     c = 0;
    return (int16_t)c;
}

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void fill_const(const DacProgram& program) {
    const int16_t amp_l = maToPeakCodes(program.amp_l_ma);
    const int16_t amp_r = maToPeakCodes(program.amp_r_ma);
    g_wave_len = 1;
    g_wave[0] = amp_r; // R
    g_wave[1] = amp_l; // L
}

static float fill_sin(DacProgram& program) {
    float fs = (program.sample_rate_hz > 0) ? (float)program.sample_rate_hz : (float)DAC_SAMPLE_RATE;
    float target = program.target_freq_hz;
    if (target <= 0.0f) target = DEFAULT_SIN_HZ;
    int period = (int)lroundf(fs / target);
    if (period < MIN_SIN_PERIOD) period = MIN_SIN_PERIOD;
    if (period > MAX_WAVE_LEN) period = MAX_WAVE_LEN;

    const int16_t amp_l = maToPeakCodes(program.amp_l_ma);
    const int16_t amp_r = maToPeakCodes(program.amp_r_ma);
    g_wave_len = (size_t)period;
    for (int i = 0; i < period; i++) {
        float v = sinf(2.0f * (float)M_PI * (float)i / (float)period);
        int32_t s = (int32_t)lroundf(v * 32767.0f);
        g_wave[i * 2 + 0] = (int16_t)(((int32_t)s * amp_r) / 32768);
        g_wave[i * 2 + 1] = (int16_t)(((int32_t)s * amp_l) / 32768);
    }
    return fs / (float)period;
}

static bool fill_custom_stereo(const int16_t* left, const int16_t* right, size_t count,
                               float amp_l_ma, float amp_r_ma) {
    if (!left || !right || count == 0 || count > MAX_WAVE_LEN) return false;
    int32_t peak_l = 0;
    int32_t peak_r = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t al = left[i];
        int32_t ar = right[i];
        if (al < 0) al = -al;
        if (ar < 0) ar = -ar;
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;
    }
    if (peak_l <= 0) peak_l = 1;
    if (peak_r <= 0) peak_r = 1;

    const int16_t amp_l = maToPeakCodes(amp_l_ma);
    const int16_t amp_r = maToPeakCodes(amp_r_ma);
    g_wave_len = count;
    for (size_t i = 0; i < count; i++) {
        int32_t l = ((int32_t)left[i] * amp_l) / peak_l;
        int32_t r = ((int32_t)right[i] * amp_r) / peak_r;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        g_wave[i * 2 + 0] = (int16_t)r; // R
        g_wave[i * 2 + 1] = (int16_t)l; // L
    }
    return true;
}

static DacProgram g_program = {
    DacWaveform::SIN,
    DEFAULT_AMP_MA,
    DEFAULT_AMP_MA,
    DEFAULT_SIN_HZ,
    DEFAULT_SIN_HZ
};
static volatile float g_gain = 1.0f;

static void rebuildWave(DacProgram& program) {
    if (program.waveform == DacWaveform::CONST_DC) {
        fill_const(program);
        program.actual_freq_hz = 0.0f;
        return;
    }
    if (program.waveform == DacWaveform::CUSTOM) {
        program.actual_freq_hz = 0.0f;
        return;
    }
    program.actual_freq_hz = fill_sin(program);
}

}  // namespace

bool DacControl::s_playing = false;
i2s_port_t DacControl::s_i2s_port = I2S_NUM_0;
TaskHandle_t DacControl::s_task = nullptr;

// ИНВАРИАНТ (см. SRS R.12): во время playing параметры программы (g_program/g_wave/
// sample_rate) НЕ меняются. Меняется только g_gain (fade in/out) — это обёртка при
// заполнении буфера DAC, а не изменение параметров пресета. Поэтому playerTask читает
// общий g_wave на другом ядре без блокировок безопасно: setProgram/setCustomWaveStereo
// вызываются строго до start() и после stop().
void DacControl::playerTask(void* arg) {
    (void)arg;
    size_t k = 0;

    while (s_playing) {
        if (g_wave_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int16_t buf[256 * 2];
        const float gain = clamp01(g_gain);
        for (int j = 0; j < 256; j++) {
            int32_t r = (int32_t)lroundf((float)g_wave[k * 2 + 0] * gain);
            int32_t l = (int32_t)lroundf((float)g_wave[k * 2 + 1] * gain);
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            buf[j * 2 + 0] = (int16_t)r;
            buf[j * 2 + 1] = (int16_t)l;
            if (++k >= g_wave_len) k = 0;
        }
        size_t written = 0;
        i2s_write(s_i2s_port, buf, sizeof(buf), &written, portMAX_DELAY);
    }
    vTaskDelete(nullptr);
}

void DacControl::init() {
    rebuildWave(g_program);

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = DAC_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_driver_install(s_i2s_port, &i2s_config, 0, NULL);

    i2s_pin_config_t pin_config = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_WCLK,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    i2s_set_pin(s_i2s_port, &pin_config);
    i2s_set_clk(s_i2s_port, DAC_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(s_i2s_port);
}

void DacControl::setProgram(const DacProgram& program) {
    DacProgram p = program;
    if (p.amp_l_ma < 0.0f) p.amp_l_ma = 0.0f;
    if (p.amp_r_ma < 0.0f) p.amp_r_ma = 0.0f;
    if (p.sample_rate_hz <= 0) p.sample_rate_hz = DAC_SAMPLE_RATE;
    rebuildWave(p);
    i2s_set_clk(s_i2s_port, (uint32_t)p.sample_rate_hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    g_program = p;
}

bool DacControl::setCustomWaveStereo(const int16_t* left, const int16_t* right, size_t count,
                                     float amp_l_ma, float amp_r_ma, int sample_rate_hz) {
    DacProgram p = g_program;
    p.waveform = DacWaveform::CUSTOM;
    p.amp_l_ma = (amp_l_ma < 0.0f) ? 0.0f : amp_l_ma;
    p.amp_r_ma = (amp_r_ma < 0.0f) ? 0.0f : amp_r_ma;
    p.actual_freq_hz = 0.0f;
    p.sample_rate_hz = (sample_rate_hz > 0) ? sample_rate_hz : DAC_SAMPLE_RATE;
    if (!fill_custom_stereo(left, right, count, p.amp_l_ma, p.amp_r_ma)) {
        return false;
    }
    i2s_set_clk(s_i2s_port, (uint32_t)p.sample_rate_hz, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    g_program = p;
    return true;
}

DacProgram DacControl::program() {
    return g_program;
}

void DacControl::setGain(float gain01) {
    g_gain = clamp01(gain01);
}

float DacControl::gain() {
    return clamp01(g_gain);
}

void DacControl::start() {
    if (s_playing) return;
    s_playing = true;
    digitalWrite(EN_WAKEUP, HIGH);

    xTaskCreatePinnedToCore(playerTask, "dac_sin", 3072, NULL, 6, &s_task, 1);
}

void DacControl::stop() {
    if (!s_playing) return;
    s_playing = false;

    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    i2s_zero_dma_buffer(s_i2s_port);
    g_gain = 0.0f;
    digitalWrite(EN_WAKEUP, LOW);
}
