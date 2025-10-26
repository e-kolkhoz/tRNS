#include "digital_pot.h"
#include "usb_commands.h"

// Текущая позиция (программное отслеживание, т.к. читать из чипа нельзя)
static int x9c_position = 99;  // Предполагаем максимум на старте

// Отправка импульса INC
static void sendPulse() {
  digitalWrite(X9C_INC, LOW);
  delayMicroseconds(X9C_PULSE_DELAY_US);
  digitalWrite(X9C_INC, HIGH);
  delayMicroseconds(X9C_PULSE_DELAY_US);
}

// Инициализация цифрового потенциометра
void initDigitalPot() {
  pinMode(X9C_INC, OUTPUT);
  pinMode(X9C_UD, OUTPUT);
  pinMode(X9C_CS, OUTPUT);
  
  // Начальное состояние: неактивен
  digitalWrite(X9C_CS, HIGH);
  digitalWrite(X9C_INC, HIGH);
  digitalWrite(X9C_UD, HIGH);
  
  usbLog("X9C103S: Initialized");
  usbLog("X9C103S: Digital pot with EEPROM (100k write cycles)");
  usbWarn("X9C103S: Cannot read position, only write!");
  
  // Устанавливаем на максимальное сопротивление (безопасный старт)
  // Так как не знаем текущую позицию, делаем 100 шагов вниз для сброса в 0,
  // потом идём на нужную позицию
  setDigitalPotPosition(X9C_MAX_STEPS);
  usbLog("X9C103S: Set to max resistance (safe start)");
}

// Установка позиции потенциометра (0-99, с сохранением в EEPROM)
// ВАЖНО: Так как нельзя прочитать позицию из чипа, всегда делаем сброс в 0,
// потом идём на нужную позицию. Гарантия правильного значения.
void setDigitalPotPosition(int target_position) {
  if (target_position < 0) target_position = 0;
  if (target_position > X9C_MAX_STEPS) target_position = X9C_MAX_STEPS;
  
  digitalWrite(X9C_CS, LOW);  // Активируем
  
  // Шаг 1: Сброс в 0 (100 шагов вниз гарантируют минимум)
  digitalWrite(X9C_UD, LOW);  // DOWN
  for (int i = 0; i < 100; i++) {
    sendPulse();
  }
  
  // Шаг 2: Идём на целевую позицию
  digitalWrite(X9C_UD, HIGH);  // UP
  for (int i = 0; i < target_position; i++) {
    sendPulse();
  }
  
  digitalWrite(X9C_CS, HIGH);  // Деактивируем - сохранит в EEPROM!
  x9c_position = target_position;
  
  usbLogf("X9C103S: Position set to %d (EEPROM saved)", x9c_position);
}

// Получить текущую позицию (программное отслеживание)
int getDigitalPotPosition() {
  return x9c_position;
}

