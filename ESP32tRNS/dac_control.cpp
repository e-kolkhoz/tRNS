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

// Калибровка кодов ЦАП на 1 мА, поканально (TODO #1); задаётся из UI/NVS, дефолт из config.h.
static volatile float g_code_to_ma_l = DEF_DAC_CODE_TO_MA_L;
static volatile float g_code_to_ma_r = DEF_DAC_CODE_TO_MA_R;

static int16_t maToPeakCodes(float ma, float code_to_ma) {
    int32_t c = (int32_t)lroundf(ma * code_to_ma);
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
    const int16_t amp_l = maToPeakCodes(program.amp_l_ma, g_code_to_ma_l);
    const int16_t amp_r = maToPeakCodes(program.amp_r_ma, g_code_to_ma_r);
    g_wave_len = 1;
    g_wave[0] = amp_l; // L (индекс 0 = физический левый канал, TODO #7)
    g_wave[1] = amp_r; // R
}

static int gcd_int(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return (a > 0) ? a : 1;
}

static int lcm_int(int a, int b) {
    if (a <= 0 || b <= 0) return 1;
    return (a / gcd_int(a, b)) * b;
}

static void fill_sin(DacProgram& program) {
    float fs = (program.sample_rate_hz > 0) ? (float)program.sample_rate_hz : (float)DAC_SAMPLE_RATE;
    float target_l = program.target_freq_hz;
    float target_r = program.target_freq_r_hz;
    if (target_l <= 0.0f) target_l = DEFAULT_SIN_HZ;
    if (target_r <= 0.0f) target_r = target_l;

    int period_l = (int)lroundf(fs / target_l);
    int period_r = (int)lroundf(fs / target_r);
    if (period_l < MIN_SIN_PERIOD) period_l = MIN_SIN_PERIOD;
    if (period_r < MIN_SIN_PERIOD) period_r = MIN_SIN_PERIOD;
    if (period_l > MAX_WAVE_LEN) period_l = MAX_WAVE_LEN;
    if (period_r > MAX_WAVE_LEN) period_r = MAX_WAVE_LEN;

    int wave_len = lcm_int(period_l, period_r);
    if (wave_len > MAX_WAVE_LEN) wave_len = (period_l > period_r) ? period_l : period_r;
    if (wave_len > MAX_WAVE_LEN) wave_len = MAX_WAVE_LEN;

    const int16_t amp_l = maToPeakCodes(program.amp_l_ma, g_code_to_ma_l);
    const int16_t amp_r = maToPeakCodes(program.amp_r_ma, g_code_to_ma_r);
    g_wave_len = (size_t)wave_len;
    for (int i = 0; i < wave_len; i++) {
        float vl = sinf(2.0f * (float)M_PI * (float)(i % period_l) / (float)period_l);
        float vr = sinf(2.0f * (float)M_PI * (float)(i % period_r) / (float)period_r);
        int32_t sl = (int32_t)lroundf(vl * 32767.0f);
        int32_t sr = (int32_t)lroundf(vr * 32767.0f);
        g_wave[i * 2 + 0] = (int16_t)(((int32_t)sl * amp_l) / 32768);
        g_wave[i * 2 + 1] = (int16_t)(((int32_t)sr * amp_r) / 32768);
    }
    program.actual_freq_hz = fs / (float)period_l;
    program.actual_freq_r_hz = fs / (float)period_r;
    program.period_samples_l = period_l;
    program.period_samples_r = period_r;
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

    const int16_t amp_l = maToPeakCodes(amp_l_ma, g_code_to_ma_l);
    const int16_t amp_r = maToPeakCodes(amp_r_ma, g_code_to_ma_r);
    g_wave_len = count;
    for (size_t i = 0; i < count; i++) {
        int32_t l = ((int32_t)left[i] * amp_l) / peak_l;
        int32_t r = ((int32_t)right[i] * amp_r) / peak_r;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        g_wave[i * 2 + 0] = (int16_t)l; // L (индекс 0, TODO #7)
        g_wave[i * 2 + 1] = (int16_t)r; // R
    }
    return true;
}

static DacProgram g_program = {
    DacWaveform::SIN,
    DEFAULT_AMP_MA,
    DEFAULT_AMP_MA,
    DEFAULT_SIN_HZ,
    DEFAULT_SIN_HZ,
    DEFAULT_SIN_HZ,
    DEFAULT_SIN_HZ,
    DAC_SAMPLE_RATE
};
static volatile float g_gain = 1.0f;

static void rebuildWave(DacProgram& program) {
    if (program.waveform == DacWaveform::CONST_DC) {
        fill_const(program);
        program.actual_freq_hz = 0.0f;
        program.actual_freq_r_hz = 0.0f;
        program.period_samples_l = 0;
        program.period_samples_r = 0;
        return;
    }
    if (program.waveform == DacWaveform::CUSTOM) {
        program.actual_freq_hz = 0.0f;
        program.actual_freq_r_hz = 0.0f;
        program.period_samples_l = 0;
        program.period_samples_r = 0;
        return;
    }
    fill_sin(program);
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
            // Позиционное копирование: индекс 0 = L, индекс 1 = R (TODO #7).
            int32_t ch0 = (int32_t)lroundf((float)g_wave[k * 2 + 0] * gain);
            int32_t ch1 = (int32_t)lroundf((float)g_wave[k * 2 + 1] * gain);
            if (ch0 > 32767) ch0 = 32767;
            if (ch0 < -32768) ch0 = -32768;
            if (ch1 > 32767) ch1 = 32767;
            if (ch1 < -32768) ch1 = -32768;
            buf[j * 2 + 0] = (int16_t)ch0;
            buf[j * 2 + 1] = (int16_t)ch1;
            if (++k >= g_wave_len) k = 0;
        }
        size_t written = 0;
        i2s_write(s_i2s_port, buf, sizeof(buf), &written, portMAX_DELAY);
    }
    // Грациозный выход: помечаем завершение до самоудаления, чтобы stop() не убивал
    // задачу, заблокированную внутри i2s_write (иначе мьютекс драйвера остаётся
    // захваченным навсегда -> повторный старт виснет). См. TODO #6.
    s_task = nullptr;
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

void DacControl::setCodeToMa(float codes_per_ma_l, float codes_per_ma_r) {
    g_code_to_ma_l = (codes_per_ma_l > 0.0f) ? codes_per_ma_l : DEF_DAC_CODE_TO_MA_L;
    g_code_to_ma_r = (codes_per_ma_r > 0.0f) ? codes_per_ma_r : DEF_DAC_CODE_TO_MA_R;
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

    // Ждём, пока playerTask сам выйдет из цикла (i2s_write возвращается каждые ~32 мс)
    // и освободит мьютекс драйвера. Только потом трогаем i2s. См. TODO #6.
    for (int i = 0; i < 100 && s_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_task) {  // подстраховка на случай зависания
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    i2s_zero_dma_buffer(s_i2s_port);
    g_gain = 0.0f;
    digitalWrite(EN_WAKEUP, LOW);
}
