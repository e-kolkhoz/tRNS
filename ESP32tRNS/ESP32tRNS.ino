// ============================================================================
// === ESP32-S3FH4R2 ===
// ============================================================================
//
// Цикл загрузки:
//   1. PresetDsl::scanAll() — FFat, список валидных YAML-пресетов (+ errors.log).
//   2. USBFlash::mount() — USB MSC, раздел как флешка на ПК.
//   3. После успешного mount — I2C OLED + энкодер.
//
// Neopixel:
//   синий пульс — жив
//   красный     — MSC не поднялся
//   жёлтый      — MSC ок, валидных пресетов нет
//   сиреневый   — MSC ок + есть хотя бы один валидный пресет

#include "config.h"
#include "version.h"
#include "preset_dsl.h"
#include "usb_flash.h"
#include "dac_control.h"
#include "adc_control.h"

#include <Wire.h>
#include <U8g2lib.h>
#include <EncButton.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "boot_control.h"

static std::vector<PresetDefinition> g_presets;
static Preferences g_pref;

#if OLED_IS_SH1106
static U8G2_SH1106_128X64_NONAME_F_HW_I2C  oled(U8G2_R0, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif

static EncButton enc(ENC_A, ENC_B, ENC_S);

enum ScreenType : uint8_t {
  SCR_MAIN_MENU = 0,
  SCR_PRESET_MENU,   // единое меню пресета; наполнение ветвится по PresetType
  SCR_SETTINGS_MENU,
  SCR_EDITOR,
  SCR_DASHBOARD,
  SCR_CONFIRM,
  SCR_FINISH,
};

enum SessionState : uint8_t {
  STATE_IDLE = 0,
  STATE_FADEIN,
  STATE_STABLE,
  STATE_FADEOUT,
};

enum DashboardView : uint8_t {
  DASH_LEFT = 0,
  DASH_RIGHT = 1,
  DASH_BOTH = 2,
};

struct UiSettings {
  bool enc_reverse = true;
};

struct PresetRuntime {
  float amplitude_mA = 1.0f;
  float frequency_hz = 140.0f;
  float duration_min = 20.0f;
  float fade_in_sec = 10.0f;
  float fade_out_sec = 10.0f;
  bool initialized = false;
};

struct EditorData {
  const char* title = "";
  float* value = nullptr;
  float min_v = 0.0f;
  float max_v = 1.0f;
  float step = 0.1f;
  bool as_int = false;
};

// Единая декларация пункта пресет-меню (R.10): один источник для отрисовки,
// обработки клика и подсчёта длины меню.
enum PresetItemKind : uint8_t { PMI_START, PMI_PARAM, PMI_BACK };
struct PresetMenuEntry {
  PresetItemKind kind;
  const char* editor_title;  // для PMI_PARAM
  float* value;              // для PMI_PARAM
  float min_v, max_v, step;
  bool as_int;
};

static constexpr uint8_t SCREEN_STACK_MAX = 4;  // глубина навигационного стека
static constexpr uint8_t MENU_MAX_PRESETS = 12; // максимум пресетов в главном меню
static constexpr uint8_t PRESET_MENU_MAX_ITEMS = 8;

static UiSettings ui;
static std::vector<PresetRuntime> g_runtime;
static EditorData editor;
static float editor_temp = 0.0f;
static ScreenType screen_stack[SCREEN_STACK_MAX] = { SCR_MAIN_MENU };
static uint8_t stack_depth = 0;
static uint8_t menu_selected = 0;
static int active_preset_idx = -1;
static SessionState session_state = STATE_IDLE;
static DashboardView dashboard_view = DASH_BOTH;
static bool session_just_finished = false;
static uint32_t session_state_start_ms = 0;
static uint32_t session_started_ms = 0;
static uint32_t session_elapsed_sec = 0;
static float fadeout_start_gain = 1.0f;
static float tacs_actual_hz = 140.0f;
static float session_amp_mA = 1.0f;
static float session_freq_hz = 0.0f;
static float session_duration_min = 20.0f;
static float session_fade_in_sec = 10.0f;
static float session_fade_out_sec = 10.0f;
static bool session_channels_both = false;
static PresetType session_type = PresetType::CONST_DC;
static FeedbackBase session_feedback_base = FeedbackBase::MEAN;
static float session_feedback_coeff = 1.0f;
static uint32_t session_scope_window = 0;  // окно осциллографа в ADC-семплах (R.8)
static String session_name = "preset";

static int g_bat_pct_cached = 100;  // R.3: заряд, измеренный вне сеанса (ADC1 vs continuous)

static inline ScreenType currentScreen() { return screen_stack[stack_depth]; }

// Сброс навигации на конкретный корневой экран (вместо ручного stack_depth=0 + присваивания).
static void resetToScreen(ScreenType scr) {
  stack_depth = 0;
  screen_stack[0] = scr;
  menu_selected = 0;
}

static void go_sleep();

static uint32_t hash32(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

static String keyForParam(const String& preset_id, const char* tag) {
  char key[16];
  snprintf(key, sizeof(key), "%s_%08lx", tag, (unsigned long)hash32(preset_id));
  return String(key);
}

static float loadParamNvs(const String& preset_id, const char* tag, float def_v, float min_v, float max_v) {
  String k = keyForParam(preset_id, tag);
  float v = g_pref.getFloat(k.c_str(), def_v);
  if (v < min_v) v = min_v;
  if (v > max_v) v = max_v;
  return v;
}

static void saveParamNvs(const String& preset_id, const char* tag, float v) {
  String k = keyForParam(preset_id, tag);
  g_pref.putFloat(k.c_str(), v);
}

// --- Напряжение батареи: делитель R_top=100k / R_bot=360k ---
// V_bat = V_adc * (R_top + R_bot) / R_bot = V_adc * 460/360
// V_adc = raw * 3.3 / 4095
static float read_bat_v() {
  return analogRead(PLUS_BAT_ADC) * (3.3f / 4095.0f) * (460.0f / 360.0f);
}

static int read_bat_pct() {
  const float v = read_bat_v();
  // Грубая линейная оценка Li-ion: 3.30V..4.20V -> 0..100%
  int pct = (int)((v - 3.30f) * 100.0f / 0.90f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// R.3: analogRead батареи (ADC1) конфликтует с adc_continuous (ADC1), поэтому реальное
// измерение делаем только когда захват ADC остановлен; в сеансе показываем кэш.
static void updateBatteryCacheIfSafe() {
  if (!AdcControl::isRunning()) g_bat_pct_cached = read_bat_pct();
}
static int batteryPct() { return g_bat_pct_cached; }

static void format_adc_stats(char* buf, size_t len, char tag, const AdcChannelStats& st) {
  if (!st.valid) {
    snprintf(buf, len, "%c:----", tag);
  } else {
    snprintf(buf, len, "%c:%.2f>%.2f<%.2f|%.4f",
             tag, st.min_v, st.mean_v, st.max_v, st.std_v);
  }
}

static PresetRuntime& runtimeForPreset(int idx) {
  if ((int)g_runtime.size() <= idx) g_runtime.resize(idx + 1);
  PresetRuntime& rt = g_runtime[idx];
  if (!rt.initialized && idx >= 0 && idx < (int)g_presets.size()) {
    const PresetDefinition& p = g_presets[idx];
    rt.amplitude_mA = loadParamNvs(p.id, "amp", p.amplitude_mA.default_v, p.amplitude_mA.min_v, p.amplitude_mA.max_v);
    rt.duration_min = loadParamNvs(p.id, "dur", p.duration_min.default_v, p.duration_min.min_v, p.duration_min.max_v);
    rt.fade_in_sec = loadParamNvs(p.id, "fin", p.fade_in_sec.default_v, p.fade_in_sec.min_v, p.fade_in_sec.max_v);
    rt.fade_out_sec = loadParamNvs(p.id, "fout", p.fade_out_sec.default_v, p.fade_out_sec.min_v, p.fade_out_sec.max_v);
    if (p.type == PresetType::SIN && p.frequency_hz.valid) {
      rt.frequency_hz = loadParamNvs(p.id, "frq", p.frequency_hz.default_v, p.frequency_hz.min_v, p.frequency_hz.max_v);
    }
    rt.initialized = true;
  }
  return rt;
}

static void saveRuntimeForPreset(int idx) {
  if (idx < 0 || idx >= (int)g_presets.size() || idx >= (int)g_runtime.size()) return;
  const PresetDefinition& p = g_presets[idx];
  const PresetRuntime& rt = g_runtime[idx];
  saveParamNvs(p.id, "amp", rt.amplitude_mA);
  saveParamNvs(p.id, "dur", rt.duration_min);
  saveParamNvs(p.id, "fin", rt.fade_in_sec);
  saveParamNvs(p.id, "fout", rt.fade_out_sec);
  if (p.type == PresetType::SIN && p.frequency_hz.valid) {
    saveParamNvs(p.id, "frq", rt.frequency_hz);
  }
}

static void pushScreen(ScreenType scr) {
  if (stack_depth < SCREEN_STACK_MAX - 1) {
    stack_depth++;
    screen_stack[stack_depth] = scr;
    menu_selected = 0;
  }
}

static void popScreen() {
  if (stack_depth > 0) {
    stack_depth--;
    menu_selected = 0;
  }
}

static void openEditor(const char* title, float* ptr, float min_v, float max_v, float step, bool as_int) {
  editor.title = title;
  editor.value = ptr;
  editor.min_v = min_v;
  editor.max_v = max_v;
  editor.step = step;
  editor.as_int = as_int;
  editor_temp = *ptr;
  pushScreen(SCR_EDITOR);
}

static void stopSessionNow() {
  DacControl::stop();
  AdcControl::stop();
  session_state = STATE_IDLE;
}

static void beginFadeOut() {
  if (session_state == STATE_FADEOUT || session_state == STATE_IDLE) return;
  fadeout_start_gain = DacControl::gain();
  session_state = STATE_FADEOUT;
  session_state_start_ms = millis();
}

static void startSession(const PresetDefinition& preset, const PresetRuntime& rt) {
  session_just_finished = false;
  dashboard_view = DASH_BOTH;
  session_name = preset.name;
  session_type = preset.type;
  session_amp_mA = rt.amplitude_mA;
  session_duration_min = rt.duration_min;
  session_fade_in_sec = rt.fade_in_sec;
  session_fade_out_sec = rt.fade_out_sec;
  session_freq_hz = rt.frequency_hz;
  session_channels_both = preset.channels_both;
  session_feedback_base = preset.feedback_base;
  session_feedback_coeff = preset.feedback_coeff;

  DacProgram p{};
  p.amp_l_ma = session_amp_mA;
  p.amp_r_ma = session_channels_both ? session_amp_mA : 0.0f;
  p.sample_rate_hz = preset.sample_rate_hz;
  if (preset.type == PresetType::CONST_DC) {
    p.waveform = DacWaveform::CONST_DC;
    DacControl::setProgram(p);
  } else if (preset.type == PresetType::SIN) {
    p.waveform = DacWaveform::SIN;
    p.target_freq_hz = session_freq_hz;
    DacControl::setProgram(p);
  } else {
    if (preset.wav_left_samples.empty() || preset.wav_right_samples.empty()) {
      return;
    }
    const int16_t* wav_l = preset.wav_left_samples.data();
    const int16_t* wav_r = preset.wav_right_samples.data();
    size_t wav_n = preset.wav_left_samples.size();

    // Маппинг по требованиям:
    // mono+left -> только левый; mono+both -> копия в оба канала
    // stereo+left -> берём только левый; stereo+both -> L/R как в файле
    std::vector<int16_t> zero;
    if (!session_channels_both) {
      zero.assign(wav_n, 0);
      wav_r = zero.data();
    } else if (preset.wav_channels == 1) {
      wav_r = wav_l; // копия mono в оба канала
    }

    if (!DacControl::setCustomWaveStereo(wav_l, wav_r, wav_n, p.amp_l_ma, p.amp_r_ma, preset.sample_rate_hz)) {
      return;
    }
  }
  tacs_actual_hz = DacControl::program().actual_freq_hz;

  // Окно осциллографа по типу пресета (R.8), в семплах ADC (ADC_OUT_RATE_HZ):
  //   SIN  -> two_periods; WAV -> one_period (луп); CONST -> no_sync (весь буфер).
  if (preset.type == PresetType::SIN && tacs_actual_hz > 0.1f) {
    session_scope_window = (uint32_t)(2.0f * ADC_OUT_RATE_HZ / tacs_actual_hz);
  } else if (preset.type == PresetType::WAV && preset.sample_rate_hz > 0) {
    float loop_sec = (float)preset.wav_left_samples.size() / (float)preset.sample_rate_hz;
    session_scope_window = (uint32_t)(loop_sec * ADC_OUT_RATE_HZ);
  } else {
    session_scope_window = 0;  // CONST: без синхронизации
  }

  DacControl::setGain(0.0f);
  DacControl::start();
  AdcControl::start();

  session_state = STATE_FADEIN;
  session_state_start_ms = millis();
  session_started_ms = session_state_start_ms;
  session_elapsed_sec = 0;
  resetToScreen(SCR_DASHBOARD);
}

static void updateSessionState() {
  if (session_state == STATE_IDLE) return;
  uint32_t now = millis();
  float elapsed_state = (now - session_state_start_ms) / 1000.0f;
  if (session_fade_in_sec < 0.2f) session_fade_in_sec = 0.2f;
  if (session_fade_out_sec < 0.2f) session_fade_out_sec = 0.2f;

  if (session_state == STATE_FADEIN) {
    float g = elapsed_state / session_fade_in_sec;
    if (g >= 1.0f) {
      g = 1.0f;
      session_state = STATE_STABLE;
      session_state_start_ms = now;
    }
    DacControl::setGain(g);
    return;
  }

  if (session_state == STATE_STABLE) {
    DacControl::setGain(1.0f);
    float total = session_duration_min * 60.0f;
    float stable = total - session_fade_in_sec - session_fade_out_sec;
    if (stable < 0.0f) stable = 0.0f;
    if (elapsed_state >= stable) {
      fadeout_start_gain = 1.0f;
      session_state = STATE_FADEOUT;
      session_state_start_ms = now;
    }
    return;
  }

  if (session_state == STATE_FADEOUT) {
    float fade_t = fadeout_start_gain * session_fade_out_sec;
    if (fade_t < 0.1f) fade_t = 0.1f;
    float g = fadeout_start_gain * (1.0f - elapsed_state / fade_t);
    if (g <= 0.0f) {
      g = 0.0f;
      DacControl::setGain(g);
      stopSessionNow();
      session_elapsed_sec = (millis() - session_started_ms) / 1000;
      session_just_finished = true;
      resetToScreen(SCR_FINISH);
      return;
    }
    DacControl::setGain(g);
  }
}

static void drawBatteryBar() {
  int pct = batteryPct();
  int w = (pct * 128) / 100;
  if (w > 0) {
    oled.drawHLine(0, 0, w);
  }
}

static void renderMenu(const char* title, const char* choices[], uint8_t count) {
  const uint8_t max_vis = 5;
  const uint8_t item_h = 10;
  const uint8_t y_start = 12;
  uint8_t scroll = 0;
  if (count > max_vis && menu_selected >= max_vis) {
    scroll = menu_selected - max_vis + 1;
    if (scroll > count - max_vis) scroll = count - max_vis;
  }

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  drawBatteryBar();
  oled.drawStr(0, 2, title);

  uint8_t vis = (count < max_vis) ? count : max_vis;
  for (uint8_t i = 0; i < vis; i++) {
    uint8_t idx = scroll + i;
    uint8_t y = y_start + i * item_h;
    if (idx == menu_selected) oled.drawStr(0, y, ">");
    oled.drawStr(10, y, choices[idx]);
  }

  if (count > max_vis) {
    if (scroll > 0) oled.drawTriangle(124, 14, 120, 18, 128, 18);
    if (scroll < count - max_vis) oled.drawTriangle(124, 62, 120, 58, 128, 58);
  }
  oled.sendBuffer();
}

// --- Осциллограф дашборда (R.6/R.8) ---
#define DASH_SCOPE_X   16
#define DASH_SCOPE_W   112
#define DASH_SCOPE_Y   12
#define DASH_SCOPE_H   36

static float feedbackForChannel(const AdcChannelStats& st, bool is_left) {
  if (!st.valid) return 0.0f;
  const float offset_v = is_left ? DEF_ADC_OFFSET_L_V : DEF_ADC_OFFSET_R_V;
  float base;
  if (session_feedback_base == FeedbackBase::STD) base = st.std_v * DEF_ADC_V_TO_MA;
  else base = fabsf(st.mean_v - offset_v) * DEF_ADC_V_TO_MA;
  return base * session_feedback_coeff;
}

static void drawDottedHLine(int x, int y, int w) {
  for (int i = 0; i < w; i += 4) oled.drawPixel(x + i, y);
}

// Рисует осциллограмму одного канала: линейная V->mA, тики по амплитуде.
// CONST -> униполярная развёртка, SIN/WAV -> биполярная.
static void drawScopeChannel(bool left) {
  const float amp = session_amp_mA;
  const bool bipolar = (session_type != PresetType::CONST_DC);
  const float y_min = bipolar ? -amp * 1.2f : -amp * 0.1f;
  const float y_max = amp * 1.2f;
  float y_range = y_max - y_min;
  if (y_range < 0.01f) y_range = 0.01f;

  oled.setFont(u8g2_font_4x6_tf);
  const int nticks = bipolar ? 3 : 2;
  const float ticks[3] = { amp, 0.0f, -amp };
  for (int t = 0; t < nticks; t++) {
    float nrm = (ticks[t] - y_min) / y_range;
    int py = DASH_SCOPE_Y + DASH_SCOPE_H - 1 - (int)(nrm * (DASH_SCOPE_H - 1));
    if (py < DASH_SCOPE_Y || py > DASH_SCOPE_Y + DASH_SCOPE_H - 1) continue;
    char lbl[8];
    snprintf(lbl, sizeof(lbl), "%.1f", ticks[t]);
    oled.drawStr(0, py - 2, lbl);
    drawDottedHLine(DASH_SCOPE_X, py, DASH_SCOPE_W);
  }

  static float trace[DASH_SCOPE_W];
  if (!AdcControl::scopeTrace(left, trace, DASH_SCOPE_W, session_scope_window, 0)) return;
  const float offset = left ? DEF_ADC_OFFSET_L_V : DEF_ADC_OFFSET_R_V;
  int prev_py = -1;
  for (int x = 0; x < DASH_SCOPE_W; x++) {
    float ma = (trace[x] - offset) * DEF_ADC_V_TO_MA;
    float nrm = (ma - y_min) / y_range;
    int py = DASH_SCOPE_Y + DASH_SCOPE_H - 1 - (int)(nrm * (DASH_SCOPE_H - 1));
    if (py < DASH_SCOPE_Y) py = DASH_SCOPE_Y;
    if (py > DASH_SCOPE_Y + DASH_SCOPE_H - 1) py = DASH_SCOPE_Y + DASH_SCOPE_H - 1;
    if (prev_py >= 0) oled.drawLine(DASH_SCOPE_X + x - 1, prev_py, DASH_SCOPE_X + x, py);
    prev_py = py;
  }
}

// Нижняя строка-показометр: усиление gain, метрика фидбэка (мА), время + прогресс-бар 1px.
static void drawMeterAndProgress(const char* metric) {
  oled.setFont(u8g2_font_6x12_tf);
  uint32_t elapsed = (millis() - session_started_ms) / 1000;
  uint16_t mm = elapsed / 60;
  uint8_t ss = elapsed % 60;
  char line[32];
  snprintf(line, sizeof(line), "x%.1f %s %02u:%02u", DacControl::gain(), metric, mm, ss);
  oled.drawStr(0, 52, line);

  float total = session_duration_min * 60.0f;
  float progress = (total > 0.0f) ? (float)elapsed / total : 0.0f;
  if (progress > 1.0f) progress = 1.0f;
  oled.drawHLine(0, 63, (int)(progress * 128));
}

static void drawDashboard() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);

  AdcChannelStats l = AdcControl::statsLeft();
  AdcChannelStats r = AdcControl::statsRight();
  const float fb_l = feedbackForChannel(l, true);
  const float fb_r = feedbackForChannel(r, false);

  char header[42], metric[20];
  if (dashboard_view == DASH_BOTH) {
    // L+R: компактные метрики без графики (SRS 5.1) + числовой заряд (SRS UI батареи).
    if (session_type == PresetType::SIN) {
      snprintf(header, sizeof(header), "%.9s %.0fHz", session_name.c_str(), tacs_actual_hz);
    } else {
      snprintf(header, sizeof(header), "%.13s", session_name.c_str());
    }
    oled.drawStr(0, 0, header);
    char batt[10];
    snprintf(batt, sizeof(batt), "%d%%", batteryPct());
    oled.drawStr(104, 0, batt);

    oled.setFont(u8g2_font_5x7_tf);
    char lbuf[28], rbuf[28];
    format_adc_stats(lbuf, sizeof(lbuf), 'L', l);
    format_adc_stats(rbuf, sizeof(rbuf), 'R', r);
    oled.drawStr(0, 14, lbuf);
    oled.drawStr(0, 24, rbuf);
    char fb_buf[30];
    snprintf(fb_buf, sizeof(fb_buf), "FB L:%.2f R:%.2f mA", fb_l, fb_r);
    oled.drawStr(0, 38, fb_buf);
    snprintf(metric, sizeof(metric), "%.2fmA", session_amp_mA);
    drawMeterAndProgress(metric);
    oled.sendBuffer();
    return;
  }

  // L или R: осциллограмма канала + метрика этого канала.
  const bool left = (dashboard_view == DASH_LEFT);
  if (session_type == PresetType::SIN) {
    snprintf(header, sizeof(header), "%c %.9s %.0fHz", left ? 'L' : 'R', session_name.c_str(), tacs_actual_hz);
  } else {
    snprintf(header, sizeof(header), "%c %.13s", left ? 'L' : 'R', session_name.c_str());
  }
  oled.drawStr(0, 0, header);

  drawScopeChannel(left);
  snprintf(metric, sizeof(metric), "%.3fmA", left ? fb_l : fb_r);
  drawMeterAndProgress(metric);
  oled.sendBuffer();
}

static void drawConfirm() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 0, "Остановить сеанс?");
  if (menu_selected == 0) {
    oled.drawStr(0, 24, "> Нет, продолжить");
    oled.drawStr(0, 38, "  Да, плавный стоп");
  } else {
    oled.drawStr(0, 24, "  Нет, продолжить");
    oled.drawStr(0, 38, "> Да, плавный стоп");
  }
  oled.sendBuffer();
}

static void drawEditor() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 0, editor.title);
  oled.setFont(u8g2_font_9x15_tf);
  char v[20];
  if (editor.as_int) snprintf(v, sizeof(v), "%d", (int)lroundf(editor_temp));
  else snprintf(v, sizeof(v), "%.1f", editor_temp);
  oled.drawStr(30, 24, v);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 54, "click=save");
  oled.sendBuffer();
}

static void drawFinish() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13_tf);
  oled.drawStr(0, 0, "СЕАНС ЗАВЕРШЕН");
  oled.setFont(u8g2_font_6x12_tf);
  char line[28];
  snprintf(line, sizeof(line), "%.14s %.1fmA", session_name.c_str(), session_amp_mA);
  oled.drawStr(0, 22, line);
  uint32_t mins = session_elapsed_sec / 60;
  uint32_t secs = session_elapsed_sec % 60;
  snprintf(line, sizeof(line), "%u:%02u", (unsigned)mins, (unsigned)secs);
  oled.drawStr(0, 38, line);
  oled.drawStr(0, 54, "click=menu");
  oled.sendBuffer();
}

// Заполняет декларативный список пунктов пресет-меню (R.10).
// items/labels — буферы длиной PRESET_MENU_MAX_ITEMS; возвращает число пунктов.
static int buildPresetMenu(const PresetDefinition& p, PresetRuntime& rt,
                           PresetMenuEntry* items, char labels[][28]) {
  int n = 0;
  items[n] = { PMI_START, nullptr, nullptr, 0, 0, 0, false };
  snprintf(labels[n], 28, "СТАРТ"); n++;

  items[n] = { PMI_PARAM, "Амплитуда, мА", &rt.amplitude_mA,
               p.amplitude_mA.min_v, p.amplitude_mA.max_v, p.amplitude_mA.step, false };
  snprintf(labels[n], 28, "Ампл.: %.1fмА", rt.amplitude_mA); n++;

  if (p.type == PresetType::SIN) {
    items[n] = { PMI_PARAM, "Частота, Гц", &rt.frequency_hz,
                 p.frequency_hz.min_v, p.frequency_hz.max_v, p.frequency_hz.step, false };
    snprintf(labels[n], 28, "Частота: %.1fГц", rt.frequency_hz); n++;
  }

  items[n] = { PMI_PARAM, "Длительность, мин", &rt.duration_min,
               p.duration_min.min_v, p.duration_min.max_v, p.duration_min.step, true };
  snprintf(labels[n], 28, "Длит.: %.0fмин", rt.duration_min); n++;

  items[n] = { PMI_PARAM, "Плавный старт, с", &rt.fade_in_sec,
               p.fade_in_sec.min_v, p.fade_in_sec.max_v, p.fade_in_sec.step, true };
  snprintf(labels[n], 28, "Fade in: %.0fс", rt.fade_in_sec); n++;

  items[n] = { PMI_PARAM, "Плавный стоп, с", &rt.fade_out_sec,
               p.fade_out_sec.min_v, p.fade_out_sec.max_v, p.fade_out_sec.step, true };
  snprintf(labels[n], 28, "Fade out: %.0fс", rt.fade_out_sec); n++;

  items[n] = { PMI_BACK, nullptr, nullptr, 0, 0, 0, false };
  snprintf(labels[n], 28, "<-Назад"); n++;
  return n;
}

static void drawCurrentScreen() {
  char line0[32], line1[32], line2[32], line3[32];
  const char* choices[4] = { line0, line1, line2, line3 };
  switch (currentScreen()) {
    case SCR_MAIN_MENU: {
      int n = (int)g_presets.size();
      if (n > MENU_MAX_PRESETS) n = MENU_MAX_PRESETS;
      static char labels[MENU_MAX_PRESETS + 1][24];
      const char* items[MENU_MAX_PRESETS + 1];
      int cnt = 0;
      for (int i = 0; i < n; i++) {
        snprintf(labels[cnt], sizeof(labels[cnt]), "%.20s", g_presets[i].name.c_str());
        items[cnt] = labels[cnt];
        cnt++;
      }
      items[cnt++] = "Настройки";
      renderMenu(n > 0 ? "== Пресеты ==" : "== Главное меню ==", items, cnt);
      break;
    }
    case SCR_PRESET_MENU: {
      if (active_preset_idx < 0 || active_preset_idx >= (int)g_presets.size()) {
        popScreen();
        break;
      }
      const PresetDefinition& p = g_presets[active_preset_idx];
      PresetRuntime& rt = runtimeForPreset(active_preset_idx);
      PresetMenuEntry entries[PRESET_MENU_MAX_ITEMS];
      char labels[PRESET_MENU_MAX_ITEMS][28];
      const char* choices2[PRESET_MENU_MAX_ITEMS];
      int cnt = buildPresetMenu(p, rt, entries, labels);
      for (int i = 0; i < cnt; i++) choices2[i] = labels[i];
      char title[24];
      snprintf(title, sizeof(title), "== %.12s ==", p.name.c_str());
      renderMenu(title, choices2, cnt);
      break;
    }
    case SCR_SETTINGS_MENU:
      snprintf(line0, sizeof(line0), "<-Назад");
      snprintf(line1, sizeof(line1), "Энкодер: %s", ui.enc_reverse ? "Инв." : "Норм.");
      snprintf(line2, sizeof(line2), "UF2");
      snprintf(line3, sizeof(line3), "Сон");
      renderMenu("== Настройки ==", choices, 4);
      break;
    case SCR_DASHBOARD:
      drawDashboard();
      break;
    case SCR_CONFIRM:
      drawConfirm();
      break;
    case SCR_EDITOR:
      drawEditor();
      break;
    case SCR_FINISH:
      drawFinish();
      break;
  }
}

static void executeMainMenu() {
  int n = (int)g_presets.size();
  if (n > MENU_MAX_PRESETS) n = MENU_MAX_PRESETS;
  if (menu_selected >= (uint8_t)n) {  // последний пункт — "Настройки"
    pushScreen(SCR_SETTINGS_MENU);
    return;
  }
  active_preset_idx = (int)menu_selected;
  pushScreen(SCR_PRESET_MENU);
}

static void executePresetMenu() {
  if (active_preset_idx < 0 || active_preset_idx >= (int)g_presets.size()) return;
  const PresetDefinition& p = g_presets[active_preset_idx];
  PresetRuntime& rt = runtimeForPreset(active_preset_idx);
  PresetMenuEntry entries[PRESET_MENU_MAX_ITEMS];
  char labels[PRESET_MENU_MAX_ITEMS][28];
  int cnt = buildPresetMenu(p, rt, entries, labels);
  if (menu_selected >= (uint8_t)cnt) return;
  const PresetMenuEntry& e = entries[menu_selected];
  switch (e.kind) {
    case PMI_START: startSession(p, rt); break;
    case PMI_BACK:  popScreen(); break;
    case PMI_PARAM: openEditor(e.editor_title, e.value, e.min_v, e.max_v, e.step, e.as_int); break;
  }
}

static int maxMenuIndexForScreen(ScreenType scr) {
  if (scr == SCR_MAIN_MENU) {
    int n = (int)g_presets.size();
    if (n > MENU_MAX_PRESETS) n = MENU_MAX_PRESETS;
    return n; // индекс пункта "Настройки"
  }
  if (scr == SCR_SETTINGS_MENU) return 3;
  if (scr == SCR_CONFIRM) return 1;
  if (scr == SCR_FINISH) return 0;
  if (scr == SCR_EDITOR || scr == SCR_DASHBOARD) return 0;

  if (scr == SCR_PRESET_MENU) {
    if (active_preset_idx < 0 || active_preset_idx >= (int)g_presets.size()) return 0;
    const PresetDefinition& p = g_presets[active_preset_idx];
    PresetRuntime& rt = runtimeForPreset(active_preset_idx);
    PresetMenuEntry entries[PRESET_MENU_MAX_ITEMS];
    char labels[PRESET_MENU_MAX_ITEMS][28];
    int cnt = buildPresetMenu(p, rt, entries, labels);
    return cnt - 1;
  }
  return 0;
}

static void handleRotate(int8_t delta) {
  if (currentScreen() == SCR_DASHBOARD) {
    int v = (int)dashboard_view + delta;
    if (v < (int)DASH_LEFT) v = (int)DASH_LEFT;
    if (v > (int)DASH_BOTH) v = (int)DASH_BOTH;
    dashboard_view = (DashboardView)v;
    return;
  }
  if (currentScreen() == SCR_EDITOR) {
    editor_temp += delta * editor.step;
    if (editor_temp < editor.min_v) editor_temp = editor.min_v;
    if (editor_temp > editor.max_v) editor_temp = editor.max_v;
    return;
  }

  int8_t max_idx = maxMenuIndexForScreen(currentScreen());
  int8_t next = (int8_t)menu_selected + delta;
  if (next < 0) next = 0;
  if (next > max_idx) next = max_idx;
  menu_selected = (uint8_t)next;
}

static void handleClick() {
  switch (currentScreen()) {
    case SCR_MAIN_MENU:
      executeMainMenu();
      break;
    case SCR_PRESET_MENU:
      executePresetMenu();
      break;
    case SCR_SETTINGS_MENU:
      if (menu_selected == 0) popScreen();
      else if (menu_selected == 1) {
        ui.enc_reverse = !ui.enc_reverse;
        enc.setEncReverse(ui.enc_reverse);
      } else if (menu_selected == 2) {
        BootControl::rebootToUF2();
      } else if (menu_selected == 3) {
        go_sleep();
      }
      break;
    case SCR_EDITOR:
      if (editor.value) {
        *editor.value = editor_temp;
        saveRuntimeForPreset(active_preset_idx);
      }
      popScreen();
      break;
    case SCR_DASHBOARD:
      if (session_state != STATE_IDLE) {
        pushScreen(SCR_CONFIRM);
        menu_selected = 0;
      } else {
        resetToScreen(SCR_MAIN_MENU);
      }
      break;
    case SCR_CONFIRM:
      if (menu_selected == 1) {
        beginFadeOut();
      }
      popScreen();
      break;
    case SCR_FINISH:
      resetToScreen(SCR_MAIN_MENU);
      break;
  }
}

// --- Глубокий сон, пробуждение по нажатию ENC_S ---
static void go_sleep() {
  digitalWrite(EN_WAKEUP, LOW);            // выключаем аналоговые модули
  rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);     // гасим неопиксель
  oled.setPowerSave(1);                    // гасим экран
  // Ждём пока кнопка точно отпущена
  while (digitalRead(ENC_S) == LOW) { delay(10); }
  delay(200);  // дебаунс отпускания

  // CHRG: во сне ESP не подтягивает линию — на DMM виден только TP4054 / подтяжка на модуле
  rtc_gpio_init((gpio_num_t)CHRG_PIN);
  rtc_gpio_set_direction((gpio_num_t)CHRG_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_dis((gpio_num_t)CHRG_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)CHRG_PIN);
  rtc_gpio_isolate((gpio_num_t)CHRG_PIN);

  // ENC_S: RTC pull-up + ext1 wakeup (обычный INPUT_PULLUP во сне отваливается)
  rtc_gpio_init((gpio_num_t)ENC_S);
  rtc_gpio_set_direction((gpio_num_t)ENC_S, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)ENC_S);
  rtc_gpio_pulldown_dis((gpio_num_t)ENC_S);
  // ESP32-S3: ext1 (ANY_LOW = любой из пинов маски пошёл в LOW)
  esp_sleep_enable_ext1_wakeup(1ULL << ENC_S, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
  // после пробуждения ESP32 перезагружается, выполняет setup() заново
}

// --- Отрисовка меню ---
// ============================================================
static void neo_restore_idle() {
  if (session_state != STATE_IDLE) {
    rgbLedWrite(NEOPIXEL_PIN, 0, 8, 32);
  } else if (g_presets.empty()) {
    rgbLedWrite(NEOPIXEL_PIN, 40, 25, 0);
  } else {
    rgbLedWrite(NEOPIXEL_PIN, 30, 0, 30);
  }
}

void IRAM_ATTR enc_isr() {
  enc.tickISR();
}

static void init_enc_oled() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(OLED_I2C_HZ);
  AdcControl::init();
  oled.setI2CAddress(DISPLAY_ADDR << 1);
  oled.begin();
  oled.enableUTF8Print();
  oled.setFontPosTop();
  oled.setContrast(255);
  oled.setPowerSave(0);

  enc.setEncType(EB_STEP4_LOW);
  enc.setDebTimeout(120); // 120ms debounce — защита от дребезга механического энкодера
  enc.setEncReverse(true);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_S, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), enc_isr, CHANGE);
  enc.setEncReverse(ui.enc_reverse);

  drawCurrentScreen();
}

// ============================================================
void setup() {
  BootControl::init();   // снять GPIO hold если остался с прошлой сессии UF2

  // Аналоговые модули включаем сразу после старта
  pinMode(EN_WAKEUP, OUTPUT);
  digitalWrite(EN_WAKEUP, HIGH);

  pinMode(USB_DET, INPUT);
  pinMode(CHRG_PIN, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);
  g_pref.begin("preset_rt", false);

  DacControl::init();

  rgbLedWrite(NEOPIXEL_PIN, 0, 0, 40);
  delay(300);
  rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);

  delay(1500);

  g_presets = PresetDsl::scanAll();
  g_runtime.clear();

  if (!USBFlash::mount()) {
    rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);
    return;
  }

  if (!g_presets.empty()) {
    rgbLedWrite(NEOPIXEL_PIN, 30, 0, 30);
  } else {
    rgbLedWrite(NEOPIXEL_PIN, 40, 25, 0);
  }

  init_enc_oled();
  
}

void loop() {
  if (!USBFlash::isMounted()) {
    return;
  }

  updateSessionState();
  updateBatteryCacheIfSafe();
  enc.tick();

  bool ch = false;

  if (enc.right()) {
    handleRotate(+1);
    ch = true;
    rgbLedWrite(NEOPIXEL_PIN, 35, 0, 20);
  }
  if (enc.left()) {
    handleRotate(-1);
    ch = true;
    rgbLedWrite(NEOPIXEL_PIN, 0, 30, 35);
  }
  if (enc.click()) {
    handleClick();
    ch = true;
    rgbLedWrite(NEOPIXEL_PIN, 40, 40, 15);
  }

  if (session_just_finished) {
    session_just_finished = false;
    ch = true;
  }

  static uint32_t t_draw;
  if (ch || millis() - t_draw > 200) {
    t_draw = millis();
    drawCurrentScreen();
  }

  static uint32_t neo_idle;
  if (ch) {
    neo_idle = millis() + 120;
  } else if (neo_idle && (int32_t)(millis() - neo_idle) >= 0) {
    neo_idle = 0;
    neo_restore_idle();
  }
}
