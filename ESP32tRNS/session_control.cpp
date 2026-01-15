#include "session_control.h"
#include "dac_control.h"
#include "preset_storage.h"
#include <EEPROM.h>
#include <math.h>

// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ===
SessionSettings current_settings;
SessionState current_state = STATE_IDLE;
uint32_t session_elapsed_sec = 0;  // Фактическое время последнего сеанса (секунды)

// === ВНУТРЕННИЕ ПЕРЕМЕННЫЕ ===
static uint32_t session_start_time = 0;     // Время старта сеанса
static uint32_t fadein_duration_ms = 5000;  // Длительность fadein (5 сек)
static uint32_t fadeout_duration_ms = 5000; // Длительность fadeout (5 сек)

// EEPROM адреса
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xA5C3  // Магическое число для проверки валидности
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_SETTINGS 2

// Дефолтные настройки
static const SessionSettings default_settings = {
  .mode = MODE_TRNS,
  .amplitude_mA = 1.0f,
  .duration_min = 20,
  .frequency_Hz = 140.0f  // Для tACS
};

// === ИНИЦИАЛИЗАЦИЯ ===
void initSession() {
  EEPROM.begin(EEPROM_SIZE);
  loadSettings();
  current_state = STATE_IDLE;
}

// === EEPROM ===
void loadSettings() {
  uint16_t magic = EEPROM.readUShort(EEPROM_ADDR_MAGIC);
  
  if (magic == EEPROM_MAGIC) {
    // EEPROM валиден - загружаем
    EEPROM.get(EEPROM_ADDR_SETTINGS, current_settings);
  } else {
    // EEPROM пуст - используем дефолт БЕЗ сохранения
    current_settings = default_settings;
    // saveSettings() вызовется при первом изменении настроек
  }
}

void saveSettings() {
  // Проверяем, нужно ли сохранять (защита от лишних записей)
  SessionSettings stored_settings;
  EEPROM.get(EEPROM_ADDR_SETTINGS, stored_settings);
  
  // Сравниваем все поля
  if (stored_settings.mode == current_settings.mode &&
      stored_settings.amplitude_mA == current_settings.amplitude_mA &&
      stored_settings.frequency_Hz == current_settings.frequency_Hz &&
      stored_settings.duration_min == current_settings.duration_min) {
    // Настройки не изменились, не пишем
    return;
  }
  
  // Настройки изменились, сохраняем
  EEPROM.writeUShort(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_SETTINGS, current_settings);
  EEPROM.commit();
}

// === ГЕНЕРАЦИЯ СИГНАЛОВ ===

// Генератор tDCS - константа
static void generateTDCS() {
  // Постоянное значение = максимум (gain будет регулировать амплитуду)
  for (uint32_t i = 0; i < SIGNAL_SAMPLES; i++) {
    signal_buffer[i] = MAX_VAL;  // Максимальное положительное значение
  }
  snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tDCS %.1fmA %dmin", 
           current_settings.amplitude_mA, current_settings.duration_min);
}

// Генератор tACS - синусоида
static void generateTACS() {
  float freq = getValidTACSFrequency(current_settings.frequency_Hz);
  float omega = 2.0f * PI * freq / SAMPLE_RATE;
  
  for (uint32_t i = 0; i < SIGNAL_SAMPLES; i++) {
    // Синусоида от -MAX_VAL до +MAX_VAL
    float sample = sinf(omega * i) * MAX_VAL;
    signal_buffer[i] = (int16_t)sample;
  }
  
  snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tACS %.1fHz %.1fmA", 
           freq, current_settings.amplitude_mA);
}

// Главная функция генерации
void generateSignal() {
  switch (current_settings.mode) {
    case MODE_TRNS:
      // tRNS уже загружен из PROGMEM в loadPresetFromFlash()
      // Просто обновляем имя
      snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "tRNS 100-640Hz %.1fmA", 
               current_settings.amplitude_mA);
      break;
      
    case MODE_TDCS:
      generateTDCS();
      break;
      
    case MODE_TACS:
      generateTACS();
      break;
  }
  
  // Обновляем gain для нужной амплитуды
  // Расчёт: gain = (amplitude_mA / max_mA) * DEFAULT_GAIN
  // Предполагаем max_mA = 2.0 мА при gain = 1.0
  dac_gain = (current_settings.amplitude_mA / 2.0f);
  if (dac_gain > 1.0f) dac_gain = 1.0f;  // Ограничение
  if (dac_gain < 0.0f) dac_gain = 0.0f;
}

// === УПРАВЛЕНИЕ СЕАНСОМ ===
void startSession() {
  if (current_state == STATE_IDLE) {
    // Генерируем сигнал для текущего режима
    if (current_settings.mode != MODE_TRNS) {
      generateSignal();  // tDCS/tACS генерятся на лету
    }
    
    session_start_time = millis();
    current_state = STATE_FADEIN;
  }
}

void stopSession() {
  if (current_state == STATE_STABLE || current_state == STATE_FADEIN) {
    // Сохраняем фактическое время сеанса (с учётом fadein)
    uint32_t total_elapsed_ms = millis() - session_start_time;
    if (current_state == STATE_STABLE) {
      // В stable: добавляем время fadein
      session_elapsed_sec = (fadein_duration_ms + total_elapsed_ms) / 1000;
    } else {
      // В fadein: только фактическое время
      session_elapsed_sec = total_elapsed_ms / 1000;
    }
    
    current_state = STATE_FADEOUT;
    session_start_time = millis();  // Перезапускаем таймер для fadeout
  }
}

void updateSession() {
  uint32_t now = millis();
  uint32_t elapsed = now - session_start_time;
  
  switch (current_state) {
    case STATE_IDLE:
      // Ничего не делаем
      break;
      
    case STATE_FADEIN:
      if (elapsed >= fadein_duration_ms) {
        // Fadein завершён → переход в stable
        current_state = STATE_STABLE;
        session_start_time = now;
      } else {
        // Плавно увеличиваем gain
        float progress = (float)elapsed / fadein_duration_ms;
        float target_gain = (current_settings.amplitude_mA / 2.0f);
        dac_gain = target_gain * progress;
      }
      break;
      
    case STATE_STABLE:
      {
        uint32_t duration_ms = current_settings.duration_min * 60UL * 1000UL;
        if (elapsed >= duration_ms) {
          // Время вышло → автоматический fadeout
          stopSession();
        }
      }
      break;
      
    case STATE_FADEOUT:
      if (elapsed >= fadeout_duration_ms) {
        // Fadeout завершён → переход в idle
        current_state = STATE_IDLE;
        dac_gain = 0.0f;
        // Показываем экран завершения (если это было автозавершение)
        // Вызывается из menu_control после обнаружения STATE_IDLE
      } else {
        // Плавно уменьшаем gain
        float progress = 1.0f - ((float)elapsed / fadeout_duration_ms);
        float target_gain = (current_settings.amplitude_mA / 2.0f);
        dac_gain = target_gain * progress;
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

