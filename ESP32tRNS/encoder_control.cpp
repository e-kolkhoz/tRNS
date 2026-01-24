#include "encoder_control.h"
#include "menu_control.h"

// Объект энкодера из библиотеки EncButton
// Параметры: S1, S2, KEY
static EncButton enc(ENC_S1, ENC_S2, ENC_KEY);

// Защита от дребезга кнопки — ЕДИНСТВЕННЫЙ механизм
static volatile uint32_t last_button_time_ms = 0;
static const uint32_t BUTTON_DEBOUNCE_MS = 300;  // 300мс между кликами — надёжно!

// Флаг клика (из ISR в loop)
static volatile bool pending_click = false;

// ISR функция для обработки прерываний от энкодера (вращение)
void IRAM_ATTR encoderISR() {
  enc.tickISR();
}

// ISR для кнопки — ТОЛЬКО устанавливает флаг, debounce в loop!
void IRAM_ATTR encoderKeyISR() {
  pending_click = true;
}

// Инициализация энкодера
void initEncoder() {
  // Настройка типа энкодера
  enc.setEncType(EB_STEP4_LOW);
  
  // Debounce для вращения
  enc.setDebTimeout(30);
  enc.setClickTimeout(400);
  
  // Прерывания для вращения
  attachInterrupt(digitalPinToInterrupt(ENC_S1), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_S2), encoderISR, CHANGE);

  // Кнопка с прерыванием
  pinMode(ENC_KEY, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_KEY), encoderKeyISR, FALLING);
  
  last_button_time_ms = millis();
}

// Опрос энкодера (вызывать в loop)
void updateEncoder() {
  enc.tick();
  
  // === Вращение ===
  if (enc.left()) {
    handleRotate(+1);
  }
  if (enc.right()) {
    handleRotate(-1);
  }
  
  // === Клик кнопки — ЕДИНСТВЕННЫЙ debounce здесь ===
  if (pending_click) {
    // СРАЗУ сбрасываем флаг (атомарно)
    pending_click = false;
    
    uint32_t now_ms = millis();
    uint32_t elapsed = now_ms - last_button_time_ms;
    
    // Проверяем debounce
    if (elapsed >= BUTTON_DEBOUNCE_MS) {
      // Дополнительная проверка: кнопка РЕАЛЬНО нажата?
      if (digitalRead(ENC_KEY) == LOW) {
        last_button_time_ms = now_ms;
        Serial.printf("[ENC] CLICK! (elapsed=%lu ms)\n", elapsed);
        handleClick();
      }
    }
  }
}

