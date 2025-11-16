#include "display_control.h"
#include "dac_control.h"
#include "adc_control.h"
#include "usb_commands.h"
#include <Wire.h>
#include <U8g2lib.h>

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
  usbLog("=== OLED Display Init ===");
  
  // Инициализация I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ);
  
  // Инициализация дисплея
  // Если дисплей не определяется, попробуйте:
  // 1. Изменить адрес: u8g2.setI2CAddress(DISPLAY_ADDR * 2); // адрес в U8g2 = адрес * 2
  // 2. Заменить класс на U8G2_SH1106_128X64_NONAME_F_HW_I2C для SH1106
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);  // Основной шрифт
  u8g2.setFontRefHeightExtendedText();
  u8g2.setDrawColor(1);
  u8g2.setFontPosTop();
  u8g2.setFontDirection(0);
  timer_start_ms = millis();
  
  // Очистка и приветствие
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawStr(0, 0, "tRNS/tACS");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, "Device Starting...");
  u8g2.sendBuffer();
  
  usbLogf("OLED Display initialized (I2C: SDA=%d, SCL=%d, Addr=0x%02X)", 
          I2C_SDA, I2C_SCL, DISPLAY_ADDR);
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
  char preset_display[23];
  const char* preset_name = current_preset_name;
  int len = strlen(preset_name);
  if (len > 22) {
    strncpy(preset_display, preset_name, 23);
    preset_display[19] = '.';
    preset_display[20] = '.';
    preset_display[21] = '.';
    preset_display[22] = '\0';
  } else {
    strncpy(preset_display, preset_name, 23);
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
  
  // === СТРОКИ 3-6: Гистограмму ADC ===
  const uint8_t NUM_BINS = 16;  // 16 столбцов для гистограммы
  uint16_t adc_bins[NUM_BINS];
  bool adc_hist_ok = buildADCHistogram(adc_bins, NUM_BINS);
  
  if (adc_hist_ok) {
    // Находим максимальное значение для нормализации высоты столбцов
    uint16_t max_adc = 0;
    for (uint8_t i = 0; i < NUM_BINS; i++) {
      if (adc_bins[i] > max_adc) max_adc = adc_bins[i];
    }
    
    if (max_adc == 0) max_adc = 1;  // Защита от деления на ноль
    
    // Высота области для гистограмм: 64 - 24 = 40 пикселей
    // Делим пополам: по 20 пикселей на каждую гистограмму
    const uint8_t HIST_HEIGHT = 18;  // Высота столбцов гистограммы
    //const uint8_t HIST_Y_PRESET = 26;  // Y координата для пресета
    const uint8_t HIST_Y_ADC = 26;     // Y координата для ADC
    const uint8_t BIN_WIDTH = 7;        // Ширина столбца (128 / 16 = 8, но с отступами 7)
    const uint8_t BIN_SPACING = 1;      // Отступ между столбцами
    
    
    // Рисуем гистограмму ADC
    for (uint8_t i = 0; i < NUM_BINS; i++) {
      uint8_t x = i * (BIN_WIDTH + BIN_SPACING);
      uint8_t height = (adc_bins[i] * HIST_HEIGHT) / max_adc;
      if (height > 0) {
        u8g2.drawBox(x, HIST_Y_ADC + HIST_HEIGHT - height, BIN_WIDTH, height);
      }
    }
  } else {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 30, "Histograms: waiting...");
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

