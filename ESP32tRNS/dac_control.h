#ifndef DAC_CONTROL_H
#define DAC_CONTROL_H

#include <Arduino.h>
#include <driver/i2s.h>
#include "config.h"

// ============================================================================
// === I2S DAC CONTROL (PCM5102A) ===
// ============================================================================

// Глобальный буфер с сигналом (пресет) - ТОЛЬКО МОНО (правый канал)!
// Левый канал = константа (DAC_LEFT_OFFSET_VOLTS) для смещения ADC
extern int16_t* signal_buffer;  // МОНО: SIGNAL_SAMPLES
extern bool dma_prefilled;

// Имя текущего пресета (например, "tACS 640Hz 1mA demo")
extern char current_preset_name[PRESET_NAME_MAX_LEN];

// Динамический коэффициент усиления (меняется на лету для fadein/fadeout)
// fadein: 0.0 → 1.0, stable: 1.0, fadeout: 1.0 → 0.0
extern float dynamic_dac_gain;
// Масштаб амплитуды (0..1) для мА → код DAC
void setAmplitudeScale(float scale);

// Инициализация I2S и DMA для DAC
void initDAC();

// === УПРАВЛЕНИЕ СИГНАЛОМ ===

// Установить новый сигнал для DAC (стерео буфер, без копирования данных)
// num_samples - количество МОНО-сэмплов (для стерео нужно умножить на 2)
void setSignalBuffer(int16_t* new_buffer, int num_samples);

// Генерация демо-сигнала (синус 640 Гц) - для отладки
void generateDemoSignal();

// === DMA УПРАВЛЕНИЕ ===

// Предзаполнение DMA буферов
void prefillDMABuffers();

// Поддержание DMA буферов заполненными (неблокирующе!)
// Возвращает true, если новый фрагмент удалось поставить в DMA
bool keepDMAFilled();

// Обновить стерео-буфер после изменения signal_buffer
// ВАЖНО: вызывать после generateSignal()!
void updateStereoBuffer();

// Полный сброс DAC DMA и повторное заполнение буфера
void resetDacPlayback();

// Управление I2S выходом (старт/стоп)
void startDacPlayback();
void stopDacPlayback();

#endif // DAC_CONTROL_H

