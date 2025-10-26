#ifndef USB_COMMANDS_H
#define USB_COMMANDS_H

#include "protocol.h"
#include "config.h"

// ============================================================================
// === USB OTG COMMAND HANDLER ===
// ============================================================================
// Обработка команд от Android/PC через USB OTG

// Глобальный объект протокола
extern Protocol* usbProtocol;

// Инициализация USB протокола
void initUSBProtocol();

// Обработка входящих команд (вызывать в loop())
void processUSBCommands();

// Отправка периодических обновлений статуса
void sendPeriodicStatus();

// Wrapper функции для отправки логов через протокол
// Эти функции можно использовать вместо Serial.printf
void usbLog(const char* text);
void usbLogf(const char* format, ...);
void usbWarn(const char* text);
void usbError(const char* text);

#endif // USB_COMMANDS_H

