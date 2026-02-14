#ifndef ADC_CALIBRATION_H
#define ADC_CALIBRATION_H

#include <Arduino.h>

// ============================================================================
// === КАЛИБРОВКА ADC: таблица ADC_raw → mA ===
// ============================================================================
// Линейная интерполяция между точками таблицы
// Экстраполяция за границами

// Инициализация LUT — вызвать один раз при старте!
void initADCCalibration();

// Пересчёт беззнакового raw ADC кода (magnitude) в миллиамперы (O(1) через LUT)
float adcRawToMilliamps(uint16_t adc_raw);

// Пересчёт знакового ADC кода в миллиамперы (для sign-magnitude данных)
// Сохраняет знак: положительный код → положительные mA, отрицательный → отрицательные
float adcSignedToMilliamps(int16_t adc_signed);

#endif // ADC_CALIBRATION_H

