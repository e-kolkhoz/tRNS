#include "session_control.h"
#include "dac_control.h"
#include "preset_storage.h"
#include <EEPROM.h>
#include <math.h>

// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ===
SessionSettings current_settings;
SessionState current_state = STATE_IDLE;
uint32_t session_elapsed_sec = 0;  // Фактическое время последнего сеанса (секунды)
uint32_t session_timer_start_ms = 0;  // Время старта сеанса для таймера на дисплее

// === ВНУТРЕННИЕ ПЕРЕМЕННЫЕ ===
static uint32_t session_start_time = 0;     // Время старта текущего состояния сеанса

// EEPROM адреса
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xA5C3  // Магическое число для проверки валидности
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_SETTINGS 2

// Дефолтные настройки для каждого режима (из config.h)
static const SessionSettings default_settings = {
  .mode = MODE_TRNS,
  
  .amplitude_tDCS_mA = DEF_AMPLITUDE_MA,
  .duration_tDCS_min = DEF_DURATION_MIN,
  .amplitude_tRNS_mA = DEF_AMPLITUDE_MA,
  .duration_tRNS_min = DEF_DURATION_MIN,
  .amplitude_tACS_mA = DEF_AMPLITUDE_MA,
  .duration_tACS_min = DEF_DURATION_MIN,
  .frequency_tACS_Hz = DEF_TACS_FREQUENCY_HZ,
  
  // Общие настройки (заводские из config.h)
  .adc_v_to_mA = DEF_ADC_V_TO_MA,
  .dac_code_to_mA = DEF_DAC_CODE_TO_MA,
  .fade_duration_sec = DEF_FADE_DURATION_SEC
};

// === ИНИЦИАЛИЗАЦИЯ ===
void initSession() {
  Serial.println("[SESSION] initSession() begin");
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("[SESSION] EEPROM.begin FAILED");
  } else {
    Serial.println("[SESSION] EEPROM.begin OK");
  }
  loadSettings();
  current_state = STATE_IDLE;
  Serial.println("[SESSION] initSession() done");
}

// === EEPROM ===
void loadSettings() {
  Serial.println("[SESSION] loadSettings()");
  uint16_t magic = EEPROM.readUShort(EEPROM_ADDR_MAGIC);
  
  if (magic == EEPROM_MAGIC) {
    // EEPROM валиден - загружаем
    EEPROM.get(EEPROM_ADDR_SETTINGS, current_settings);
    Serial.println("[SESSION] EEPROM valid, settings loaded");
  } else {
    // EEPROM пуст - используем дефолт БЕЗ сохранения
    current_settings = default_settings;
    // saveSettings() вызовется при первом изменении настроек
    Serial.println("[SESSION] EEPROM empty, defaults applied");
  }

  // Валидация калибровки DAC (коды/мА)
  if (current_settings.dac_code_to_mA < MIN_DAC_CODE_TO_MA ||
      current_settings.dac_code_to_mA > MAX_DAC_CODE_TO_MA) {
    current_settings.dac_code_to_mA = DEF_DAC_CODE_TO_MA;
  }
}

void saveSettings() {
  // Проверяем, нужно ли сохранять (защита от лишних записей)
  SessionSettings stored_settings;
  EEPROM.get(EEPROM_ADDR_SETTINGS, stored_settings);
  
  // Сравниваем все поля (memcmp проще!)
  if (memcmp(&stored_settings, &current_settings, sizeof(SessionSettings)) == 0) {
    // Настройки не изменились, не пишем
    return;
  }
  
  // Настройки изменились, сохраняем
  EEPROM.writeUShort(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_SETTINGS, current_settings);
  EEPROM.commit();
}

void resetToDefaults() {
  current_settings = default_settings;
  saveSettings();
}

// === ГЕНЕРАЦИЯ СИГНАЛОВ ===

// Генератор tDCS - константа
static void generateTDCS() {
  // Постоянное значение = максимум (gain будет регулировать амплитуду)
  for (uint32_t i = 0; i < SIGNAL_SAMPLES; i++) {
    signal_buffer[i] = MAX_VAL;  // Максимальное положительное значение
  }
  snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tDCS %.1fmA %umin", 
           current_settings.amplitude_tDCS_mA, current_settings.duration_tDCS_min);
}

// Генератор tACS - синусоида
static void generateTACS() {
  float freq = getValidTACSFrequency(current_settings.frequency_tACS_Hz);
  float omega = 2.0f * PI * freq / SAMPLE_RATE;
  
  for (uint32_t i = 0; i < SIGNAL_SAMPLES; i++) {
    // Синусоида от -MAX_VAL до +MAX_VAL
    float sample = sinf(omega * i) * MAX_VAL;
    signal_buffer[i] = (int16_t)sample;
  }
  
  snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tACS %.0fГц %.1fmA", 
           freq, current_settings.amplitude_tACS_mA);
}

// Главная функция генерации
void generateSignal() {
  switch (current_settings.mode) {
    case MODE_TRNS:
      // Загружаем tRNS пресет из PROGMEM при каждом старте
      if (!loadPresetFromFlash(signal_buffer, current_preset_name, PRESET_NAME_MAX_LEN)) {
        // Если пресет не загрузился, хотя бы оставим имя по умолчанию
        snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tRNS 100-640Гц %.1fmA", 
                 current_settings.amplitude_tRNS_mA);
      }
      break;
      
    case MODE_TDCS:
      generateTDCS();
      break;
      
    case MODE_TACS:
      generateTACS();
      break;
  }
  
  // ВАЖНО: dynamic_dac_gain НЕ трогаем здесь!
  // Он управляется автоматически в updateSession() для fadein/fadeout
  // Амплитуда регулируется через scaling в signal_buffer (уже учтено в генераторах)
}

// === УПРАВЛЕНИЕ СЕАНСОМ ===
void startSession() {
  if (current_state == STATE_IDLE) {
    // Генерируем сигнал для текущего режима
    generateSignal();  // Всегда вызываем - и для tRNS обновит имя
    
    // Настраиваем масштаб амплитуды по мА → код DAC
    float amplitude_mA = current_settings.amplitude_tRNS_mA;
    switch (current_settings.mode) {
      case MODE_TDCS: amplitude_mA = current_settings.amplitude_tDCS_mA; break;
      case MODE_TACS: amplitude_mA = current_settings.amplitude_tACS_mA; break;
      case MODE_TRNS: default: break;
    }
    // dac_code_to_mA = сколько КОДОВ на 1 мА
    float target_code = (current_settings.dac_code_to_mA > 0.0f)
                          ? (amplitude_mA * current_settings.dac_code_to_mA)
                          : 0.0f;
    if (target_code < 0.0f) target_code = 0.0f;
    if (target_code > 32767.0f) target_code = 32767.0f;
    setAmplitudeScale(target_code / 32767.0f);
    
    // ВАЖНО: Обновляем стерео-буфер после генерации сигнала!
    updateStereoBuffer();

    // Сбрасываем DMA и заполняем заново, чтобы не играть старый мусор
    resetDacPlayback();

    // Включаем DAC только при старте сеанса
    startDacPlayback();
    
    // СБРАСЫВАЕМ ТАЙМЕР СЕАНСА!
    session_timer_start_ms = millis();
    
    // НАЧИНАЕМ FADEIN с нулевого gain!
    dynamic_dac_gain = 0.0f;
    session_start_time = millis();
    current_state = STATE_FADEIN;
  }
}

// Начальный gain при входе в FADEOUT (для плавного спуска с текущего значения)
static float fadeout_start_gain = 1.0f;

void stopSession() {
  // УНИВЕРСАЛЬНАЯ ОСТАНОВКА: просто переводим в FADEOUT
  // dynamic_dac_gain начинает декрементироваться С ТЕКУЩЕГО значения!
  if (current_state == STATE_FADEIN || current_state == STATE_STABLE) {
    // Сохраняем фактическое время сеанса ОТ НАЧАЛА (session_timer_start_ms)!
    session_elapsed_sec = (millis() - session_timer_start_ms) / 1000;
    
    // ЗАПОМИНАЕМ текущий gain для плавного спуска!
    fadeout_start_gain = dynamic_dac_gain;
    
    // Переходим в FADEOUT (gain начнёт падать с текущего значения)
    current_state = STATE_FADEOUT;
    session_start_time = millis();  // Для отсчёта времени fadeout
  }
  // Если уже в FADEOUT - ничего не делаем (непрерываемый fadeout!)
}

void updateSession() {
  uint32_t now = millis();
  float elapsed_sec = (now - session_start_time) / 1000.0f;
  
  // Используем fade_duration_sec из настроек!
  float fade_duration = current_settings.fade_duration_sec;
  
  switch (current_state) {
    case STATE_IDLE:
      // Ничего не делаем, gain = 0
      dynamic_dac_gain = 0.0f;
      break;
      
    case STATE_FADEIN:
      // Линейное нарастание 0.0 → 1.0 за fade_duration
      dynamic_dac_gain = elapsed_sec / fade_duration;
      
      // Проверка перехода в STABLE при достижении 1.0
      if (dynamic_dac_gain >= 1.0f) {
        dynamic_dac_gain = 1.0f;  // Насыщение
        current_state = STATE_STABLE;
        session_start_time = now;  // Сброс таймера для stable
      }
      break;
      
    case STATE_STABLE:
      // Gain постоянно = 1.0
      dynamic_dac_gain = 1.0f;
      
      // Проверка окончания сеанса по заданной длительности
      {
        // Выбираем длительность в зависимости от режима
        uint16_t duration_min = DEF_DURATION_MIN;  // дефолт из config.h
        switch (current_settings.mode) {
          case MODE_TRNS: duration_min = current_settings.duration_tRNS_min; break;
          case MODE_TDCS: duration_min = current_settings.duration_tDCS_min; break;
          case MODE_TACS: duration_min = current_settings.duration_tACS_min; break;
        }
        
        uint32_t total_duration_sec = duration_min * 60;
        // Общее время = fadein + stable + fadeout
        // Время в STABLE = total - fadein - fadeout = total - 2*fade
        float stable_time = total_duration_sec - 2.0f * fade_duration;
        if (stable_time < 0) stable_time = 0;  // Защита от отрицательного времени
        if (elapsed_sec >= stable_time) {
          // Автоматический переход в fadeout
          fadeout_start_gain = 1.0f;  // Из STABLE всегда начинаем с 1.0
          current_state = STATE_FADEOUT;
          session_start_time = now;  // Сброс таймера для fadeout
        }
      }
      break;
      
    case STATE_FADEOUT:
      {
        // Линейное убывание С ТЕКУЩЕГО gain до 0.0
        // Время fadeout пропорционально текущему gain: если gain=0.5, то fadeout=10сек (не 20)
        float fadeout_time = fadeout_start_gain * fade_duration;
        if (fadeout_time < 0.1f) fadeout_time = 0.1f;  // Минимум 0.1 сек
        
        dynamic_dac_gain = fadeout_start_gain * (1.0f - elapsed_sec / fadeout_time);
        
        // Проверка перехода в IDLE при достижении 0.0
        if (dynamic_dac_gain <= 0.0f) {
          dynamic_dac_gain = 0.0f;  // Насыщение
          current_state = STATE_IDLE;
          // Останавливаем DAC в idle, чтобы не было мусора
          stopDacPlayback();
          // Автоматически покажется SCR_FINISH через isSessionJustFinished()
        }
      }
      break;
  }
}

// Проверка завершения сеанса (для вызова из main loop)
bool isSessionJustFinished() {
  static SessionState last_state = STATE_IDLE;
  bool just_finished = (last_state != STATE_IDLE && current_state == STATE_IDLE);
  last_state = current_state;
  return just_finished;
}

// === УТИЛИТЫ ===
const char* getModeName(StimMode mode) {
  switch (mode) {
    case MODE_TRNS: return "tRNS";
    case MODE_TDCS: return "tDCS";
    case MODE_TACS: return "tACS";
    default: return "Unknown";
  }
}

float getValidTACSFrequency(float target_Hz) {
  // Период пресета = SIGNAL_SAMPLES / SAMPLE_RATE секунд
  // Допустимые частоты: n * SAMPLE_RATE / SIGNAL_SAMPLES, где n = 1, 2, 3, ...
  // Минимум: 1 * 8000 / 16384 ≈ 0.488 Гц
  // Для tACS разумный минимум ~0.5 Гц
  
  float min_freq = 0.5f;  // Минимум 0.5 Гц
  float max_freq = 640.0f;  // Максимум 640 Гц (как у tRNS)
  
  // Ограничиваем диапазон
  if (target_Hz < min_freq) target_Hz = min_freq;
  if (target_Hz > max_freq) target_Hz = max_freq;
  
  // Находим ближайшую кратную частоту
  float fundamental = (float)SAMPLE_RATE / SIGNAL_SAMPLES;  // ~0.488 Гц
  float n = roundf(target_Hz / fundamental);
  if (n < 1.0f) n = 1.0f;
  
  return n * fundamental;
}

