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

// Получить минимальное и максимальное напряжение с ADC (в вольтах)
// Возвращает true если есть валидные данные, false если буфер ещё не заполнен
bool getADCMinMaxVoltage(float* min_voltage, float* max_voltage);

// Получить перцентили (1%, 99%) и среднее напряжение с ADC (в вольтах)
// Возвращает true если есть валидные данные, false если буфер ещё не заполнен
bool getADCPercentiles(float* p1_voltage, float* p99_voltage, float* mean_voltage);

// Построить гистограмму из ADC данных
// bins - массив для хранения гистограммы (размер должен быть num_bins)
// num_bins - количество столбцов гистограммы (рекомендуется 16-20)
// Возвращает true если есть валидные данные
bool buildADCHistogram(uint16_t* bins, uint8_t num_bins);

// Вычислить спектр для заданных частот (Goertzel algorithm)
// magnitudes - выходной массив для амплитуд (размер num_freqs)
// frequencies - массив частот для анализа (в Гц)
// num_freqs - количество частот
// Возвращает true если успешно
bool computeADCSpectrum(float* magnitudes, const float* frequencies, uint8_t num_freqs);

// Запланировать старт записи ADC после задержки (сбрасывает буфер)
void scheduleADCCaptureStart(uint32_t delay_ms);

#endif // ADC_CONTROL_H

