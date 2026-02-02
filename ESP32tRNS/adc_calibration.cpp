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
// === COMPILE-TIME ПРОВЕРКИ ТАБЛИЦЫ ===
// ============================================================================

constexpr bool isTableValid() {
  for (size_t i = 1; i < CALIB_TABLE_SIZE; i++) {
    if (CALIB_TABLE[i].adc_raw <= CALIB_TABLE[i-1].adc_raw) {
      return false;
    }
  }
  return true;
}

static_assert(CALIB_TABLE_SIZE >= 2, "Таблица должна содержать минимум 2 точки");
static_assert(isTableValid(), "Таблица должна быть отсортирована по возрастанию ADC без дубликатов");

// ============================================================================
// === LOOKUP TABLE (LUT) ===
// ============================================================================

static float code2mA[4096];  // Предрассчитанная таблица: индекс = ADC код, значение = mA

// Интерполяция для заполнения LUT (вызывается один раз при старте)
static float interpolateMilliamps(uint16_t adc_raw) {
  size_t i1 = 0, i2 = 1;
  
  if (adc_raw <= CALIB_TABLE[0].adc_raw) {
    i1 = 0; i2 = 1;
  } else if (adc_raw >= CALIB_TABLE[CALIB_TABLE_SIZE - 1].adc_raw) {
    i1 = CALIB_TABLE_SIZE - 2;
    i2 = CALIB_TABLE_SIZE - 1;
  } else {
    for (size_t i = 0; i < CALIB_TABLE_SIZE - 1; i++) {
      if (adc_raw >= CALIB_TABLE[i].adc_raw && adc_raw < CALIB_TABLE[i + 1].adc_raw) {
        i1 = i;
        i2 = i + 1;
        break;
      }
    }
  }
  
  float adc1 = (float)CALIB_TABLE[i1].adc_raw;
  float adc2 = (float)CALIB_TABLE[i2].adc_raw;
  float mA1 = CALIB_TABLE[i1].mA;
  float mA2 = CALIB_TABLE[i2].mA;
  
  float mA = mA1 + ((float)adc_raw - adc1) * (mA2 - mA1) / (adc2 - adc1);
  return (mA < 0.0f) ? 0.0f : mA;
}

// Инициализация LUT — вызвать один раз при старте!
void initADCCalibration() {
  for (uint16_t i = 0; i < 4096; i++) {
    code2mA[i] = interpolateMilliamps(i);
  }
}

// ============================================================================
// === БЫСТРЫЙ ДОСТУП (O(1) вместо O(log n)) ===
// ============================================================================

float adcRawToMilliamps(uint16_t adc_raw) {
  if (adc_raw >= 4096) adc_raw = 4095;  // Защита от выхода за границы
  return code2mA[adc_raw];
}

// Для знаковых данных — abs + восстановление знака
float adcSignedToMilliamps(int16_t adc_signed) {
  float mag = adcRawToMilliamps((uint16_t)abs(adc_signed));
  return (adc_signed < 0) ? -mag : mag;
}
