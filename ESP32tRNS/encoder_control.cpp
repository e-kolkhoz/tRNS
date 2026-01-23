#include "encoder_control.h"
#include "menu_control.h"

// Объект энкодера из библиотеки EncButton
// Параметры: S1, S2, KEY
static EncButton enc(ENC_S1, ENC_S2, ENC_KEY);
static volatile bool enc_button_irq = false;
static volatile uint32_t last_button_irq_us = 0;
static uint32_t last_button_us = 0;

// ISR функция для обработки прерываний от энкодера
void IRAM_ATTR encoderISR() {
  enc.tickISR();  // Специальный метод для ISR - быстрый опрос
}

// ISR для кнопки энкодера (фиксируем событие, обработка в loop)
void IRAM_ATTR encoderKeyISR() {
  uint32_t now_us = micros();
  // Жёсткий debounce в ISR — увеличен, т.к. loop() теперь быстрее
  if (now_us - last_button_irq_us > 100000) {  // 100мс
    last_button_irq_us = now_us;
    enc_button_irq = true;
  }
}

// Инициализация энкодера
void initEncoder() {
  // Библиотека EncButton сама настраивает пины как INPUT_PULLUP
  
  // Настройка типа энкодера
  enc.setEncType(EB_STEP4_LOW);  // Можно попробовать EB_STEP2 или EB_STEP1
  
  // Уменьшаем debounce для более отзывчивой кнопки (по умолчанию 50мс)
  enc.setDebTimeout(20);  // 20мс debounce
  enc.setClickTimeout(300);  // Таймаут клика 300мс
  
  // ВАЖНО: Подключаем прерывания к пинам энкодера (вращение)
  attachInterrupt(digitalPinToInterrupt(ENC_S1), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_S2), encoderISR, CHANGE);

  // Кнопку обрабатываем своим ISR (чтобы не терять клики во время рендера)
  pinMode(ENC_KEY, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_KEY), encoderKeyISR, FALLING);
}

// Опрос энкодера (вызывать в loop)
void updateEncoder() {
  // ВАЖНО: tick() нужен для обработки КНОПКИ!
  // Вращение обрабатывается в ISR через tickISR()
  enc.tick();
  
  // === Вращение ===
  if (enc.left()) {
    Serial.println("[ENC] Rotate LEFT");
    handleRotate(+1);  // По часовой = +1
  }
  if (enc.right()) {
    Serial.println("[ENC] Rotate RIGHT");
    handleRotate(-1);  // Против часовой = -1
  }
  
  // === Клик кнопки ===
  if (enc_button_irq) {
    uint32_t now_us = micros();
    if (now_us - last_button_us > 150000) {  // 150мс debounce — loop теперь быстрее
      last_button_us = now_us;
      enc_button_irq = false;
      Serial.println("[ENC] Button CLICK detected!");
      handleClick();
    } else {
      enc_button_irq = false;  // Сбрасываем ложный клик
    }
  }
}

