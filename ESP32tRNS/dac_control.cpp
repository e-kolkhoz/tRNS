#include "dac_control.h"
#include "config.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>
#include <cmath>

namespace {

constexpr uint32_t SAMPLE_RATE = 8000;
//constexpr float    FREQ_HZ     = 640.0f;
// 8000/640 = 12.5 сэмпла на период → 25 сэмплов = ровно 2 периода, буфер циклически замыкается без дроби
// constexpr int WAVE_LEN = 25;

constexpr float    FREQ_HZ     = 100.0f;
// 8000/100 = 80 сэмпла на период
constexpr int WAVE_LEN = 80;

constexpr int16_t AMP_LEFT  = 32767;
constexpr int16_t AMP_RIGHT = 32767;

// один раз заполняется в init(): [R,L,R,L,...]
int16_t g_wave[WAVE_LEN * 2];

void fill_wave_once() {
    for (int i = 0; i < WAVE_LEN; i++) {
        float   v = std::sin(2.0f * M_PI * FREQ_HZ * (float)i / (float)SAMPLE_RATE);
        int32_t s = (int32_t)(v * 32767.0f);
        g_wave[i * 2 + 0] = (int16_t)((s * AMP_RIGHT) / 32768);
        g_wave[i * 2 + 1] = (int16_t)((s * AMP_LEFT) / 32768);
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
            k++;
            if (k >= (size_t)WAVE_LEN) {
                k = 0;
            }
        }

        size_t written = 0;
        i2s_write(s_i2s_port, buf, sizeof(buf), &written, portMAX_DELAY);
    }

    vTaskDelete(NULL);
}

void DacControl::init() {
    fill_wave_once();

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
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
    i2s_set_clk(s_i2s_port, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    i2s_zero_dma_buffer(s_i2s_port);
}

void DacControl::start() {
    if (s_playing) return;
    s_playing = true;

    digitalWrite(EN_WAKEUP, HIGH);

    xTaskCreatePinnedToCore(
        playerTask,
        "dac_sin",
        2048,
        NULL,
        5,
        &s_task,
        1
    );
}

void DacControl::stop() {
    if (!s_playing) return;
    s_playing = false;

    if (s_task) {
        vTaskDelete(s_task);
        s_task = nullptr;
    }

    // Драйвер I2S остаётся установленным (см. init в setup) — иначе повторный start()
    // пишет в несуществующий порт и на выходе мусор. Полный uninstall не делаем.
    i2s_zero_dma_buffer(s_i2s_port);

    digitalWrite(EN_WAKEUP, LOW);
}
