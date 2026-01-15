#include "encoder_control.h"
#include "menu_control.h"

// Объект энкодера из библиотеки EncButton
// Параметры: S1, S2, KEY
static EncButton enc(ENC_S1, ENC_S2, ENC_KEY);

// ISR функция для обработки прерываний от энкодера
void IRAM_ATTR encoderISR() {
  enc.tickISR();  // Специальный метод для ISR - быстрый опрос
}

// Инициализация энкодера
void initEncoder() {
  // Библиотека EncButton сама настраивает пины как INPUT_PULLUP
  
  // Настройка типа энкодера
  enc.setEncType(EB_STEP4_LOW);  // Можно попробовать EB_STEP2 или EB_STEP1
  
  // ВАЖНО: Подключаем прерывания к обоим пинам энкодера!
  // CHANGE = прерывание на любое изменение (HIGH→LOW или LOW→HIGH)
  attachInterrupt(digitalPinToInterrupt(ENC_S1), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_S2), encoderISR, CHANGE);
  
  // Кнопку оставляем на polling в tick() - это нормально для кнопки
}

// Опрос энкодера (вызывать в loop)
void updateEncoder() {
  // ВАЖНО: tick() нужен для обработки КНОПКИ!
  // Вращение обрабатывается в ISR через tickISR()
  enc.tick();
  
  // === Вращение ===
  if (enc.left()) {
    handleRotate(+1);  // По часовой = +1
  }
  if (enc.right()) {
    handleRotate(-1);  // Против часовой = -1
  }
  
  // === Клик кнопки ===
  if (enc.click()) {
    handleClick();
  }
}

