#include "display_control.h"
#include "dac_control.h"
#include "adc_control.h"
#include "adc_calibration.h"
#include "menu_control.h"
#include "session_control.h"
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
static const uint32_t DISPLAY_UPDATE_INTERVAL_MS = 200;  // Обновление каждые 500 мс — чтобы не блокировать DAC!

// Инициализация дисплея
void initDisplay() {
  // Инициализация I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ);
  
  // Инициализация дисплея
  u8g2.begin();
  u8g2.enableUTF8Print();  // ВАЖНО! Включаем UTF-8 для кириллицы
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.setFontRefHeightExtendedText();
  u8g2.setDrawColor(1);
  u8g2.setFontPosTop();
  u8g2.setFontDirection(0);
  
  // Приветственный экран
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_9x15_t_cyrillic);
  u8g2.drawStr(10, 10, "tRNS/tACS");
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.drawStr(20, 35, "Booting...");
  u8g2.sendBuffer();
}

// Показать экран загрузки с шагом инициализации
void showBootScreen(const char* step) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13_t_cyrillic);
  u8g2.drawStr(0, 0, "tRNS/tACS/tDCS");
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  
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
  
  renderCurrentScreen();  // Рендерим текущий экран
}

// Принудительное обновление дисплея
void refreshDisplay() {
  u8g2.clearBuffer();
  
  // === СТРОКА 1: Название пресета ===
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  
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
  
  // ПРАВЫЙ ВЕРХНИЙ УГОЛ: dynamic_dac_gain в формате X.X
  extern float dynamic_dac_gain;
  char gain_str[6];
  snprintf(gain_str, sizeof(gain_str), "%.1f", dynamic_dac_gain);
  u8g2.drawStr(110, 0, gain_str);  // Правый край

  // === СТРОКА 2: Таймер MM:SS (крупный шрифт) ===
  u8g2.setFont(u8g2_font_7x13_t_cyrillic);
  uint32_t elapsed_sec = (millis() - session_timer_start_ms) / 1000;
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
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  
  // === СТРОКА 2: Перцентили и среднее ADC ===
  int16_t p1_raw = 0, p99_raw = 0, mean_raw = 0;
  bool adc_valid = getADCPercentilesRaw(&p1_raw, &p99_raw, &mean_raw);
  
  if (adc_valid) {
    char stats_str[32];
    
    // Пересчёт через калибровочную таблицу
    float p1_mA = adcSignedToMilliamps(p1_raw);
    float mean_mA = adcSignedToMilliamps(mean_raw);
    float p99_mA = adcSignedToMilliamps(p99_raw);

    snprintf(stats_str, sizeof(stats_str), "%.1f > %.1f < %.1f", p1_mA, mean_mA, p99_mA);
    u8g2.setFont(u8g2_font_7x13_t_cyrillic);
    u8g2.drawStr(0, 50, stats_str);
  } else {
    u8g2.drawStr(0, 28, "ADC: waiting...");
  }
  
  // === СТРОКИ 3-6: Гистограмму ADC ===
  const uint8_t NUM_BINS = 11;  // Меньше бинов = шире каждый = меньше шума ADC
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
    const uint8_t BIN_WIDTH = 10;       // Ширина столбца (128 / 11 ≈ 11)
    const uint8_t BIN_SPACING = 1;      // Отступ между столбцами
    
    
    // Рисуем гистограмму ADC
    for (uint8_t i = 0; i < NUM_BINS; i++) {
      uint8_t x = i * (BIN_WIDTH + BIN_SPACING) + 4;
      uint8_t height = (adc_bins[i] * HIST_HEIGHT) / max_adc;
      if (height > 0) {
        u8g2.drawBox(x, HIST_Y_ADC + HIST_HEIGHT - height, BIN_WIDTH, height);
      }
    }
  } else {
    // Чтобы не накладывать текст "ADC: waiting..." и "Histograms: waiting..."
    if (adc_valid) {
      u8g2.setFont(u8g2_font_6x12_t_cyrillic);
      u8g2.drawStr(0, 40, "Histograms: waiting...");
    }
  }
  
  u8g2.sendBuffer();
}

// === РЕНДЕРИНГ ЭКРАНОВ МЕНЮ ===
void renderMenu(const char* title, const char* choices[], uint8_t num_choices, uint8_t selected) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  
  // Заголовок (без линии - экономим пиксели)
  u8g2.setCursor(0, 0);
  u8g2.print(title);
  
  // Прокрутка: экран 64px, заголовок 12px, остаётся 52px для пунктов (шаг 10px = 5 пунктов)
  const uint8_t MENU_Y_START = 12;   // Начало списка опций
  const uint8_t ITEM_HEIGHT = 10;    // Высота пункта
  const uint8_t MAX_VISIBLE = 5;     // Макс. видимых пунктов
  
  // Вычисляем смещение прокрутки
  uint8_t scroll_offset = 0;
  if (num_choices > MAX_VISIBLE) {
    if (selected >= MAX_VISIBLE) {
      scroll_offset = selected - MAX_VISIBLE + 1;
    }
    // Не выходим за границы
    if (scroll_offset > num_choices - MAX_VISIBLE) {
      scroll_offset = num_choices - MAX_VISIBLE;
    }
  }
  
  // Рисуем видимые пункты
  uint8_t visible_count = (num_choices < MAX_VISIBLE) ? num_choices : MAX_VISIBLE;
  for (uint8_t i = 0; i < visible_count; i++) {
    uint8_t item_idx = scroll_offset + i;
    uint8_t y = MENU_Y_START + i * ITEM_HEIGHT;
    
    if (item_idx == selected) {
      u8g2.setCursor(0, y);
      u8g2.print(">");
    }
    u8g2.setCursor(10, y);
    u8g2.print(choices[item_idx]);
  }
  
  // Индикаторы прокрутки (справа)
  if (num_choices > MAX_VISIBLE) {
    if (scroll_offset > 0) {
      u8g2.drawTriangle(124, 14, 120, 18, 128, 18);  // ▲ вверху
    }
    if (scroll_offset < num_choices - MAX_VISIBLE) {
      u8g2.drawTriangle(124, 62, 120, 58, 128, 58);  // ▼ внизу
    }
  }
  
  u8g2.sendBuffer();
}

void renderEditor() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  
  // Название параметра (без линии)
  u8g2.setCursor(0, 0);
  u8g2.print(editor_data.name);
  
  // Значение (крупным шрифтом) - показываем ВРЕМЕННОЕ значение!
  u8g2.setFont(u8g2_font_9x15_t_cyrillic);
  char value_str[16];
  if (editor_data.is_int) {
    snprintf(value_str, sizeof(value_str), "%d", (int)editor_temp_value);
  } else {
    snprintf(value_str, sizeof(value_str), "%.1f", editor_temp_value);
  }
  u8g2.drawStr(30, 30, value_str);
  
  u8g2.sendBuffer();
}

void renderConfirm() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  
  u8g2.setCursor(0, 0);
  u8g2.print("Остановить сеанс?");
  
  if (menu_selected == 0) {
    u8g2.setCursor(0, 25);
    u8g2.print("> Нет, продолжить");
    u8g2.setCursor(0, 40);
    u8g2.print("  Да, плавный стоп");
  } else {
    u8g2.setCursor(0, 25);
    u8g2.print("  Нет, продолжить");
    u8g2.setCursor(0, 40);
    u8g2.print("> Да, плавный стоп");
  }
  
  u8g2.sendBuffer();
}

// === ГЛАВНАЯ ФУНКЦИЯ РЕНДЕРИНГА ===
void renderCurrentScreen() {
  switch (current_screen) {
    case SCR_DASHBOARD:
      refreshDisplay();  // Используем старый dashboard
      break;
      
    case SCR_MAIN_MENU:
      {
        const char* choices[] = { "tRNS", "tDCS", "tACS", "Общие настройки" };
        renderMenu("Главное меню", choices, 4, menu_selected);
      }
      break;
      
    case SCR_TRNS_MENU:
      {
        static char amp_str[48], dur_str[48];
        snprintf(amp_str, sizeof(amp_str), "Амплитуда: %.1fмА", current_settings.amplitude_tRNS_mA);
        snprintf(dur_str, sizeof(dur_str), "Длительность: %uм", current_settings.duration_tRNS_min);
        const char* choices[] = { "СТАРТ", amp_str, dur_str, "<-Назад" };
        renderMenu("tRNS", choices, 4, menu_selected);
      }
      break;
      
    case SCR_TDCS_MENU:
      {
        static char amp_str[48], dur_str[48];
        snprintf(amp_str, sizeof(amp_str), "Макс.ток: %.1fмА", current_settings.amplitude_tDCS_mA);
        snprintf(dur_str, sizeof(dur_str), "Длительность: %uм", current_settings.duration_tDCS_min);
        const char* choices[] = { "СТАРТ", amp_str, dur_str, "<-Назад" };
        renderMenu("tDCS", choices, 4, menu_selected);
      }
      break;
      
    case SCR_TACS_MENU:
      {
        static char amp_str[48], freq_str[48], dur_str[48];
        snprintf(amp_str, sizeof(amp_str), "Амплитуда: %.1fмА", current_settings.amplitude_tACS_mA);
        snprintf(freq_str, sizeof(freq_str), "Частота: %.0fГц", current_settings.frequency_tACS_Hz);
        snprintf(dur_str, sizeof(dur_str), "Длительность: %uм", current_settings.duration_tACS_min);
        const char* choices[] = { "СТАРТ", amp_str, freq_str, dur_str, "<-Назад" };
        renderMenu("tACS", choices, 5, menu_selected);
      }
      break;
      
    case SCR_SETTINGS_MENU:
      {
        static char dac_str[48], fade_str[48], pol_str[48], enc_str[48];
        snprintf(dac_str, sizeof(dac_str), "DAC коды/мА: %.0f", current_settings.dac_code_to_mA);
        snprintf(fade_str, sizeof(fade_str), "Плавный пуск: %.0fс", current_settings.fade_duration_sec);
        snprintf(pol_str, sizeof(pol_str), "Полярность: %s", current_settings.polarity_invert ? "Инв" : "Норм");
        snprintf(enc_str, sizeof(enc_str), "Энкодер: %s", current_settings.enc_direction_invert ? "Инв" : "Норм");
        const char* choices[] = { "<-Назад", dac_str, fade_str, pol_str, enc_str, "СБРОС на заводские" };
        renderMenu("ОБЩИЕ НАСТРОЙКИ", choices, 6, menu_selected);
      }
      break;
      
    case SCR_EDITOR:
      renderEditor();
      break;
      
    case SCR_CONFIRM:
      renderConfirm();
      break;
      
    case SCR_FINISH:
      {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_7x13_t_cyrillic);
        u8g2.setCursor(0, 0);
        u8g2.print("СЕАНС ЗАВЕРШЕН");

        
        u8g2.setFont(u8g2_font_6x12_t_cyrillic);
        char info_str[32];
        snprintf(info_str, sizeof(info_str), "%s", getModeName(current_settings.mode));
        u8g2.setCursor(10, 35);
        u8g2.print(info_str);
        
        // Показываем фактическое время сеанса в формате MM:SS
        extern uint32_t session_elapsed_sec;
        uint32_t mins = session_elapsed_sec / 60;
        uint32_t secs = session_elapsed_sec % 60;
        
        // Выбираем амплитуду в зависимости от режима
        float amplitude = DEF_AMPLITUDE_MA;  // дефолт из config.h
        switch (current_settings.mode) {
          case MODE_TRNS: amplitude = current_settings.amplitude_tRNS_mA; break;
          case MODE_TDCS: amplitude = current_settings.amplitude_tDCS_mA; break;
          case MODE_TACS: amplitude = current_settings.amplitude_tACS_mA; break;
        }
        
        snprintf(info_str, sizeof(info_str), "%.1fmA %u:%02u", amplitude, mins, secs);
        u8g2.setCursor(10, 47);
        u8g2.print(info_str);
        u8g2.sendBuffer();
      }
      break;
  }
}

// Установить статус устройства
void setDisplayStatus(const char* status) {
  if (status != NULL) {
    strncpy(device_status, status, sizeof(device_status) - 1);
    device_status[sizeof(device_status) - 1] = '\0';
    refreshDisplay();  // Немедленное обновление при смене статуса
  }
}


