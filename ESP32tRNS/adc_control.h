#ifndef ADC_CONTROL_H
#define ADC_CONTROL_H

#include <Arduino.h>
#include <esp_adc/adc_continuous.h>
#include "config.h"

// ============================================================================
// === ADC DMA CONTROL (показометр тока) ===
// ============================================================================

// Глобальные переменные
extern adc_continuous_handle_t adc_handle;
extern int16_t* adc_ring_buffer;
extern volatile uint32_t adc_write_index;

// Инициализация ADC в continuous mode (DMA!)
void initADC();

// Чтение данных из ADC DMA и добавление в кольцевой буфер
void readADCFromDMA();

// Получить копию текущего состояния кольцевого буфера
// Вызывается по запросу от Android через USB OTG
void getADCRingBuffer(int16_t* output_buffer, uint32_t* current_write_pos);

// Печать статистики ADC буфера (для отладки)
void printADCStats();

#endif // ADC_CONTROL_H

