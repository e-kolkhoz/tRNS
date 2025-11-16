#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

// ============================================================================
// === BINARY PROTOCOL for USB OTG Communication ===
// ============================================================================
// 
// Packet format:
// ┌──────────────────────────────────────────────────┐
// │ Magic (2) │ Type (1) │ Len (4) │ Payload │ CRC16 │
// └──────────────────────────────────────────────────┘
//    0xAA55     1 byte    uint32    N bytes   uint16
//
// Total overhead: 9 bytes per packet

// === PROTOCOL CONSTANTS ===
#define PROTOCOL_MAGIC_0    0xAA
#define PROTOCOL_MAGIC_1    0x55
#define PROTOCOL_OVERHEAD   9          // magic(2) + type(1) + len(4) + crc(2)
#define PROTOCOL_MAX_PAYLOAD 0xFFFFFFFF  // uint32 max (4GB)

// === MESSAGE TYPES ===

// ESP32 → Host (responses, 0x01-0x7F)
enum HostMessageType : uint8_t {
  MSG_TEXT        = 0x01,  // Текстовое сообщение (логи, warnings)
  MSG_ADC_DATA    = 0x02,  // ADC буфер данных
  MSG_STATUS      = 0x03,  // Статус устройства
  MSG_ACK         = 0x04,  // Подтверждение команды (OK)
  MSG_ERROR       = 0x05,  // Ошибка выполнения команды
};

// Host → ESP32 (commands, 0x80-0xFF)
enum DeviceCommand : uint8_t {
  CMD_GET_ADC     = 0x82,  // Запросить ADC буфер
  CMD_SET_DAC     = 0x83,  // Загрузить DAC буфер (payload: samples)
  CMD_SET_PARAMS  = 0x84,  // Установить параметры (freq, amp)
  CMD_GET_STATUS  = 0x85,  // Запросить статус
  CMD_RESET       = 0x86,  // Сброс устройства
  CMD_SET_GAIN    = 0x88,  // Установить gain (float32)
  CMD_GET_GAIN    = 0x89,  // Получить текущий gain
};

// === STATUS STRUCTURE ===
// ВАЖНО: После структуры идёт строка имени пресета (переменной длины)
// Формат: [DeviceStatus fixed] [preset_name: variable length string]
struct DeviceStatus {
  uint32_t adc_samples;     // Количество собранных ADC сэмплов
  uint16_t adc_rate;        // Частота ADC (Hz)
  float gain;               // Коэффициент усиления (gain)
  uint8_t error_flags;      // Флаги ошибок
  // preset_name идёт ПОСЛЕ структуры (переменная длина, до конца пакета)
} __attribute__((packed));

// === PROTOCOL CLASS ===
class Protocol {
public:
  Protocol(Stream& stream);
  
  // === SENDING ===
  
  // Отправить текстовое сообщение (для логов, warnings)
  void sendText(const char* text);
  void sendTextf(const char* format, ...);  // printf-style
  
  // Отправить ACK
  void sendAck();
  
  // Отправить ERROR с текстом
  void sendError(const char* error_text);
  
  // Отправить ADC буфер
  void sendADCData(const int16_t* buffer, uint32_t sample_count);
  
  // Отправить статус
  void sendStatus(const DeviceStatus& status, const char* preset_name);
  
  // Отправить произвольные бинарные данные
  void sendBinary(uint8_t msg_type, const uint8_t* data, uint32_t len);
  
  // === RECEIVING ===
  
  // Проверить и обработать входящие команды (неблокирующе!)
  // Вызывать в loop()
  void poll();
  
  // Callback для обработки команд (установить через setCommandHandler)
  typedef void (*CommandHandler)(uint8_t cmd, const uint8_t* payload, uint32_t len);
  void setCommandHandler(CommandHandler handler);
  
private:
  Stream& _stream;
  CommandHandler _cmd_handler;
  
  // Буфер для приёма
  static const uint16_t RX_BUFFER_SIZE = 256;  // Для заголовков и малых команд
  uint8_t _rx_buffer[RX_BUFFER_SIZE];
  uint16_t _rx_index;
  
  // Состояние приёмника
  enum RxState {
    WAIT_MAGIC_0,
    WAIT_MAGIC_1,
    WAIT_TYPE,
    WAIT_LEN_0,
    WAIT_LEN_1,
    WAIT_LEN_2,
    WAIT_LEN_3,
    WAIT_PAYLOAD,
    WAIT_CRC_LOW,
    WAIT_CRC_HIGH
  };
  RxState _rx_state;
  uint8_t _rx_msg_type;
  uint32_t _rx_payload_len;
  uint32_t _rx_payload_received;
  uint16_t _rx_crc_expected;
  
  // Внутренние функции
  void sendPacket(uint8_t msg_type, const uint8_t* payload, uint32_t len);
  uint16_t calcCRC16(const uint8_t* data, uint32_t len);
  void processCommand(uint8_t cmd, const uint8_t* payload, uint32_t len);
  void resetRxState();
};

#endif // PROTOCOL_H

