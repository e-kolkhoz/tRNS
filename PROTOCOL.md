# USB OTG Binary Protocol

Бинарный протокол для взаимодействия ESP32-S2 с Android/PC через USB CDC.

## Формат пакета

```
┌────────────────────────────────────────────────────┐
│ Magic (2) │ Type (1) │ Length (2) │ Payload │ CRC16 │
└────────────────────────────────────────────────────┘
   0xAA55     1 byte     uint16 LE    N bytes   uint16 LE

Overhead: 7 байт
CRC: CRC16-CCITT (polynomial 0x1021)
```

## Типы сообщений

### ESP32 → Host (0x01-0x7F)

| Type | Название | Payload | Описание |
|------|----------|---------|----------|
| `0x01` | TEXT_MESSAGE | string | Текстовые логи, warnings |
| `0x02` | ADC_DATA | int16[] | ADC буфер (40000 samples) |
| `0x03` | STATUS | struct | Статус устройства |
| `0x04` | ACK | empty | Подтверждение команды |
| `0x05` | ERROR | string | Ошибка выполнения |

### Host → ESP32 (0x80-0xFF)

| Type | Название | Payload | Описание |
|------|----------|---------|----------|
| `0x81` | SET_POT | uint8 (0-99) | Установить потенциометр |
| `0x87` | GET_POT | empty | Получить позицию потенциометра |
| `0x82` | GET_ADC | empty | Запросить ADC буфер |
| `0x83` | SET_DAC | int16[] mono + name | Загрузить DAC буфер (МОНО + имя пресета) |
| `0x84` | SET_PARAMS | TBD | Параметры (freq, amp) |
| `0x85` | GET_STATUS | empty | Запросить статус |
| `0x86` | RESET | empty | Сброс устройства |

## Структура STATUS

```c
struct DeviceStatus {
  uint8_t pot_position;   // Позиция потенциометра (0-99)
  uint32_t adc_samples;   // Количество собранных ADC сэмплов
  uint16_t adc_rate;      // Частота ADC (Hz)
  uint8_t error_flags;    // Флаги ошибок
  // После структуры идёт строка имени пресета (переменной длины)
} __attribute__((packed));

Размер: 8 байт фиксированный + preset_name (переменная длина)
```

**Формат payload:**
```
[DeviceStatus: 8 bytes] [preset_name: string до конца пакета]
```

**Пример preset_name:**
- `"tACS 640Hz 1mA demo"`
- `"tRNS 100-640 normal 1mA"`
- `"tDCS 1mA"`

## Примеры пакетов

### 1. Установить потенциометр на 75

**Отправка (Host → ESP32):**
```
AA 55          # Magic
81             # CMD_SET_POT
01 00          # Length = 1
4B             # Payload: 75
XX XX          # CRC16
```

**Ответ (ESP32 → Host):**
```
AA 55          # Magic
04             # MSG_ACK
00 00          # Length = 0
XX XX          # CRC16
```

### 2. Получить позицию потенциометра

**Отправка (Host → ESP32):**
```
AA 55          # Magic
87             # CMD_GET_POT
00 00          # Length = 0
XX XX          # CRC16
```

**Ответ (ESP32 → Host):**
```
AA 55          # Magic
04             # MSG_ACK
01 00          # Length = 1
4B             # Payload: 75 (позиция)
XX XX          # CRC16
```

### 3. Запросить ADC буфер

**Отправка:**
```
AA 55          # Magic
82             # CMD_GET_ADC
00 00          # Length = 0
XX XX          # CRC16
```

**Ответ:**
```
AA 55          # Magic
02             # MSG_ADC_DATA
40 9C          # Length = 40000 (0x9C40)
[80KB data]    # 40000 × int16
XX XX          # CRC16
```

### 4. Загрузить DAC буфер (пресет)

**Отправка:**
```
AA 55                      # Magic
83                         # CMD_SET_DAC
2C 7D                      # Length = 32012 (0x7D2C)
                           #   = 32000 bytes MONO + 12 bytes name
[32000 bytes]              # int16[] MONO (16000 samples)
"tACS 640Hz\0"             # Preset name (null-terminated)
XX XX                      # CRC16
```

**Формат payload:**
```
[int16_t buffer[SIGNAL_SAMPLES]] + [char preset_name[]]
  ^--- МОНО (только правый канал)    ^--- переменная длина
  32000 bytes (16000 samples)
```

**Важно:** Левый канал = константа (-0.5V), не передаётся!

### 5. Текстовое сообщение (лог)

**ESP32 → Host:**
```
AA 55               # Magic
01                  # MSG_TEXT
10 00               # Length = 16
"WARN: test\n"      # Payload
XX XX               # CRC16
```

## Python пример

```python
from test_device import tRNSDevice
import numpy as np

# Подключение
dev = tRNSDevice('/dev/ttyACM0')

# Установить потенциометр
dev.set_pot(50)

# Получить позицию потенциометра
position = dev.get_pot_position()
print(f"Pot: {position}")

# Получить ADC данные
adc_data = dev.get_adc_data()

# Получить статус (с именем пресета)
status = dev.get_status()
print(f"Preset: {status['preset_name']}")
print(f"Pot: {status['pot_position']}")

# Загрузить новый пресет (МОНО буфер + имя)
buffer = np.sin(2 * np.pi * 640 * np.arange(16000) / 8000)
buffer = (buffer * 10000).astype(np.int16)  # МОНО!
dev.upload_dac_buffer(buffer, "tACS 640Hz custom")
```

## Особенности

### ✅ Преимущества

- **Бинарный** - эффективная передача больших данных (80 KB ADC)
- **Magic bytes** - надёжная синхронизация при рассинхронизации
- **CRC16** - проверка целостности данных
- **Неблокирующий** - не мешает работе DMA генератора
- **Простой парсинг** - state machine для приёма
- **Читаемые логи** - текстовые сообщения внутри протокола

### 🎯 Производительность

- Скорость: **921600 baud** ≈ 92 KB/s теоретически
- ADC буфер (80 KB): **~1 секунда** передачи
- DAC буфер МОНО (32 KB): **~350 мс** передачи (вместо 700 мс для стерео!)
- DMA буфер DAC: **2 сек** звука → успевает с запасом!
- Команды: **< 1 мс** (малые пакеты)

**Экономия:** DAC передача **x2 быстрее** (32KB вместо 64KB), т.к. левый канал = константа

### 🔧 Android реализация

Для Android использовать библиотеку:
```gradle
implementation 'com.github.mik3y:usb-serial-for-android:3.5.1'
```

Протокол идентичный - те же magic bytes, CRC16, структуры.

## Файлы

- `protocol.h/cpp` - базовый протокол (парсинг, CRC)
- `usb_commands.h/cpp` - обработка команд устройства
- `test_device.py` - Python клиент для тестирования

