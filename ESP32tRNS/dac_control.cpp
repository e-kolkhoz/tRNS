#include "dac_control.h"
#include "config.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>
#include <cmath>

namespace {

constexpr float    FREQ_HZ     = 640.0f;
constexpr int      WAVE_LEN    = 25;   // 8000/640×2 = 2 периода, без дроби
constexpr float    TEST_MA_L   = 1.0f;
constexpr float    TEST_MA_R   = 2.0f;

int16_t g_wave[WAVE_LEN * 2];

static int16_t maToPeakCodes(float ma) {
    int32_t c = (int32_t)lroundf(ma * DEF_DAC_CODE_TO_MA);
    if (c > 32767) c = 32767;
    if (c < 0)     c = 0;
    return (int16_t)c;
}

void fill_wave_once() {
    const int16_t amp_l = maToPeakCodes(TEST_MA_L);
    const int16_t amp_r = maToPeakCodes(TEST_MA_R);

    for (int i = 0; i < WAVE_LEN; i++) {
        float v = sinf(2.0f * (float)M_PI * FREQ_HZ * (float)i / (float)DAC_SAMPLE_RATE);
        int32_t s = (int32_t)lroundf(v * 32767.0f);
        g_wave[i * 2 + 0] = (int16_t)(((int32_t)s * amp_r) / 32768);  // R = I2S right
        g_wave[i * 2 + 1] = (int16_t)(((int32_t)s * amp_l) / 32768);  // L = I2S left
    }
}

}  // namespace

bool DacControl::s_playing = false;
i2s_port_t DacControl::s_i2s_port = I2S_NUM_0;
TaskHandle_t DacControl::s_task = nullptr;

void DacControl::playerTask(void* arg) {
    (void)arg;
    size_t k = 0;

    while (s_playing) {
        int16_t buf[256 * 2];
        for (int j = 0; j < 256; j++) {
            buf[j * 2 + 0] = g_wave[k * 2 + 0];
            buf[j * 2 + 1] = g_wave[k * 2 + 1];
            if (++k >= (size_t)WAVE_LEN) k = 0;
        }
        size_t written = 0;
        i2s_write(s_i2s_port, buf, sizeof(buf), &written, portMAX_DELAY);
    }
    vTaskDelete(nullptr);
}

void DacControl::init() {
    fill_wave_once();

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
    digitalWrite(EN_WAKEUP, LOW);
}
