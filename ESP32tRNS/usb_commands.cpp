#include "usb_commands.h"
#include "adc_control.h"
#include "dac_control.h"
#include "preset_storage.h"
#include <stdarg.h>

// Check if USB CDC is ready
static inline bool isUSBReady() {
  // Просто проверяем что Serial инициализирован и доступен для записи
  return Serial && Serial.availableForWrite() > 0;
}


// Глобальный объект протокола
Protocol* usbProtocol = nullptr;

// Последнее время отправки статуса
static unsigned long lastStatusSendTime = 0;
static const unsigned long STATUS_SEND_INTERVAL = 1000; // 1 сек

// Callback для обработки команд
static void commandHandler(uint8_t cmd, const uint8_t* payload, uint32_t len) {
  // Диагностика: логируем все входящие команды
  usbLogf("CMD received: 0x%02X, len=%u", cmd, len);
  
  switch (cmd) {
    case CMD_GET_ADC:
      // Отправить ADC буфер
      {
        if (!usbProtocol) {
          break; // Протокол не инициализирован
        }
        
        uint32_t write_pos;
        int16_t* temp_buffer = (int16_t*)malloc(ADC_RING_SIZE * sizeof(int16_t));
        
        if (!temp_buffer) {
          usbProtocol->sendError("ADC: Out of memory");
          break;
        }
        
        // Копируем данные из кольцевого буфера
        getADCRingBuffer(temp_buffer, &write_pos);
        
        // Отправляем данные (только если USB готов)
        if (isUSBReady()) {
          usbProtocol->sendADCData(temp_buffer, ADC_RING_SIZE);
          usbLogf("ADC buffer sent (%u samples, write_pos=%u)", ADC_RING_SIZE, write_pos);
        }
        
        free(temp_buffer);
      }
      break;
      
    case CMD_SET_DAC:
      // Загрузить новый DAC буфер + имя пресета
      // Формат: [buffer MONO: SIGNAL_SAMPLES*2 bytes] [preset_name: string до конца]
      {
        const uint16_t buffer_size = SIGNAL_SAMPLES * sizeof(int16_t); // 32000 bytes (МОНО!)
        
        if (len >= buffer_size && usbProtocol && isUSBReady()) {
          // Копируем МОНО буфер (только правый канал)
          memcpy(signal_buffer, payload, buffer_size);
          
          // Читаем имя пресета (если есть)
          uint16_t preset_name_len = len - buffer_size;
          if (preset_name_len > 0) {
            // Ограничиваем длину
            if (preset_name_len >= PRESET_NAME_MAX_LEN) {
              preset_name_len = PRESET_NAME_MAX_LEN - 1;
            }
            memcpy(current_preset_name, payload + buffer_size, preset_name_len);
            current_preset_name[preset_name_len] = '\0'; // null-terminate
          } else {
            // Нет имени - используем дефолтное
            snprintf(current_preset_name, PRESET_NAME_MAX_LEN, "Custom preset");
          }
          
          // Перезаполняем DMA новым сигналом
          setSignalBuffer(signal_buffer, SIGNAL_SAMPLES);
          
          // Сохраняем пресет во flash для автозагрузки при старте
          if (!savePresetToFlash(signal_buffer, current_preset_name)) {
            usbWarn("Failed to save preset to flash");
          }
          
          usbProtocol->sendAck();
          usbLogf("DAC buffer updated (MONO): '%s'", current_preset_name);
        } else {
          usbProtocol->sendError("DAC: Buffer too small");
          usbLogf("DAC: Expected at least %u bytes MONO, got %u", buffer_size, len);
        }
      }
      break;
      
    case CMD_SET_PARAMS:
      // Установить параметры (freq, amp и т.д.)
      // TODO: реализовать парсинг параметров
      usbProtocol->sendError("SET_PARAMS: Not implemented yet");
      break;
      
    case CMD_SET_GAIN:
      // Установить gain (коэффициент усиления)
      if (len >= sizeof(float) && usbProtocol && isUSBReady()) {
        float gain;
        memcpy(&gain, payload, sizeof(float));
        setDACGain(gain);
        usbProtocol->sendAck();
      } else {
        usbProtocol->sendError("GAIN: Missing parameter (float32)");
      }
      break;
      
    case CMD_GET_GAIN:
      // Получить текущий gain
      if (usbProtocol && isUSBReady()) {
        float gain = getDACGain();
        usbProtocol->sendBinary(MSG_ACK, (uint8_t*)&gain, sizeof(float));
        usbLogf("GAIN: Current gain = %.2f", gain);
      }
      break;
      
    case CMD_GET_STATUS:
      // Отправить статус + имя пресета
      if (usbProtocol && isUSBReady()) {
        DeviceStatus status;
        status.adc_samples = ADC_RING_SIZE;
        status.adc_rate = ADC_SAMPLE_RATE;
        status.gain = getDACGain();
        status.error_flags = 0; // TODO: добавить флаги ошибок
        
        usbProtocol->sendStatus(status, current_preset_name);
      }
      break;
      
    case CMD_RESET:
      // Сброс устройства
      if (usbProtocol && isUSBReady()) {
        usbProtocol->sendAck();
        usbLog("Resetting device...");
        delay(100);
        ESP.restart();
      }
      break;
      
    default:
      usbProtocol->sendError("Unknown command");
      break;
  }
}

// Инициализация USB протокола
void initUSBProtocol() {
  usbProtocol = new Protocol(Serial);
  usbProtocol->setCommandHandler(commandHandler);
  
  // НЕ логируем через протокол здесь - протокол только что создан!
  // Первые сообщения пойдут уже через usbLog из других мест
}

// Обработка входящих команд (вызывать в loop())
void processUSBCommands() {
  if (usbProtocol) {
    usbProtocol->poll();
  }
}

// Отправка периодических обновлений статуса (опционально)
void sendPeriodicStatus() {
  unsigned long now = millis();
  
  if (now - lastStatusSendTime >= STATUS_SEND_INTERVAL) {
    lastStatusSendTime = now;
    
    DeviceStatus status;
    status.adc_samples = adc_write_index; // Текущая позиция записи
    status.adc_rate = ADC_SAMPLE_RATE;
    status.gain = getDACGain();
    status.error_flags = 0;
    
    if (usbProtocol) {
      usbProtocol->sendStatus(status, current_preset_name);
    }
  }
}

// === LOGGING WRAPPERS ===

void usbLog(const char* text) {
  if (usbProtocol && isUSBReady()) {
    usbProtocol->sendText(text);
  }
}

void usbLogf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  
  usbLog(buffer);
}

void usbWarn(const char* text) {
  char buffer[300];
  snprintf(buffer, sizeof(buffer), "WARN: %s", text);
  usbLog(buffer);
}

void usbError(const char* text) {
  if (usbProtocol && isUSBReady()) {
    usbProtocol->sendError(text);
  }
}

