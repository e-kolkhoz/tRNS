#include "protocol.h"
#include <stdarg.h>

// ============================================================================
// === PROTOCOL IMPLEMENTATION ===
// ============================================================================

Protocol::Protocol(Stream& stream) 
  : _stream(stream), 
    _cmd_handler(nullptr),
    _rx_index(0),
    _rx_state(WAIT_MAGIC_0),
    _rx_msg_type(0),
    _rx_payload_len(0),
    _rx_payload_received(0),
    _rx_crc_expected(0) {
}

// === CRC16-CCITT ===
// Polynomial: 0x1021 (x^16 + x^12 + x^5 + 1)
uint16_t Protocol::calcCRC16(const uint8_t* data, uint16_t len) {
  uint16_t crc = 0xFFFF;
  
  for (uint16_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  
  return crc;
}

// === SENDING ===

void Protocol::sendPacket(uint8_t msg_type, const uint8_t* payload, uint16_t len) {
  // Вычисляем CRC (type + len + payload)
  uint8_t header[3] = { msg_type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  
  uint16_t crc = 0xFFFF;
  // CRC от header
  for (int i = 0; i < 3; i++) {
    crc ^= ((uint16_t)header[i]) << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  // CRC от payload
  for (uint16_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)payload[i]) << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  
  // Отправляем пакет
  _stream.write(PROTOCOL_MAGIC_0);
  _stream.write(PROTOCOL_MAGIC_1);
  _stream.write(msg_type);
  _stream.write((uint8_t)(len & 0xFF));
  _stream.write((uint8_t)(len >> 8));
  
  if (len > 0 && payload != nullptr) {
    _stream.write(payload, len);
  }
  
  _stream.write((uint8_t)(crc & 0xFF));
  _stream.write((uint8_t)(crc >> 8));
}

void Protocol::sendText(const char* text) {
  sendPacket(MSG_TEXT, (const uint8_t*)text, strlen(text));
}

void Protocol::sendTextf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  sendText(buffer);
}

void Protocol::sendAck() {
  sendPacket(MSG_ACK, nullptr, 0);
}

void Protocol::sendError(const char* error_text) {
  sendPacket(MSG_ERROR, (const uint8_t*)error_text, strlen(error_text));
}

void Protocol::sendADCData(const int16_t* buffer, uint32_t sample_count) {
  // Отправляем данные порциями, если нужно
  // Пока отправляем всё за раз (max 40000 samples = 80KB)
  uint16_t byte_count = sample_count * sizeof(int16_t);
  sendPacket(MSG_ADC_DATA, (const uint8_t*)buffer, byte_count);
}

void Protocol::sendStatus(const DeviceStatus& status, const char* preset_name) {
  // Формат: [DeviceStatus] [preset_name string]
  uint16_t preset_len = preset_name ? strlen(preset_name) : 0;
  uint16_t total_len = sizeof(DeviceStatus) + preset_len;
  
  // Выделяем буфер для всего пакета
  uint8_t* buffer = (uint8_t*)malloc(total_len);
  if (buffer) {
    // Копируем структуру
    memcpy(buffer, &status, sizeof(DeviceStatus));
    // Копируем имя пресета
    if (preset_len > 0) {
      memcpy(buffer + sizeof(DeviceStatus), preset_name, preset_len);
    }
    // Отправляем
    sendPacket(MSG_STATUS, buffer, total_len);
    free(buffer);
  } else {
    // Fallback: отправляем без имени
    sendPacket(MSG_STATUS, (const uint8_t*)&status, sizeof(DeviceStatus));
  }
}

void Protocol::sendBinary(uint8_t msg_type, const uint8_t* data, uint16_t len) {
  sendPacket(msg_type, data, len);
}

// === RECEIVING ===

void Protocol::setCommandHandler(CommandHandler handler) {
  _cmd_handler = handler;
}

void Protocol::resetRxState() {
  _rx_state = WAIT_MAGIC_0;
  _rx_index = 0;
  _rx_payload_received = 0;
}

void Protocol::poll() {
  // Неблокирующее чтение входящих данных
  while (_stream.available()) {
    uint8_t byte = _stream.read();
    
    switch (_rx_state) {
      case WAIT_MAGIC_0:
        if (byte == PROTOCOL_MAGIC_0) {
          _rx_state = WAIT_MAGIC_1;
        }
        break;
        
      case WAIT_MAGIC_1:
        if (byte == PROTOCOL_MAGIC_1) {
          _rx_state = WAIT_TYPE;
        } else {
          resetRxState();
        }
        break;
        
      case WAIT_TYPE:
        _rx_msg_type = byte;
        _rx_state = WAIT_LEN_LOW;
        break;
        
      case WAIT_LEN_LOW:
        _rx_payload_len = byte;
        _rx_state = WAIT_LEN_HIGH;
        break;
        
      case WAIT_LEN_HIGH:
        _rx_payload_len |= ((uint16_t)byte) << 8;
        
        if (_rx_payload_len == 0) {
          // Нет payload, сразу ждём CRC
          _rx_state = WAIT_CRC_LOW;
        } else if (_rx_payload_len <= RX_BUFFER_SIZE) {
          // Payload помещается в буфер
          _rx_index = 0;
          _rx_payload_received = 0;
          _rx_state = WAIT_PAYLOAD;
        } else {
          // Слишком большой payload для команды!
          sendError("Payload too large");
          resetRxState();
        }
        break;
        
      case WAIT_PAYLOAD:
        _rx_buffer[_rx_index++] = byte;
        _rx_payload_received++;
        
        if (_rx_payload_received >= _rx_payload_len) {
          _rx_state = WAIT_CRC_LOW;
        }
        break;
        
      case WAIT_CRC_LOW:
        _rx_crc_expected = byte;
        _rx_state = WAIT_CRC_HIGH;
        break;
        
      case WAIT_CRC_HIGH:
        _rx_crc_expected |= ((uint16_t)byte) << 8;
        
        // Проверяем CRC
        uint8_t header[3] = { 
          _rx_msg_type, 
          (uint8_t)(_rx_payload_len & 0xFF), 
          (uint8_t)(_rx_payload_len >> 8) 
        };
        
        uint16_t crc = 0xFFFF;
        // CRC от header
        for (int i = 0; i < 3; i++) {
          crc ^= ((uint16_t)header[i]) << 8;
          for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
          }
        }
        // CRC от payload
        for (uint16_t i = 0; i < _rx_payload_len; i++) {
          crc ^= ((uint16_t)_rx_buffer[i]) << 8;
          for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
          }
        }
        
        if (crc == _rx_crc_expected) {
          // CRC OK - обрабатываем команду
          processCommand(_rx_msg_type, _rx_buffer, _rx_payload_len);
        } else {
          // CRC ошибка
          sendError("CRC mismatch");
        }
        
        resetRxState();
        break;
    }
  }
}

void Protocol::processCommand(uint8_t cmd, const uint8_t* payload, uint16_t len) {
  // Вызываем callback если установлен
  if (_cmd_handler != nullptr) {
    _cmd_handler(cmd, payload, len);
  }
}


