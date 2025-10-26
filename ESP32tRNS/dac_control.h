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
void keepDMAFilled();

#endif // DAC_CONTROL_H

