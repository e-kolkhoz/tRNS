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

// Получить перцентили (1%, 99%) и среднее в RAW ADC кодах (знаковые!)
// Для использования с калибровочной таблицей
bool getADCPercentilesRaw(int16_t* p1_raw, int16_t* p99_raw, int16_t* mean_raw);

// Построить гистограмму из ADC данных
// bins - массив для хранения гистограммы (размер должен быть num_bins)
// num_bins - количество столбцов гистограммы (рекомендуется 16-20)
// Возвращает true если есть валидные данные
bool buildADCHistogram(uint16_t* bins, uint8_t num_bins);

// Запланировать старт записи ADC после задержки (сбрасывает буфер)
void scheduleADCCaptureStart(uint32_t delay_ms);

// Вывод буфера в Serial для Arduino Plotter (с децимацией)
// decimation = 1 — каждый сэмпл, 10 — каждый 10-й, и т.д.
void dumpADCToSerial(uint16_t decimation = 40);

#endif // ADC_CONTROL_H

