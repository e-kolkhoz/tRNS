#pragma once
#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>
#include <driver/i2s_types.h>
#include <soc/clk_tree_defs.h>

// Двухканальный тон-генератор поверх I2S PDM TX (TWO_LINE_DAC).
// Каждый канал = независимый предрасчитанный кольцевой буфер ("луп") + gain.
//
//   ch0 → GPIO1
//   ch1 → GPIO2
//
// Архитектура и обоснование — TODO.md.
//
// Использование:
//   ToneGen::begin(384000);
//   ToneGen::setSineLoop(0, 640.0f,   ToneGen::ampToGainQ15(1.0f));
//   ToneGen::setSineLoop(1, 64000.0f, ToneGen::ampToGainQ15(1.5f));

class ToneGen {
public:
    static constexpr int     CHANNELS = 2;
    static constexpr int32_t Q15_ONE  = 32768;

    // Опорное напряжение на выходе PDM (после RC ФНЧ) при int16 = full scale.
    // PDM output 0..3.3V, средняя точка = 1.65V → ±1.65V полный размах.
    static constexpr float   FULLSCALE_VOLTS = 1.65f;

    struct Config {
        uint32_t                 sample_rate;
        soc_periph_i2s_clk_src_t clk_src;       // 0 = I2S_CLK_SRC_DEFAULT
        uint32_t                 up_sample_fp;  // 0 = взять из DAC default
        uint32_t                 up_sample_fs;  // 0 = взять из DAC default
        uint32_t                 bclk_div;      // 0 = взять из DAC default
    };

    // Запустить I2S PDM TX и фоновую writer-task.
    // Возвращает ESP_OK или код ошибки драйвера (виден в esp_err_to_name()).
    static esp_err_t begin(const Config& cfg);

    // Сокращение для совместимости.
    static esp_err_t begin(uint32_t sample_rate);

    // Перебрать список конфигов, остановиться на первом успешном.
    // На каждой попытке вызывает callback (если задан) с парой (cfg_index, err).
    // Возвращает индекс успешного конфига или -1.
    typedef void (*TryCb)(int idx, const Config& cfg, esp_err_t err);
    static int beginTry(const Config* configs, size_t n, TryCb cb);

    // Пробует ВСЕ конфиги, не запускает ничего по факту.
    // out_errs[i] = код ошибки i-го конфига. Канал после каждой попытки
    // удаляется, состояние ToneGen остаётся неинициализированным.
    static void tryAll(const Config* configs, size_t n, esp_err_t* out_errs);

    // Сменить содержимое канала на новый синус.
    // freq_hz берётся как есть, реальная частота = sample_rate / round(sample_rate/freq_hz).
    static bool setSineLoop(int ch, float freq_hz, int32_t gain_q15);

    // Мгновенно поменять усиление канала.
    static void setGain(int ch, int32_t gain_q15);

    static uint32_t sampleRate();

    // Удобный конвертер: желаемая амплитуда в вольтах → gain_q15.
    static int32_t ampToGainQ15(float volts);
};
