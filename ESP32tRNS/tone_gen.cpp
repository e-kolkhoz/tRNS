#include "tone_gen.h"
#include <Arduino.h>
#include <math.h>
#include <atomic>

#include <driver/i2s_common.h>
#include <driver/i2s_pdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

struct LoopBuf {
    int16_t* data;
    uint32_t size;
};

struct Channel {
    std::atomic<LoopBuf*> loop;
    uint32_t              pos;          // только writer-task
    std::atomic<int32_t>  gain_q15;
};

// Одна "тишина" на оба канала при старте — указатель валиден всегда,
// loop никогда не nullptr → горячий путь без проверок.
int16_t  s_zero_sample = 0;
LoopBuf  s_silence     = { &s_zero_sample, 1 };

Channel  s_ch[ToneGen::CHANNELS];

i2s_chan_handle_t s_tx        = nullptr;
uint32_t          s_rate      = 0;
TaskHandle_t      s_taskHandle = nullptr;

// Размер чанка, который writer-task отдаёт в I2S за один write.
// 256 stereo-семплов × 4 байта = 1 КБ. На 384 кГц это ~0.67 мс — достаточно
// мелко, чтобы DMA-кольцо не голодало, и достаточно крупно, чтоб не дёргать ОС.
static constexpr int CHUNK_FRAMES = 256;

void writerTask(void*) {
    int16_t buf[CHUNK_FRAMES * 2];
    for (;;) {
        // Снимок указателей атомарно (loop может быть подменён в setSineLoop).
        LoopBuf* l0 = s_ch[0].loop.load(std::memory_order_acquire);
        LoopBuf* l1 = s_ch[1].loop.load(std::memory_order_acquire);
        int32_t  g0 = s_ch[0].gain_q15.load(std::memory_order_relaxed);
        int32_t  g1 = s_ch[1].gain_q15.load(std::memory_order_relaxed);

        for (int i = 0; i < CHUNK_FRAMES; ++i) {
            int16_t s0 = (int16_t)(((int32_t)l0->data[s_ch[0].pos] * g0) >> 15);
            int16_t s1 = (int16_t)(((int32_t)l1->data[s_ch[1].pos] * g1) >> 15);

            if (++s_ch[0].pos >= l0->size) s_ch[0].pos = 0;
            if (++s_ch[1].pos >= l1->size) s_ch[1].pos = 0;

            buf[i * 2 + 0] = s0;
            buf[i * 2 + 1] = s1;
        }

        size_t written = 0;
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

}  // namespace

bool ToneGen::begin(uint32_t sample_rate) {
    if (s_tx) return true;
    s_rate = sample_rate;

    for (int i = 0; i < CHANNELS; ++i) {
        s_ch[i].loop.store(&s_silence);
        s_ch[i].pos = 0;
        s_ch[i].gain_q15.store(0);
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = CHUNK_FRAMES;
    chan_cfg.auto_clear    = true;

    if (i2s_new_channel(&chan_cfg, &s_tx, nullptr) != ESP_OK) {
        s_tx = nullptr;
        return false;
    }

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk          = I2S_GPIO_UNUSED,
            .dout         = GPIO_NUM_1,
            .dout2        = GPIO_NUM_2,
            .invert_flags = { .clk_inv = 0 },
        },
    };

    if (i2s_channel_init_pdm_tx_mode(s_tx, &pdm_cfg) != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    if (i2s_channel_enable(s_tx) != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    BaseType_t r = xTaskCreatePinnedToCore(writerTask, "tonegen",
                                           4096, nullptr, 10, &s_taskHandle, 1);
    if (r != pdPASS) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    return true;
}

bool ToneGen::setSineLoop(int ch, float freq_hz, int32_t gain_q15) {
    if (ch < 0 || ch >= CHANNELS) return false;
    if (freq_hz <= 0.0f || s_rate == 0) return false;

    uint32_t loop_size = (uint32_t)lroundf((float)s_rate / freq_hz);
    if (loop_size < 2) loop_size = 2;

    int16_t* data = (int16_t*)heap_caps_malloc(loop_size * sizeof(int16_t),
                                               MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!data) return false;

    for (uint32_t i = 0; i < loop_size; ++i) {
        float v = sinf(2.0f * (float)M_PI * (float)i / (float)loop_size);
        int32_t s = (int32_t)lroundf(v * (float)INT16_MAX);
        if (s >  INT16_MAX) s = INT16_MAX;
        if (s < -INT16_MAX) s = -INT16_MAX;
        data[i] = (int16_t)s;
    }

    LoopBuf* lb = (LoopBuf*)heap_caps_malloc(sizeof(LoopBuf), MALLOC_CAP_8BIT);
    if (!lb) { heap_caps_free(data); return false; }
    lb->data = data;
    lb->size = loop_size;

    LoopBuf* old = s_ch[ch].loop.exchange(lb, std::memory_order_acq_rel);
    s_ch[ch].gain_q15.store(gain_q15, std::memory_order_relaxed);

    // Старый буфер мог ещё читаться writer-task'ом — даём ей пройти один цикл.
    if (old && old != &s_silence) {
        vTaskDelay(pdMS_TO_TICKS(20));
        heap_caps_free(old->data);
        heap_caps_free(old);
    }
    return true;
}

void ToneGen::setGain(int ch, int32_t gain_q15) {
    if (ch < 0 || ch >= CHANNELS) return;
    s_ch[ch].gain_q15.store(gain_q15, std::memory_order_relaxed);
}

uint32_t ToneGen::sampleRate() { return s_rate; }

int32_t ToneGen::ampToGainQ15(float volts) {
    if (volts < 0.0f) volts = 0.0f;
    if (volts > FULLSCALE_VOLTS) volts = FULLSCALE_VOLTS;
    return (int32_t)lroundf((volts / FULLSCALE_VOLTS) * (float)Q15_ONE);
}
