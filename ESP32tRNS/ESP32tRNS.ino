// ============================================================================
// === ESP32-S3FH4R2 ===
// ============================================================================
//
// Цикл загрузки:
//   1. PresetDsl::scanAll() — FFat, список валидных YAML-пресетов (+ errors.log).
//   2. USBFlash::mount() — USB MSC, раздел как флешка на ПК.
//   3. OLED — splash «== ГРУЗИМСЯ ==» сразу после I2C; меню после mount; reconnect — R.14/SRS.
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
  SCR_CALIB_MENU,    // редактор калибровок (TODO #1)
  SCR_EDITOR,
  SCR_DASHBOARD,
  SCR_CONFIRM,
  SCR_FINISH,
  SCR_PRE_START,
  SCR_ERROR,         // экран ошибки старта (TODO #2/#3)
};

enum ConfirmKind : uint8_t {
  CONFIRM_STOP_SESSION = 0,
  CONFIRM_RESET_PRESETS,
  CONFIRM_RESET_CALIB,
  CONFIRM_RESET_ALL_NVS,
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
  float amp_l_mA = 1.0f;
  float amp_r_mA = 1.0f;
  float freq_l_hz = 140.0f;
  float freq_r_hz = 140.0f;
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
  bool is_cal = false;  // true -> при сохранении писать калибровку в NVS (TODO #1)
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
static constexpr uint8_t PRESET_MENU_MAX_ITEMS = 11;

static UiSettings ui;

static bool g_oled_ok = false;
static constexpr uint32_t OLED_PROBE_MS = 500;
static std::vector<PresetRuntime> g_runtime;
static EditorData editor;
static float editor_temp = 0.0f;
static ScreenType screen_stack[SCREEN_STACK_MAX] = { SCR_MAIN_MENU };
static uint8_t stack_depth = 0;
static uint8_t menu_selected = 0;
static ConfirmKind confirm_kind = CONFIRM_STOP_SESSION;
static int active_preset_idx = -1;
static SessionState session_state = STATE_IDLE;
static DashboardView dashboard_view = DASH_LEFT;
static bool session_just_finished = false;
static uint32_t session_state_start_ms = 0;
static uint32_t session_started_ms = 0;
static uint32_t session_elapsed_sec = 0;
static float fadeout_start_gain = 1.0f;
static float session_amp_l_mA = 1.0f;
static float session_amp_r_mA = 0.0f;
static float session_freq_l_hz = 0.0f;
static float session_freq_r_hz = 0.0f;
static float session_actual_freq_l_hz = 0.0f;
static float session_actual_freq_r_hz = 0.0f;
static float session_duration_min = 20.0f;
static float session_fade_in_sec = 10.0f;
static float session_fade_out_sec = 10.0f;
static bool session_channels_both = false;
static PresetType session_type = PresetType::CONST_DC;
static FeedbackBase session_feedback_base = FeedbackBase::MEAN;
static float session_feedback_coeff = 1.0f;
static uint32_t session_scope_period = 0;    // целый период L в ADC-семплах
static uint32_t session_scope_period_r = 0; // целый период R
static uint8_t session_scope_nper   = 0;      // сколько периодов показывать (0 = free-run)
static String session_name = "preset";

static int g_bat_pct_cached = 100;  // R.3: заряд, измеренный вне сеанса (ADC1 vs continuous)

// Калибровки (TODO #1). Дефолты из config.h, персист в NVS (fallback-on-read, R.1).
static float g_cal_voffset_l   = DEF_ADC_OFFSET_L_V;
static float g_cal_voffset_r   = DEF_ADC_OFFSET_R_V;
static float g_cal_v_to_ma_l   = DEF_ADC_V_TO_MA;
static float g_cal_v_to_ma_r   = DEF_ADC_V_TO_MA;
static float g_cal_dac_code_l  = DEF_DAC_CODE_TO_MA_L;
static float g_cal_dac_code_r  = DEF_DAC_CODE_TO_MA_R;

static char g_error_msg[40] = "";  // текст экрана ошибки старта (TODO #2/#3)

static inline ScreenType currentScreen() { return screen_stack[stack_depth]; }

static void syncEnWakeup() {
  const ScreenType scr = currentScreen();
  const bool on = (scr == SCR_PRE_START || scr == SCR_FINISH ||
                   (session_state != STATE_IDLE &&
                    (scr == SCR_DASHBOARD || scr == SCR_CONFIRM)));
  digitalWrite(EN_WAKEUP, on ? HIGH : LOW);
}

// Сброс навигации на конкретный корневой экран (вместо ручного stack_depth=0 + присваивания).
static void resetToScreen(ScreenType scr) {
  stack_depth = 0;
  screen_stack[0] = scr;
  menu_selected = 0;
  syncEnWakeup();
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

// --- Калибровки (TODO #1) ---
static void loadCalibration() {
  g_cal_voffset_l   = g_pref.getFloat("cal_voff_l", DEF_ADC_OFFSET_L_V);
  g_cal_voffset_r   = g_pref.getFloat("cal_voff_r", DEF_ADC_OFFSET_R_V);
  g_cal_v_to_ma_l   = g_pref.getFloat("cal_vma_l",  DEF_ADC_V_TO_MA);
  g_cal_v_to_ma_r   = g_pref.getFloat("cal_vma_r",  DEF_ADC_V_TO_MA);
  g_cal_dac_code_l  = g_pref.getFloat("cal_dac_l",  DEF_DAC_CODE_TO_MA_L);
  g_cal_dac_code_r  = g_pref.getFloat("cal_dac_r",  DEF_DAC_CODE_TO_MA_R);
  DacControl::setCodeToMa(g_cal_dac_code_l, g_cal_dac_code_r);
}

static void saveCalibration() {
  g_pref.putFloat("cal_voff_l", g_cal_voffset_l);
  g_pref.putFloat("cal_voff_r", g_cal_voffset_r);
  g_pref.putFloat("cal_vma_l",  g_cal_v_to_ma_l);
  g_pref.putFloat("cal_vma_r",  g_cal_v_to_ma_r);
  g_pref.putFloat("cal_dac_l",  g_cal_dac_code_l);
  g_pref.putFloat("cal_dac_r",  g_cal_dac_code_r);
  DacControl::setCodeToMa(g_cal_dac_code_l, g_cal_dac_code_r);
}

// Сброс NVS: пресеты, калибровка или всё (отдельные пункты в настройках).
static void clearPresetNvs() {
  static const char* tags[] = { "amp", "amp_l", "amp_r", "dur", "fin", "fout", "frq", "frq_l", "frq_r" };
  for (const PresetDefinition& p : g_presets) {
    for (const char* tag : tags) {
      g_pref.remove(keyForParam(p.id, tag).c_str());
    }
  }
  g_runtime.clear();
}

static void clearCalibrationNvs() {
  static const char* keys[] = {
    "cal_voff_l", "cal_voff_r", "cal_vma_l", "cal_vma_r", "cal_dac_l", "cal_dac_r"
  };
  for (const char* k : keys) g_pref.remove(k);
  loadCalibration();
}

static void clearAllNvs() {
  g_pref.clear();
  g_runtime.clear();
  loadCalibration();
}

static void openNvsResetConfirm(ConfirmKind kind) {
  confirm_kind = kind;
  pushScreen(SCR_CONFIRM);
  menu_selected = 0;
}

// --- Напряжение батареи: делитель R_top=100k / R_bot=360k ---
// V_bat = V_adc * (R_top + R_bot) / R_bot = V_adc * 460/360
// V_adc = raw * 3.3 / 4095
static float read_bat_v() {
  const int N = 16;                 // усреднение против шума одиночного отсчёта (TODO #4)
  uint32_t acc = 0;
  for (int i = 0; i < N; i++) acc += analogRead(PLUS_BAT_ADC);
  return ((float)acc / N) * (3.3f / 4095.0f) * (460.0f / 360.0f);
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
// TODO #4: обновляем не чаще раза в секунду + EMA — иначе значение шумит и скачет при
// перерисовке меню (вращение энкодера).
static float    g_bat_pct_ema = -1.0f;
static uint32_t g_bat_last_ms = 0;
static void updateBatteryCacheIfSafe() {
  if (AdcControl::isRunning()) return;
  uint32_t now = millis();
  if (g_bat_last_ms != 0 && (now - g_bat_last_ms) < 1000) return;
  g_bat_last_ms = now;
  int pct = read_bat_pct();
  if (g_bat_pct_ema < 0.0f) g_bat_pct_ema = (float)pct;      // первичная инициализация
  else g_bat_pct_ema += 0.2f * ((float)pct - g_bat_pct_ema); // сглаживание
  g_bat_pct_cached = (int)lroundf(g_bat_pct_ema);
}
static int batteryPct() { return g_bat_pct_cached; }

static PresetRuntime& runtimeForPreset(int idx) {
  if ((int)g_runtime.size() <= idx) g_runtime.resize(idx + 1);
  PresetRuntime& rt = g_runtime[idx];
  if (!rt.initialized && idx >= 0 && idx < (int)g_presets.size()) {
    const PresetDefinition& p = g_presets[idx];
    const ParamRange& ar = p.amplitude_mA;
    const float amp_def = ar.default_v;

    // Амплитуда/частота: всегда amp_l (+ amp_r при both). Без legacy-ключей amp/frq.
    rt.amp_l_mA = loadParamNvs(p.id, "amp_l", amp_def, ar.min_v, ar.max_v);
    rt.amp_r_mA = p.channels_both
        ? loadParamNvs(p.id, "amp_r", amp_def, ar.min_v, ar.max_v) : amp_def;

    rt.duration_min = loadParamNvs(p.id, "dur", p.duration_min.default_v, p.duration_min.min_v, p.duration_min.max_v);
    rt.fade_in_sec = loadParamNvs(p.id, "fin", p.fade_in_sec.default_v, p.fade_in_sec.min_v, p.fade_in_sec.max_v);
    rt.fade_out_sec = loadParamNvs(p.id, "fout", p.fade_out_sec.default_v, p.fade_out_sec.min_v, p.fade_out_sec.max_v);

    if (p.type == PresetType::SIN && p.frequency_hz.valid) {
      const ParamRange& fr = p.frequency_hz;
      const float fq_def = fr.default_v;
      rt.freq_l_hz = loadParamNvs(p.id, "frq_l", fq_def, fr.min_v, fr.max_v);
      rt.freq_r_hz = p.channels_both
          ? loadParamNvs(p.id, "frq_r", fq_def, fr.min_v, fr.max_v) : fq_def;
    }
    rt.initialized = true;
  }
  return rt;
}

static void saveRuntimeForPreset(int idx) {
  if (idx < 0 || idx >= (int)g_presets.size() || idx >= (int)g_runtime.size()) return;
  const PresetDefinition& p = g_presets[idx];
  const PresetRuntime& rt = g_runtime[idx];
  saveParamNvs(p.id, "amp_l", rt.amp_l_mA);
  if (p.channels_both) saveParamNvs(p.id, "amp_r", rt.amp_r_mA);
  saveParamNvs(p.id, "dur", rt.duration_min);
  saveParamNvs(p.id, "fin", rt.fade_in_sec);
  saveParamNvs(p.id, "fout", rt.fade_out_sec);
  if (p.type == PresetType::SIN && p.frequency_hz.valid) {
    saveParamNvs(p.id, "frq_l", rt.freq_l_hz);
    if (p.channels_both) saveParamNvs(p.id, "frq_r", rt.freq_r_hz);
  }
}

static void pushScreen(ScreenType scr) {
  if (stack_depth < SCREEN_STACK_MAX - 1) {
    stack_depth++;
    screen_stack[stack_depth] = scr;
    menu_selected = 0;
    syncEnWakeup();
  }
}

static void popScreen() {
  if (stack_depth > 0) {
    stack_depth--;
    menu_selected = 0;
    syncEnWakeup();
  }
}

static void openEditor(const char* title, float* ptr, float min_v, float max_v, float step,
                       bool as_int, bool is_cal = false) {
  editor.title = title;
  editor.value = ptr;
  editor.min_v = min_v;
  editor.max_v = max_v;
  editor.step = step;
  editor.as_int = as_int;
  editor.is_cal = is_cal;
  editor_temp = *ptr;
  pushScreen(SCR_EDITOR);
}

static void showError(const char* msg) {
  snprintf(g_error_msg, sizeof(g_error_msg), "%s", msg);
  pushScreen(SCR_ERROR);
}

// Проверки перед стартом сеанса (TODO #2/#3). false + экран ошибки, если старт запрещён.
static bool guardSessionStart() {
  if (digitalRead(USB_DET) == HIGH) {
    showError("Отключите USB");
    return false;
  }
  if (batteryPct() < MIN_BATTERY_START_PCT) {
    showError("Низкий заряд");
    return false;
  }
  return true;
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
  dashboard_view = DASH_LEFT;
  session_name = preset.name;
  session_type = preset.type;
  session_amp_l_mA = rt.amp_l_mA;
  session_amp_r_mA = preset.channels_both ? rt.amp_r_mA : 0.0f;
  session_duration_min = rt.duration_min;
  session_fade_in_sec = rt.fade_in_sec;
  session_fade_out_sec = rt.fade_out_sec;
  session_freq_l_hz = rt.freq_l_hz;
  session_freq_r_hz = preset.channels_both ? rt.freq_r_hz : rt.freq_l_hz;
  session_channels_both = preset.channels_both;
  session_feedback_base = preset.feedback_base;
  session_feedback_coeff = preset.feedback_coeff;

  DacProgram p{};
  p.amp_l_ma = session_amp_l_mA;
  p.amp_r_ma = session_amp_r_mA;
  p.sample_rate_hz = preset.sample_rate_hz;
  if (preset.type == PresetType::CONST_DC) {
    p.waveform = DacWaveform::CONST_DC;
    DacControl::setProgram(p);
  } else if (preset.type == PresetType::SIN) {
    p.waveform = DacWaveform::SIN;
    p.target_freq_hz = session_freq_l_hz;
    p.target_freq_r_hz = session_freq_r_hz;
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
  const DacProgram& prog = DacControl::program();
  session_actual_freq_l_hz = prog.actual_freq_hz;
  session_actual_freq_r_hz = prog.actual_freq_r_hz;

  if (preset.type == PresetType::SIN && prog.period_samples_l >= 2) {
    // Период в ADC-семплах = DAC-период при Fs=8000 (TODO #8, целое число).
    session_scope_period   = (uint32_t)prog.period_samples_l;
    session_scope_period_r = (prog.period_samples_r >= 2)
        ? (uint32_t)prog.period_samples_r : session_scope_period;
    session_scope_nper = 2;
  } else if (preset.type == PresetType::WAV && preset.sample_rate_hz > 0 && !preset.wav_left_samples.empty()) {
    session_scope_period = (uint32_t)(
        ((uint64_t)preset.wav_left_samples.size() * ADC_OUT_RATE_HZ) / (uint32_t)preset.sample_rate_hz);
    session_scope_period_r = session_scope_period;
    session_scope_nper   = 1;
  } else {
    session_scope_period = 0;
    session_scope_period_r = 0;
    session_scope_nper   = 0;
  }

  DacControl::setGain(0.0f);
  DacControl::start();
  AdcControl::start(g_cal_voffset_l, g_cal_voffset_r);

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

// Число заряда в правом верхнем углу (TODO #4): читаемее, чем полоса в 1px.
static void drawBatteryPct() {
  char batt[8];
  snprintf(batt, sizeof(batt), "%d%%", batteryPct());
  int bw = oled.getUTF8Width(batt);
  oled.drawUTF8(128 - bw, 2, batt);
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
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  drawBatteryPct();
  oled.drawUTF8(0, 2, title);

  uint8_t vis = (count < max_vis) ? count : max_vis;
  for (uint8_t i = 0; i < vis; i++) {
    uint8_t idx = scroll + i;
    uint8_t y = y_start + i * item_h;
    if (idx == menu_selected) oled.drawStr(0, y, ">");
    oled.drawUTF8(10, y, choices[idx]);
  }

  if (count > max_vis) {
    if (scroll > 0) oled.drawTriangle(124, 14, 120, 18, 128, 18);
    if (scroll < count - max_vis) oled.drawTriangle(124, 62, 120, 58, 128, 58);
  }
  oledSendBuffer();
}

// --- Осциллограф дашборда (R.6/R.8) ---
#define DASH_SCOPE_X   16
#define DASH_SCOPE_W   112
#define DASH_SCOPE_Y   12
#define DASH_SCOPE_H   36

static float feedbackForChannel(const AdcChannelStats& st, bool is_left) {
  if (!st.valid) return 0.0f;
  const float offset_v = is_left ? g_cal_voffset_l : g_cal_voffset_r;
  const float v_to_ma  = is_left ? g_cal_v_to_ma_l : g_cal_v_to_ma_r;
  float base;
  if (session_feedback_base == FeedbackBase::STD) base = st.std_v * v_to_ma;
  else base = fabsf(st.mean_v - offset_v) * v_to_ma;
  return base * session_feedback_coeff;
}

static void drawDottedHLine(int x, int y, int w) {
  for (int i = 0; i < w; i += 4) oled.drawPixel(x + i, y);
}

// Рисует осциллограмму одного канала: линейная V->mA, тики по амплитуде.
// CONST -> униполярная развёртка, SIN/WAV -> биполярная.
static void drawScopeChannel(bool left) {
  const float amp = left ? session_amp_l_mA : session_amp_r_mA;
  const uint32_t scope_period = left ? session_scope_period : session_scope_period_r;
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
  if (!AdcControl::scopeTrace(left, trace, DASH_SCOPE_W, scope_period, session_scope_nper)) return;
  const float offset  = left ? g_cal_voffset_l : g_cal_voffset_r;
  const float v_to_ma = left ? g_cal_v_to_ma_l : g_cal_v_to_ma_r;
  int prev_py = -1;
  for (int x = 0; x < DASH_SCOPE_W; x++) {
    float ma = (trace[x] - offset) * v_to_ma;
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
  oled.setFont(u8g2_font_6x12_t_cyrillic);
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

static void drawBothChannelBlock(bool left, int y, const AdcChannelStats& st) {
  const float off = left ? g_cal_voffset_l : g_cal_voffset_r;
  const float k   = left ? g_cal_v_to_ma_l : g_cal_v_to_ma_r;
  char l1[24], l2[32];
  if (st.valid) {
    const float peak_v  = fmaxf(fabsf(st.max_v - off), fabsf(st.min_v - off));
    const float max_ma  = peak_v * k;
    const float mean_ma = fabsf(st.mean_v - off) * k;
    snprintf(l1, sizeof(l1), "%c: макс. %.2fмА", left ? 'L' : 'R', max_ma);
    snprintf(l2, sizeof(l2), "   ср. %.2fмА (%.2fВ)", mean_ma, st.mean_v);
  } else {
    snprintf(l1, sizeof(l1), "%c: макс. --", left ? 'L' : 'R');
    snprintf(l2, sizeof(l2), "   ср. -- (--)");
  }
  oled.drawUTF8(0, y, l1);
  oled.drawUTF8(0, y + 9, l2);
}

static void drawDashboard() {
  oled.clearBuffer();

  AdcChannelStats l = AdcControl::statsLeft();
  AdcChannelStats r = AdcControl::statsRight();
  const float fb_l = feedbackForChannel(l, true);
  const float fb_r = feedbackForChannel(r, false);

  char header[48], metric[20];
  if (dashboard_view == DASH_BOTH) {
    // L+R (TODO #14): <имя> [Hz] mA ; поканально макс./ср. мА + среднее ADC (В).
    oled.setFont(u8g2_font_5x8_t_cyrillic);
    char head[48];
    if (session_channels_both) {
      if (session_type == PresetType::SIN) {
        snprintf(head, sizeof(head), "%s %.0f/%.0fHz %.1f/%.1fmA",
                 session_name.c_str(), session_freq_l_hz, session_freq_r_hz,
                 session_amp_l_mA, session_amp_r_mA);
      } else {
        snprintf(head, sizeof(head), "%s L%.1f/R%.1fmA",
                 session_name.c_str(), session_amp_l_mA, session_amp_r_mA);
      }
    } else if (session_type == PresetType::SIN) {
      snprintf(head, sizeof(head), "%s %.0fHz %.1fmA",
               session_name.c_str(), session_freq_l_hz, session_amp_l_mA);
    } else {
      snprintf(head, sizeof(head), "%s %.1fmA", session_name.c_str(), session_amp_l_mA);
    }
    oled.drawUTF8(0, 0, head);
    char batt[8];
    snprintf(batt, sizeof(batt), "%d%%", batteryPct());
    oled.drawUTF8(128 - oled.getUTF8Width(batt), 0, batt);

    drawBothChannelBlock(true,  11, l);
    drawBothChannelBlock(false, 29, r);

    uint32_t elapsed = (millis() - session_started_ms) / 1000;
    char tb[24];
    snprintf(tb, sizeof(tb), "t %u:%02u / %.0fм",
             (unsigned)(elapsed / 60), (unsigned)(elapsed % 60), session_duration_min);
    oled.drawUTF8(0, 48, tb);

    float total = session_duration_min * 60.0f;
    float progress = (total > 0.0f) ? (float)elapsed / total : 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    oled.drawHLine(0, 63, (int)(progress * 128));
    oledSendBuffer();
    return;
  }

  oled.setFont(u8g2_font_6x12_t_cyrillic);
  // L/R: осциллограмма + выставленные параметры (TODO #13).
  const bool left = (dashboard_view == DASH_LEFT);
  const char ch = left ? 'L' : 'R';
  const float ch_amp  = left ? session_amp_l_mA : session_amp_r_mA;
  const float ch_freq = left ? session_freq_l_hz : session_freq_r_hz;
  if (session_type == PresetType::SIN) {
    snprintf(header, sizeof(header), "%c %s %.0fHz %.1fmA",
             ch, session_name.c_str(), ch_freq, ch_amp);
  } else {
    snprintf(header, sizeof(header), "%c %s %.1fmA",
             ch, session_name.c_str(), ch_amp);
  }
  oled.drawUTF8(0, 0, header);

  drawScopeChannel(left);
  snprintf(metric, sizeof(metric), "%.1fmA", left ? fb_l : fb_r);
  drawMeterAndProgress(metric);
  oledSendBuffer();
}

static void drawConfirm() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  const char* title = "Остановить сеанс?";
  const char* yes_lbl = "Да, плавный стоп";
  if (confirm_kind == CONFIRM_RESET_PRESETS) {
    title = "Сброс пресетов?";
    yes_lbl = "Да, сбросить";
  } else if (confirm_kind == CONFIRM_RESET_CALIB) {
    title = "Сброс калибровки?";
    yes_lbl = "Да, сбросить";
  } else if (confirm_kind == CONFIRM_RESET_ALL_NVS) {
    title = "Сброс всего NVS?";
    yes_lbl = "Да, сбросить";
  }
  oled.drawUTF8(0, 0, title);
  if (menu_selected == 0) {
    oled.drawUTF8(0, 24, "> Нет");
    char no2[32];
    snprintf(no2, sizeof(no2), "  %s", yes_lbl);
    oled.drawUTF8(0, 38, no2);
  } else {
    oled.drawUTF8(0, 24, "  Нет");
    char yes2[32];
    snprintf(yes2, sizeof(yes2), "> %s", yes_lbl);
    oled.drawUTF8(0, 38, yes2);
  }
  oledSendBuffer();
}

static void drawEditor() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  oled.drawUTF8(0, 0, editor.title);
  oled.setFont(u8g2_font_9x15_tf);
  char v[20];
  if (editor.as_int) snprintf(v, sizeof(v), "%d", (int)lroundf(editor_temp));
  else {
    const char* fmt = (editor.step < 0.05f) ? "%.3f" : (editor.step < 0.5f ? "%.2f" : "%.1f");
    snprintf(v, sizeof(v), fmt, editor_temp);
  }
  oled.drawStr(30, 24, v);
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  oled.drawUTF8(0, 52, "> сохранить");
  oledSendBuffer();
}

static void formatSessionParamsLine(char* buf, size_t len,
                                    const PresetDefinition& p, const PresetRuntime& rt) {
  if (p.type == PresetType::SIN) {
    if (p.channels_both) {
      snprintf(buf, len, "%.0f/%.0fHz %.1f/%.1fmA",
               rt.freq_l_hz, rt.freq_r_hz, rt.amp_l_mA, rt.amp_r_mA);
    } else {
      snprintf(buf, len, "%.0fHz %.1fmA", rt.freq_l_hz, rt.amp_l_mA);
    }
  } else if (p.channels_both) {
    snprintf(buf, len, "L%.1f/R%.1fmA", rt.amp_l_mA, rt.amp_r_mA);
  } else {
    snprintf(buf, len, "%.1fmA", rt.amp_l_mA);
  }
}

static void drawPreStart() {
  if (active_preset_idx < 0 || active_preset_idx >= (int)g_presets.size()) return;
  const PresetDefinition& p = g_presets[active_preset_idx];
  const PresetRuntime& rt = runtimeForPreset(active_preset_idx);

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  oled.drawUTF8(0, 0, "НАЧАТЬ СЕАНС");
  char line[40];
  formatSessionParamsLine(line, sizeof(line), p, rt);
  oled.drawUTF8(0, 13, line);
  snprintf(line, sizeof(line), "%.0f мин", rt.duration_min);
  oled.drawUTF8(0, 25, line);
  oled.drawUTF8(0, 37, "*подключите электроды*");
  oled.drawUTF8(0, 52, "> старт");
  oledSendBuffer();
}

static void drawFinish() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  oled.drawUTF8(0, 0, "СЕАНС ЗАВЕРШЕН");
  char line[40];
  if (session_type == PresetType::SIN) {
    if (session_channels_both) {
      snprintf(line, sizeof(line), "%.0f/%.0fHz %.1f/%.1fmA",
               session_freq_l_hz, session_freq_r_hz, session_amp_l_mA, session_amp_r_mA);
    } else {
      snprintf(line, sizeof(line), "%.0fHz %.1fmA", session_freq_l_hz, session_amp_l_mA);
    }
  } else if (session_channels_both) {
    snprintf(line, sizeof(line), "L%.1f/R%.1fmA", session_amp_l_mA, session_amp_r_mA);
  } else {
    snprintf(line, sizeof(line), "%.1fmA", session_amp_l_mA);
  }
  oled.drawUTF8(0, 13, line);
  uint32_t mins = session_elapsed_sec / 60;
  uint32_t secs = session_elapsed_sec % 60;
  snprintf(line, sizeof(line), "%u:%02u", (unsigned)mins, (unsigned)secs);
  oled.drawUTF8(0, 25, line);
  oled.drawUTF8(0, 37, "*отсоедините электроды*");
  oled.drawUTF8(0, 52, "> меню");
  oledSendBuffer();
}

static void drawError() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13_t_cyrillic);
  oled.drawUTF8(0, 4, "ОШИБКА");
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  oled.drawUTF8(0, 26, g_error_msg);
  oled.drawUTF8(0, 50, "> назад");
  oledSendBuffer();
}

// Заполняет декларативный список пунктов пресет-меню (R.10).
// items/labels — буферы длиной PRESET_MENU_MAX_ITEMS; возвращает число пунктов.
static int buildPresetMenu(const PresetDefinition& p, PresetRuntime& rt,
                           PresetMenuEntry* items, char labels[][36]) {
  int n = 0;
  items[n] = { PMI_START, nullptr, nullptr, 0, 0, 0, false };
  snprintf(labels[n], 36, "СТАРТ"); n++;

  const ParamRange& ar = p.amplitude_mA;
  if (p.channels_both) {
    items[n] = { PMI_PARAM, "Ампл. L, мА", &rt.amp_l_mA, ar.min_v, ar.max_v, ar.step, false };
    snprintf(labels[n], 36, "Ампл. L: %.1f мА", rt.amp_l_mA); n++;
    items[n] = { PMI_PARAM, "Ампл. R, мА", &rt.amp_r_mA, ar.min_v, ar.max_v, ar.step, false };
    snprintf(labels[n], 36, "Ампл. R: %.1f мА", rt.amp_r_mA); n++;
  } else {
    items[n] = { PMI_PARAM, "Амплитуда, мА", &rt.amp_l_mA, ar.min_v, ar.max_v, ar.step, false };
    snprintf(labels[n], 36, "Амплитуда: %.1f мА", rt.amp_l_mA); n++;
  }

  if (p.type == PresetType::SIN && p.frequency_hz.valid) {
    const ParamRange& fr = p.frequency_hz;
    if (p.channels_both) {
      items[n] = { PMI_PARAM, "Част. L, Гц", &rt.freq_l_hz, fr.min_v, fr.max_v, fr.step, false };
      snprintf(labels[n], 36, "Част. L: %.1f Гц", rt.freq_l_hz); n++;
      items[n] = { PMI_PARAM, "Част. R, Гц", &rt.freq_r_hz, fr.min_v, fr.max_v, fr.step, false };
      snprintf(labels[n], 36, "Част. R: %.1f Гц", rt.freq_r_hz); n++;
    } else {
      items[n] = { PMI_PARAM, "Частота, Гц", &rt.freq_l_hz, fr.min_v, fr.max_v, fr.step, false };
      snprintf(labels[n], 36, "Частота: %.1f Гц", rt.freq_l_hz); n++;
    }
  }

  items[n] = { PMI_PARAM, "Длительность, мин", &rt.duration_min,
               p.duration_min.min_v, p.duration_min.max_v, p.duration_min.step, true };
  snprintf(labels[n], 36, "Длит.: %.0f мин", rt.duration_min); n++;

  items[n] = { PMI_PARAM, "Плавный старт, с", &rt.fade_in_sec,
               p.fade_in_sec.min_v, p.fade_in_sec.max_v, p.fade_in_sec.step, true };
  snprintf(labels[n], 36, "Плавн.старт: %.0fс", rt.fade_in_sec); n++;

  items[n] = { PMI_PARAM, "Плавный стоп, с", &rt.fade_out_sec,
               p.fade_out_sec.min_v, p.fade_out_sec.max_v, p.fade_out_sec.step, true };
  snprintf(labels[n], 36, "Плавн.стоп: %.0fс", rt.fade_out_sec); n++;

  items[n] = { PMI_BACK, nullptr, nullptr, 0, 0, 0, false };
  snprintf(labels[n], 36, "<-Назад"); n++;
  return n;
}

static void drawCurrentScreen() {
  char line0[32], line1[32], line2[32], line3[32];
  const char* choices[4] = { line0, line1, line2, line3 };
  switch (currentScreen()) {
    case SCR_MAIN_MENU: {
      int n = (int)g_presets.size();
      if (n > MENU_MAX_PRESETS) n = MENU_MAX_PRESETS;
      static char labels[MENU_MAX_PRESETS + 1][40];
      const char* items[MENU_MAX_PRESETS + 2];
      int cnt = 0;
      for (int i = 0; i < n; i++) {
        snprintf(labels[cnt], sizeof(labels[cnt]), "%s", g_presets[i].name.c_str());
        items[cnt] = labels[cnt];
        cnt++;
      }
      items[cnt++] = "Настройки";   // предпоследний (TODO #11)
      items[cnt++] = "Выключить";   // последний
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
      char labels[PRESET_MENU_MAX_ITEMS][36];
      const char* choices2[PRESET_MENU_MAX_ITEMS];
      int cnt = buildPresetMenu(p, rt, entries, labels);
      for (int i = 0; i < cnt; i++) choices2[i] = labels[i];
      char title[40];
      snprintf(title, sizeof(title), "== %s ==", p.name.c_str());
      renderMenu(title, choices2, cnt);
      break;
    }
    case SCR_SETTINGS_MENU: {
      static char slabels[8][36];
      const char* sitems[8];
      int scnt = 0;
      snprintf(slabels[scnt], 36, "<-Назад");
      sitems[scnt++] = slabels[0];
      snprintf(slabels[scnt], 36, "Энкодер: %s", ui.enc_reverse ? "Инв." : "Норм.");
      sitems[scnt++] = slabels[1];
      snprintf(slabels[scnt], 36, "Калибровка");
      sitems[scnt++] = slabels[2];
      snprintf(slabels[scnt], 36, "UF2");
      sitems[scnt++] = slabels[3];
      snprintf(slabels[scnt], 36, "%s", FIRMWARE_VERSION);
      sitems[scnt++] = slabels[4];
      snprintf(slabels[scnt], 36, "Сброс пресетов");
      sitems[scnt++] = slabels[5];
      snprintf(slabels[scnt], 36, "Сброс калибровки");
      sitems[scnt++] = slabels[6];
      snprintf(slabels[scnt], 36, "Сброс всего NVS");
      sitems[scnt++] = slabels[7];
      renderMenu("== Настройки ==", sitems, scnt);
      break;
    }
    case SCR_CALIB_MENU: {
      char line4[32], line5[32], line6[32];
      const char* citems[7] = { line0, line1, line2, line3, line4, line5, line6 };
      snprintf(line0, sizeof(line0), "<-Назад");
      snprintf(line1, sizeof(line1), "Voffset L: %.2f", g_cal_voffset_l);
      snprintf(line2, sizeof(line2), "Voffset R: %.2f", g_cal_voffset_r);
      snprintf(line3, sizeof(line3), "V→mA L: %.2f", g_cal_v_to_ma_l);
      snprintf(line4, sizeof(line4), "V→mA R: %.2f", g_cal_v_to_ma_r);
      snprintf(line5, sizeof(line5), "DAC к/мА L: %d", (int)lroundf(g_cal_dac_code_l));
      snprintf(line6, sizeof(line6), "DAC к/мА R: %d", (int)lroundf(g_cal_dac_code_r));
      renderMenu("== Калибровка ==", citems, 7);
      break;
    }
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
    case SCR_PRE_START:
      drawPreStart();
      break;
    case SCR_ERROR:
      drawError();
      break;
  }
}

static void executeMainMenu() {
  int n = (int)g_presets.size();
  if (n > MENU_MAX_PRESETS) n = MENU_MAX_PRESETS;
  if (menu_selected == (uint8_t)n) {         // предпоследний — "Настройки"
    pushScreen(SCR_SETTINGS_MENU);
    return;
  }
  if (menu_selected == (uint8_t)(n + 1)) {   // последний — "Выключить" (TODO #11)
    go_sleep();
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
  char labels[PRESET_MENU_MAX_ITEMS][36];
  int cnt = buildPresetMenu(p, rt, entries, labels);
  if (menu_selected >= (uint8_t)cnt) return;
  const PresetMenuEntry& e = entries[menu_selected];
  switch (e.kind) {
    case PMI_START:
      if (guardSessionStart()) pushScreen(SCR_PRE_START);
      break;
    case PMI_BACK:  popScreen(); break;
    case PMI_PARAM: openEditor(e.editor_title, e.value, e.min_v, e.max_v, e.step, e.as_int); break;
  }
}

static int maxMenuIndexForScreen(ScreenType scr) {
  if (scr == SCR_MAIN_MENU) {
    int n = (int)g_presets.size();
    if (n > MENU_MAX_PRESETS) n = MENU_MAX_PRESETS;
    return n + 1; // ..., "Настройки" (n), "Выключить" (n+1)
  }
  if (scr == SCR_SETTINGS_MENU) return 7;
  if (scr == SCR_CALIB_MENU) return 6;
  if (scr == SCR_CONFIRM) return 1;
  if (scr == SCR_FINISH) return 0;
  if (scr == SCR_PRE_START) return 0;
  if (scr == SCR_ERROR) return 0;
  if (scr == SCR_EDITOR || scr == SCR_DASHBOARD) return 0;

  if (scr == SCR_PRESET_MENU) {
    if (active_preset_idx < 0 || active_preset_idx >= (int)g_presets.size()) return 0;
    const PresetDefinition& p = g_presets[active_preset_idx];
    PresetRuntime& rt = runtimeForPreset(active_preset_idx);
    PresetMenuEntry entries[PRESET_MENU_MAX_ITEMS];
    char labels[PRESET_MENU_MAX_ITEMS][36];
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
    // Инверсия относительно меню: при задании чисел вращение по часовой должно
    // увеличивать значение (TODO #9).
    editor_temp -= delta * editor.step;
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
        pushScreen(SCR_CALIB_MENU);
      } else if (menu_selected == 3) {
        BootControl::rebootToUF2();
      } else if (menu_selected == 5) {
        openNvsResetConfirm(CONFIRM_RESET_PRESETS);
      } else if (menu_selected == 6) {
        openNvsResetConfirm(CONFIRM_RESET_CALIB);
      } else if (menu_selected == 7) {
        openNvsResetConfirm(CONFIRM_RESET_ALL_NVS);
      }
      // 4 = версия прошивки (информация)
      break;
    case SCR_CALIB_MENU:
      switch (menu_selected) {
        case 0: popScreen(); break;
        case 1: openEditor("Voffset L, V", &g_cal_voffset_l, CAL_VOFFSET_MIN, CAL_VOFFSET_MAX, CAL_VOFFSET_STEP, false, true); break;
        case 2: openEditor("Voffset R, V", &g_cal_voffset_r, CAL_VOFFSET_MIN, CAL_VOFFSET_MAX, CAL_VOFFSET_STEP, false, true); break;
        case 3: openEditor("V->mA L", &g_cal_v_to_ma_l, CAL_V_TO_MA_MIN, CAL_V_TO_MA_MAX, CAL_V_TO_MA_STEP, false, true); break;
        case 4: openEditor("V->mA R", &g_cal_v_to_ma_r, CAL_V_TO_MA_MIN, CAL_V_TO_MA_MAX, CAL_V_TO_MA_STEP, false, true); break;
        case 5: openEditor("DAC код/мА L", &g_cal_dac_code_l, CAL_DAC_CODE_MIN, CAL_DAC_CODE_MAX, CAL_DAC_CODE_STEP, true, true); break;
        case 6: openEditor("DAC код/мА R", &g_cal_dac_code_r, CAL_DAC_CODE_MIN, CAL_DAC_CODE_MAX, CAL_DAC_CODE_STEP, true, true); break;
      }
      break;
    case SCR_EDITOR:
      if (editor.value) {
        *editor.value = editor_temp;
        if (editor.is_cal) saveCalibration();
        else saveRuntimeForPreset(active_preset_idx);
      }
      popScreen();
      break;
    case SCR_DASHBOARD:
      if (session_state != STATE_IDLE) {
        confirm_kind = CONFIRM_STOP_SESSION;
        pushScreen(SCR_CONFIRM);
        menu_selected = 0;
      } else {
        resetToScreen(SCR_MAIN_MENU);
      }
      break;
    case SCR_CONFIRM:
      if (menu_selected == 1) {
        switch (confirm_kind) {
          case CONFIRM_STOP_SESSION: beginFadeOut(); break;
          case CONFIRM_RESET_PRESETS: clearPresetNvs(); break;
          case CONFIRM_RESET_CALIB: clearCalibrationNvs(); break;
          case CONFIRM_RESET_ALL_NVS: clearAllNvs(); break;
        }
      }
      confirm_kind = CONFIRM_STOP_SESSION;
      popScreen();
      break;
    case SCR_FINISH:
      resetToScreen(SCR_MAIN_MENU);
      break;
    case SCR_PRE_START:
      if (active_preset_idx >= 0 && active_preset_idx < (int)g_presets.size()) {
        startSession(g_presets[active_preset_idx], runtimeForPreset(active_preset_idx));
      }
      break;
    case SCR_ERROR:
      popScreen();
      break;
  }
}

// --- Глубокий сон, пробуждение по нажатию ENC_S ---
static void go_sleep() {
  digitalWrite(EN_WAKEUP, LOW);
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

static bool oledProbe() {
  Wire.beginTransmission(DISPLAY_ADDR);
  return Wire.endTransmission() == 0;
}

static void oledInit() {
  Wire.end();
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(OLED_I2C_HZ);
  Wire.setTimeOut(50);
  oled.setI2CAddress(DISPLAY_ADDR << 1);
  oled.begin();
  oled.enableUTF8Print();
  oled.setFontPosTop();
  oled.setContrast(255);
  oled.setPowerSave(0);
  g_oled_ok = oledProbe();
}

static void oledSendBuffer() {
  if (g_oled_ok) oled.sendBuffer();
}

// Периодическая проверка I2C; при восстановлении связи — повторный oledInit().
// Возвращает true, если экран доступен; *reconnected=1 после успешного reinit.
static bool oledPollConnection(uint8_t* reconnected) {
  if (reconnected) *reconnected = 0;
  if (oledProbe()) {
    if (!g_oled_ok) {
      oledInit();
      if (g_oled_ok && reconnected) *reconnected = 1;
    }
    return g_oled_ok;
  }
  g_oled_ok = false;
  return false;
}

static void drawBootSplash() {
  static const char msg[] = "== ГРУЗИМСЯ ==";
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_t_cyrillic);
  const int w = oled.getUTF8Width(msg);
  const int w_ver = oled.getUTF8Width(FIRMWARE_VERSION);
  oled.drawUTF8((128 - w) / 2, (64 - 12) / 2, msg);
  oled.drawUTF8((128 - w_ver) / 2, 52, FIRMWARE_VERSION);
  oledSendBuffer();
}

static void init_enc() {
  AdcControl::init();
  enc.setEncType(EB_STEP4_LOW);
  enc.setDebTimeout(120); // 120ms debounce — защита от дребезга механического энкодера
  enc.setEncReverse(true);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_S, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), enc_isr, CHANGE);
  enc.setEncReverse(ui.enc_reverse);
}

// ============================================================
void setup() {
  BootControl::init();   // снять GPIO hold если остался с прошлой сессии UF2

  // EN_WAKEUP: управляется syncEnWakeup() — HIGH на PRE_START / сеанс / FINISH.
  pinMode(EN_WAKEUP, OUTPUT);
  digitalWrite(EN_WAKEUP, LOW);

  pinMode(USB_DET, INPUT);
  pinMode(CHRG_PIN, INPUT_PULLUP);

  oledInit();
  drawBootSplash();

  analogSetAttenuation(ADC_11db);
  g_pref.begin("preset_rt", false);
  loadCalibration();

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

  init_enc();
  drawCurrentScreen();
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

  static uint32_t t_oled_probe = 0;
  const uint32_t now = millis();
  if (t_oled_probe == 0 || now - t_oled_probe >= OLED_PROBE_MS) {
    t_oled_probe = now;
    uint8_t reconnected = 0;
    if (oledPollConnection(&reconnected) && reconnected) ch = true;
  }

  static uint32_t t_draw;
  if ((ch || now - t_draw > 200) && g_oled_ok) {
    t_draw = now;
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
