#pragma once
#include <stdint.h>
#include <stddef.h>

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

    // Запустить I2S PDM TX и фоновую writer-task.
    // Каналы стартуют со "тишиной" — нулевым усилением.
    static bool begin(uint32_t sample_rate);

    // Сменить содержимое канала на новый синус.
    // freq_hz берётся как есть, реальная частота = sample_rate / round(sample_rate/freq_hz).
    static bool setSineLoop(int ch, float freq_hz, int32_t gain_q15);

    // Мгновенно поменять усиление канала.
    static void setGain(int ch, int32_t gain_q15);

    static uint32_t sampleRate();

    // Удобный конвертер: желаемая амплитуда в вольтах → gain_q15.
    static int32_t ampToGainQ15(float volts);
};
