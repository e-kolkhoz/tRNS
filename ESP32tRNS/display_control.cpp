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

// ============================================================================
// === ОСЦИЛЛОГРАММЫ ДЛЯ ДАШБОРДОВ ===
// ============================================================================

// Константы осциллограммы
#define SCOPE_X       17    // Начало по X (после тиков Y)
#define SCOPE_W       112   // Ширина осциллограммы
#define SCOPE_Y       12    // Начало по Y
#define SCOPE_H       38    // Высота осциллограммы

// Рисует точечную горизонтальную линию
static void drawDottedHLine(uint8_t x, uint8_t y, uint8_t w) {
  for (uint8_t i = 0; i < w; i += 4) {
    u8g2.drawPixel(x + i, y);
  }
}

// Рисует осциллограмму из кольцевого буфера
// y_min, y_max — лимиты в мА
// tick_positions[] — Y позиции для тиков (в мА), tick_labels[] — подписи
// num_ticks — количество тиков
// samples — сколько сэмплов показать (0 = весь буфер), start_offset — смещение от конца
static void drawOscilloscope(float y_min, float y_max, 
                             const float* tick_positions, const char* const* tick_labels, uint8_t num_ticks,
                             uint32_t samples, uint32_t start_offset) {
  float y_range = y_max - y_min;
  if (y_range < 0.01f) y_range = 0.01f;
  
  u8g2.setFont(u8g2_font_4x6_tr);
  
  // Рисуем тики и точечные линии
  for (uint8_t t = 0; t < num_ticks; t++) {
    float normalized = (tick_positions[t] - y_min) / y_range;
    int16_t py = SCOPE_Y + SCOPE_H - 1 - (int16_t)(normalized * (SCOPE_H - 1));
    if (py >= SCOPE_Y && py <= SCOPE_Y + SCOPE_H - 1) {
      // Подпись слева
      u8g2.drawStr(0, py - 2, tick_labels[t]);
      // Точечная линия
      drawDottedHLine(SCOPE_X, py, SCOPE_W);
    }
  }
  
  // Децимация: буфер → экран
  if (samples == 0) samples = ADC_RING_SIZE;
  uint32_t decimation = samples / SCOPE_W;
  if (decimation < 1) decimation = 1;
  
  uint32_t buf_end = adc_write_index;
  uint32_t buf_start = (buf_end + ADC_RING_SIZE - samples - start_offset) % ADC_RING_SIZE;
  
  int16_t prev_py = -1;
  for (uint8_t x = 0; x < SCOPE_W; x++) {
    uint32_t idx = (buf_start + x * decimation) % ADC_RING_SIZE;
    int16_t raw = adc_ring_buffer[idx];
    if (raw == ADC_INVALID_VALUE) continue;
    
    float mA = adcSignedToMilliamps(raw);
    
    // Нормализация в пиксели
    float normalized = (mA - y_min) / y_range;
    int16_t py = SCOPE_Y + SCOPE_H - 1 - (int16_t)(normalized * (SCOPE_H - 1));
    if (py < SCOPE_Y) py = SCOPE_Y;
    if (py > SCOPE_Y + SCOPE_H - 1) py = SCOPE_Y + SCOPE_H - 1;
    
    // Рисуем линию от предыдущей точки
    if (prev_py >= 0) {
      u8g2.drawLine(SCOPE_X + x - 1, prev_py, SCOPE_X + x, py);
    }
    prev_py = py;
  }
}

// Вычисление RMS и среднего из буфера
static void calcBufferStats(float* mean_mA, float* rms_mA) {
  double sum = 0, sum_sq = 0;
  uint32_t count = 0;
  
  for (uint32_t i = 0; i < ADC_RING_SIZE; i++) {
    int16_t raw = adc_ring_buffer[i];
    if (raw == ADC_INVALID_VALUE) continue;
    float mA = adcSignedToMilliamps(raw);
    sum += mA;
    sum_sq += mA * mA;
    count++;
  }
  
  if (count > 0) {
    *mean_mA = sum / count;
    *rms_mA = sqrtf(sum_sq / count);
  } else {
    *mean_mA = 0;
    *rms_mA = 0;
  }
}

// Строка метрик + тонкий прогресс-бар внизу
static void drawMetricsAndProgress(const char* metric_str) {
  extern float dynamic_dac_gain;
  
  // Метрики: x1.0  0.9mA  12:24
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  char line[32];
  
  uint32_t elapsed_sec = (millis() - session_timer_start_ms) / 1000;
  uint16_t minutes = elapsed_sec / 60;
  uint8_t seconds = elapsed_sec % 60;
  if (minutes > 99) { minutes = 99; seconds = 59; }
  
  snprintf(line, sizeof(line), "x%.1f   %s    %02u:%02u", dynamic_dac_gain, metric_str, minutes, seconds);
  u8g2.drawStr(0, 52, line);
  
  // Тонкий прогресс-бар (1 пиксель) внизу экрана
  uint16_t duration_min = current_settings.duration_tRNS_min;
  switch (current_settings.mode) {
    case MODE_TDCS: duration_min = current_settings.duration_tDCS_min; break;
    case MODE_TACS: duration_min = current_settings.duration_tACS_min; break;
    default: break;
  }
  uint32_t total_sec = duration_min * 60;
  float progress = (total_sec > 0) ? ((float)elapsed_sec / total_sec) : 0;
  if (progress > 1.0f) progress = 1.0f;
  
  uint8_t fill_w = (uint8_t)(progress * 128);
  u8g2.drawHLine(0, 63, fill_w);
}

// === ДАШБОРД tRNS ===
static void renderDashboardTRNS() {
  float amp = current_settings.amplitude_tRNS_mA;
  
  // Строка 1: Конфиг
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  char title[28];
  snprintf(title, sizeof(title), "hf-tRNS %.1fmA %um", amp, current_settings.duration_tRNS_min);
  u8g2.drawStr(0, 0, title);
  
  // Осциллограмма: ylim = ±amp*1.2, тики на ±amp и 0
  float ticks[] = { amp, 0, -amp };
  char tick_plus[8], tick_zero[] = "0", tick_minus[8];
  snprintf(tick_plus, sizeof(tick_plus), "%.1f", amp);
  snprintf(tick_minus, sizeof(tick_minus), "%.1f", -amp);
  const char* labels[] = { tick_plus, tick_zero, tick_minus };
  
  drawOscilloscope(-amp * 1.2f, amp * 1.2f, ticks, labels, 3, 0, 0);
  
  // Метрики: 3σ
  float mean_mA, rms_mA;
  calcBufferStats(&mean_mA, &rms_mA);
  float sigma = sqrtf(rms_mA * rms_mA - mean_mA * mean_mA);
  
  char metric[16];
  snprintf(metric, sizeof(metric), "%.1fmA", sigma * 3);
  drawMetricsAndProgress(metric);
}

// === ДАШБОРД tDCS ===
static void renderDashboardTDCS() {
  float amp = current_settings.amplitude_tDCS_mA;
  
  // Строка 1: Конфиг
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  char title[24];
  snprintf(title, sizeof(title), "tDCS %.1fmA %um", amp, current_settings.duration_tDCS_min);
  u8g2.drawStr(0, 0, title);
  
  // Осциллограмма УНИПОЛЯРНАЯ: ylim = [0, amp*1.2], тики на amp и 0
  float ticks[] = { amp, 0 };
  char tick_amp[8], tick_zero[] = "0";
  snprintf(tick_amp, sizeof(tick_amp), "%.1f", amp);
  const char* labels[] = { tick_amp, tick_zero };
  
  drawOscilloscope(-amp * 0.1f, amp * 1.2f, ticks, labels, 2, 0, 0);
  
  // Метрики: средний ток
  float mean_mA, rms_mA;
  calcBufferStats(&mean_mA, &rms_mA);
  
  char metric[16];
  snprintf(metric, sizeof(metric), "%.1fmA", mean_mA);
  drawMetricsAndProgress(metric);
}

// === ДАШБОРД tACS ===
static void renderDashboardTACS() {
  float amp = current_settings.amplitude_tACS_mA;
  float freq = current_settings.frequency_tACS_Hz;
  
  // Строка 1: Конфиг
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  char title[28];
  snprintf(title, sizeof(title), "tACS %.0fHz %.1fmA %um", freq, amp, current_settings.duration_tACS_min);
  u8g2.drawStr(0, 0, title);
  
  // Синхронизация по фронту: ищем 2 периода от конца буфера
  uint32_t period_samples = (uint32_t)(ADC_SAMPLE_RATE / freq);
  uint32_t two_periods = period_samples * 2;
  
  // Поиск фронта от конца буфера
  uint32_t start_offset = 0;
  uint32_t search_start = (adc_write_index + ADC_RING_SIZE - two_periods - 100) % ADC_RING_SIZE;
  int16_t prev_raw = adc_ring_buffer[search_start];
  uint8_t crossings_found = 0;
  
  for (uint32_t i = 1; i < two_periods + 100 && crossings_found < 3; i++) {
    uint32_t idx = (search_start + i) % ADC_RING_SIZE;
    int16_t raw = adc_ring_buffer[idx];
    if (raw == ADC_INVALID_VALUE || prev_raw == ADC_INVALID_VALUE) {
      prev_raw = raw;
      continue;
    }
    if (prev_raw < 0 && raw >= 0) {
      crossings_found++;
      if (crossings_found == 1) {
        start_offset = ADC_RING_SIZE - (search_start + i - adc_write_index + ADC_RING_SIZE) % ADC_RING_SIZE;
      }
    }
    prev_raw = raw;
  }
  
  // Осциллограмма: 2 периода, ylim = ±amp*1.2, тики на ±amp и 0
  float ticks[] = { amp, 0, -amp };
  char tick_plus[8], tick_zero[] = "0", tick_minus[8];
  snprintf(tick_plus, sizeof(tick_plus), "%.1f", amp);
  snprintf(tick_minus, sizeof(tick_minus), "%.1f", -amp);
  const char* labels[] = { tick_plus, tick_zero, tick_minus };
  
  drawOscilloscope(-amp * 1.2f, amp * 1.2f, ticks, labels, 3, two_periods, start_offset);
  
  // Метрики: амплитуда
  float mean_mA, rms_mA;
  calcBufferStats(&mean_mA, &rms_mA);
  float amplitude_mA = sqrtf(rms_mA * rms_mA - mean_mA * mean_mA) * 1.414f;
  
  char metric[16];
  snprintf(metric, sizeof(metric), "%.1fmA", amplitude_mA);
  drawMetricsAndProgress(metric);
}

// Принудительное обновление дисплея — ветвление по режиму
void refreshDisplay() {
  u8g2.clearBuffer();
  
  switch (current_settings.mode) {
    case MODE_TRNS:
      renderDashboardTRNS();
      break;
    case MODE_TDCS:
      renderDashboardTDCS();
      break;
    case MODE_TACS:
      renderDashboardTACS();
      break;
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
  } else if (editor_data.increment < 0.1f) {
    // Мелкий шаг (0.01) — показываем 2 знака после запятой
    snprintf(value_str, sizeof(value_str), "%.2f", editor_temp_value);
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
        snprintf(amp_str, sizeof(amp_str), "Ток: %.1fмА", current_settings.amplitude_tDCS_mA);
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
        static char dac_str[32], fade_str[32], adc_str[32], trns_str[32], pol_str[32], enc_str[32];
        snprintf(enc_str, sizeof(enc_str), "Энкодер: %s", current_settings.enc_direction_invert ? "Инв." : "Норм.");
        snprintf(pol_str, sizeof(pol_str), "Полярность: %s", current_settings.polarity_invert ? "Инв." : "Норм.");
        snprintf(dac_str, sizeof(dac_str), "DAC коды/мА: %.0f", current_settings.dac_code_to_mA);
        snprintf(fade_str, sizeof(fade_str), "Плавный пуск: %.0fs", current_settings.fade_duration_sec);
        snprintf(adc_str, sizeof(adc_str), "ADC mult: %.2f", current_settings.adc_multiplier);
        snprintf(trns_str, sizeof(trns_str), "tRNS mult: %.2f", current_settings.trns_multiplier);        
        const char* choices[] = { "<-Назад", enc_str, pol_str, dac_str, fade_str, adc_str, trns_str, "СБРОС на заводские" };
        renderMenu("НАСТРОЙКИ", choices, 8, menu_selected);
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


