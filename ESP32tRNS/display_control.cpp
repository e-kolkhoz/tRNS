#include "display_control.h"
#include "dac_control.h"
#include "adc_control.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>

// Инициализация U8g2 для OLED 128x64 (SSD1306 или SH1106)
// Используем HW I2C (Wire) на пинах GPIO7 (SDA) и GPIO9 (SCL)
// Если дисплей не определяется, попробуйте заменить U8G2_SSD1306_128X64_NONAME_F_HW_I2C на U8G2_SH1106_128X64_NONAME_F_HW_I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Статус устройства для отображения
static char device_status[32] = "Ready";
static uint32_t last_display_update = 0;
static const uint32_t DISPLAY_UPDATE_INTERVAL_MS = 200;  // Обновление каждые 200 мс
static uint32_t timer_start_ms = 0;  // Начальная точка таймера (ms)

// Инициализация дисплея
void initDisplay() {
  // Инициализация I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ);
  
  // Инициализация дисплея
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setFontRefHeightExtendedText();
  u8g2.setDrawColor(1);
  u8g2.setFontPosTop();
  u8g2.setFontDirection(0);
  timer_start_ms = millis();
  
  // Приветственный экран
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_9x15B_tf);
  u8g2.drawStr(10, 10, "tRNS/tACS");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(20, 35, "Booting...");
  u8g2.sendBuffer();
}

// Показать экран загрузки с шагом инициализации
void showBootScreen(const char* step) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawStr(0, 0, "tRNS/tACS");
  u8g2.setFont(u8g2_font_6x10_tf);
  
  // Рисуем шаг инициализации
  u8g2.drawStr(0, 20, step);
  
  // Анимация загрузки (точки)
  static uint8_t dots = 0;
  dots = (dots + 1) % 4;
  char progress[8] = "";
  for (uint8_t i = 0; i < dots; i++) {
    strcat(progress, ".");
  }
  u8g2.drawStr(0, 35, progress);
  
  u8g2.sendBuffer();
}

// Обновление дисплея с текущими метриками
void updateDisplay() {
  uint32_t now = millis();
  
  // Обновляем дисплей не чаще чем раз в DISPLAY_UPDATE_INTERVAL_MS
  if (now - last_display_update < DISPLAY_UPDATE_INTERVAL_MS) {
    return;
  }
  last_display_update = now;
  
  refreshDisplay();
}

// Принудительное обновление дисплея
void refreshDisplay() {
  u8g2.clearBuffer();
  
  // === СТРОКА 1: Название пресета ===
  u8g2.setFont(u8g2_font_6x10_tf);
  
  // Название пресета (обрезаем если длинное)
  char preset_display[24];
  const char* preset_name = current_preset_name;
  int len = strlen(preset_name);
  if (len > 23) {
    strncpy(preset_display, preset_name, 21);
    preset_display[20] = '.';
    preset_display[21] = '.';
    preset_display[22] = '.';
    preset_display[23] = '\0';
  } else {
    strncpy(preset_display, preset_name, 24);
    preset_display[23] = '\0';
  }
  u8g2.drawStr(0, 0, preset_display);

  // === СТРОКА 2: Таймер MM:SS (крупный шрифт) ===
  u8g2.setFont(u8g2_font_7x14_tf);
  uint32_t elapsed_sec = (millis() - timer_start_ms) / 1000;
  uint16_t minutes_total = elapsed_sec / 60;
  uint8_t seconds = elapsed_sec % 60;
  if (minutes_total > 99) {
    minutes_total = 99;
    seconds = 59;
  }
  char timer_str[6];
  snprintf(timer_str, sizeof(timer_str), "%02u:%02u", (uint8_t)minutes_total, seconds);
  u8g2.drawStr(0, 14, timer_str);
  
  // Возвращаем основной шрифт для остального текста
  u8g2.setFont(u8g2_font_6x10_tf);
  
  // === СТРОКА 2: Перцентили и среднее ADC ===
  float p1_v = 0.0f, p99_v = 0.0f, mean_v = 0.0f;
  bool adc_valid = getADCPercentiles(&p1_v, &p99_v, &mean_v);
  
  if (adc_valid) {
    char stats_str[32];
    float p1_mA = (p1_v - ADC_CENTER_V) * ADC_V_TO_MA;
    float mean_mA = (mean_v - ADC_CENTER_V) * ADC_V_TO_MA;
    float p99_mA = (p99_v - ADC_CENTER_V) * ADC_V_TO_MA;
    

    snprintf(stats_str, sizeof(stats_str), "%.1f > %.1f < %.1f", p1_mA, mean_mA, p99_mA);
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(0, 50, stats_str);
  } else {
    u8g2.drawStr(0, 28, "ADC: waiting...");
  }
  
  // === СТРОКИ 3-6: Гистограмма (слева) + Спектр (справа) ===
  const uint8_t VISUAL_Y_START = 26;     // Y начало визуализации
  const uint8_t VISUAL_HEIGHT = 38;      // Высота области (64 - 26 = 38 пикселей)
  
  // --- Гистограмма (узкая, слева) ---
  const uint8_t HIST_NUM_BINS = 8;       // Меньше столбцов для экономии места
  const uint8_t HIST_WIDTH = 40;         // Ширина области гистограммы
  const uint8_t HIST_BIN_WIDTH = 4;      // Ширина столбца
  const uint8_t HIST_SPACING = 1;        // Отступ между столбцами
  
  uint16_t adc_bins[HIST_NUM_BINS];
  bool adc_hist_ok = buildADCHistogram(adc_bins, HIST_NUM_BINS);
  
  if (adc_hist_ok) {
    uint16_t max_bin = 1;
    for (uint8_t i = 0; i < HIST_NUM_BINS; i++) {
      if (adc_bins[i] > max_bin) max_bin = adc_bins[i];
    }
    
    for (uint8_t i = 0; i < HIST_NUM_BINS; i++) {
      uint8_t x = i * (HIST_BIN_WIDTH + HIST_SPACING);
      uint8_t height = (adc_bins[i] * VISUAL_HEIGHT) / max_bin;
      if (height > 0) {
        u8g2.drawBox(x, VISUAL_Y_START + VISUAL_HEIGHT - height, HIST_BIN_WIDTH, height);
      }
    }
  }
  
  // --- Спектр (винтажный стиль, справа) ---
  const uint8_t SPEC_X_START = HIST_WIDTH + 2;  // Начало спектра (после гистограммы)
  const uint8_t SPEC_WIDTH = 128 - SPEC_X_START; // Оставшаяся ширина
  const uint8_t SPEC_NUM_BANDS = SPECTRUM_NUM_BANDS;
  const uint8_t SPEC_BAR_WIDTH = (SPEC_WIDTH - SPEC_NUM_BANDS) / SPEC_NUM_BANDS;
  const uint8_t SPEC_SPACING = 1;
  
  float spectrum_mags[SPEC_NUM_BANDS];
  bool spectrum_ok = computeADCSpectrum(spectrum_mags, SPECTRUM_FREQUENCIES, SPEC_NUM_BANDS);
  
  if (spectrum_ok) {
    // Находим максимум для нормализации (логарифмическая шкала)
    float max_mag = 0.0001f;  // Минимальное значение для защиты
    for (uint8_t i = 0; i < SPEC_NUM_BANDS; i++) {
      if (spectrum_mags[i] > max_mag) max_mag = spectrum_mags[i];
    }
    
    // Рисуем столбцы спектра
    for (uint8_t i = 0; i < SPEC_NUM_BANDS; i++) {
      uint8_t x = SPEC_X_START + i * (SPEC_BAR_WIDTH + SPEC_SPACING);
      // Логарифмическая шкала для лучшей читаемости
      float normalized = spectrum_mags[i] / max_mag;
      if (normalized > 0.001f) {
        normalized = logf(normalized * 100.0f + 1.0f) / logf(101.0f);
      }
      uint8_t height = (uint8_t)(normalized * VISUAL_HEIGHT);
      if (height > VISUAL_HEIGHT) height = VISUAL_HEIGHT;
      
      if (height > 0) {
        u8g2.drawBox(x, VISUAL_Y_START + VISUAL_HEIGHT - height, SPEC_BAR_WIDTH, height);
      }
    }
  }
  
  // Если данных нет
  if (!adc_hist_ok && !spectrum_ok) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 30, "Waiting data...");
  }  
  u8g2.sendBuffer();
}

// Установить статус устройства
void setDisplayStatus(const char* status) {
  if (status != NULL) {
    strncpy(device_status, status, sizeof(device_status) - 1);
    device_status[sizeof(device_status) - 1] = '\0';
    refreshDisplay();  // Немедленное обновление при смене статуса
  }
}

void resetDisplayTimer() {
  timer_start_ms = millis();
  refreshDisplay();
}

