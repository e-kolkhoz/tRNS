#include "adc_calibration.h"

// ============================================================================
// === КАЛИБРОВОЧНАЯ ТАБЛИЦА ===
// ============================================================================
// Формат: {ADC_raw, mA}
// ВАЖНО: таблица ОБЯЗАНА быть отсортирована по возрастанию ADC_raw!

struct CalibrationPoint {
  uint16_t adc_raw;
  float mA;
};

static constexpr CalibrationPoint CALIB_TABLE[] = {
  {1046, 0.1f},
  {1116, 0.2f},
  {1178, 0.3f},
  {1232, 0.4f},
  {1282, 0.5f},
  {1334, 0.6f},
  {1386, 0.7f},
  {1430, 0.8f},
  {1476, 0.9f},
  {1522, 1.0f},
  {1620, 1.2f},
  {1658, 1.3f},
  {1746, 1.5f},
  {1830, 1.7f},
  {1876, 1.8f},
  {1956, 2.0f},
};

static constexpr size_t CALIB_TABLE_SIZE = sizeof(CALIB_TABLE) / sizeof(CALIB_TABLE[0]);

// ============================================================================
// === COMPILE-TIME ПРОВЕРКИ ===
// ============================================================================

// Проверка что таблица отсортирована и без дубликатов
constexpr bool isTableValid() {
  for (size_t i = 1; i < CALIB_TABLE_SIZE; i++) {
    if (CALIB_TABLE[i].adc_raw <= CALIB_TABLE[i-1].adc_raw) {
      return false;  // Не отсортирована или дубликат
    }
  }
  return true;
}

static_assert(CALIB_TABLE_SIZE >= 2, "Таблица должна содержать минимум 2 точки");
static_assert(isTableValid(), "Таблица должна быть отсортирована по возрастанию ADC без дубликатов");

// ============================================================================
// === ФУНКЦИЯ ПЕРЕСЧЁТА ===
// ============================================================================

float adcRawToMilliamps(uint16_t adc_raw) {
  // Ищем интервал для интерполяции
  size_t i1 = 0;
  size_t i2 = 1;
  
  if (adc_raw <= CALIB_TABLE[0].adc_raw) {
    // Экстраполяция влево
    i1 = 0; i2 = 1;
  } else if (adc_raw >= CALIB_TABLE[CALIB_TABLE_SIZE - 1].adc_raw) {
    // Экстраполяция вправо
    i1 = CALIB_TABLE_SIZE - 2;
    i2 = CALIB_TABLE_SIZE - 1;
  } else {
    // Бинарный поиск интервала
    size_t lo = 0, hi = CALIB_TABLE_SIZE - 1;
    while (hi - lo > 1) {
      size_t mid = (lo + hi) / 2;
      if (CALIB_TABLE[mid].adc_raw <= adc_raw) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    i1 = lo;
    i2 = hi;
  }
  
  // Линейная интерполяция/экстраполяция
  float adc1 = (float)CALIB_TABLE[i1].adc_raw;
  float adc2 = (float)CALIB_TABLE[i2].adc_raw;
  float mA1 = CALIB_TABLE[i1].mA;
  float mA2 = CALIB_TABLE[i2].mA;
  
  float mA = mA1 + ((float)adc_raw - adc1) * (mA2 - mA1) / (adc2 - adc1);
  
  return (mA < 0.0f) ? 0.0f : mA;
}

// Для знаковых данных — просто берём abs и восстанавливаем знак
float adcSignedToMilliamps(int16_t adc_signed) {
  float mag = adcRawToMilliamps((uint16_t)abs(adc_signed));
  return (adc_signed < 0) ? -mag : mag;
}
