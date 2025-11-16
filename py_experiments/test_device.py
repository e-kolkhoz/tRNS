#!/usr/bin/env python3
"""
Пример работы с tRNS/tACS устройством через USB OTG
Бинарный протокол с magic bytes + CRC16

Обновления протокола:
- DeviceStatus: добавлено имя пресета (переменная длина) и gain
- CMD_SET_DAC: МОНО буфер (32KB) + имя пресета
  * Левый канал = константа (-0.5V), формируется на ESP32
  * Правый канал = сигнал (tRNS/tACS/tDCS)
- Цифровой потенциометр заменён на программный gain (CMD_SET_GAIN / CMD_GET_GAIN)
  * Применяется к правому каналу с насыщением int16
  * По умолчанию 1.0, можно любой ≥ 0.0
"""

import serial
import struct
import numpy as np
import time

# === PROTOCOL CONSTANTS ===
PROTOCOL_MAGIC = b'\xAA\x55'

# Message types (ESP32 → Host)
MSG_TEXT = 0x01
MSG_ADC_DATA = 0x02
MSG_STATUS = 0x03
MSG_ACK = 0x04
MSG_ERROR = 0x05

# Commands (Host → ESP32)
CMD_GET_ADC = 0x82
CMD_SET_DAC = 0x83
CMD_SET_PARAMS = 0x84
CMD_GET_STATUS = 0x85
CMD_RESET = 0x86
CMD_SET_GAIN = 0x88
CMD_GET_GAIN = 0x89


def calc_crc16(data):
    """CRC16-CCITT (polynomial 0x1021)"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc


class tRNSDevice:
    def __init__(self, port='/dev/ttyACM0', baudrate=921600):
        self.ser = serial.Serial(port, baudrate, timeout=1)
        time.sleep(0.1)  # Дать устройству время
        
    def send_command(self, cmd, payload=b''):
        """Отправить команду устройству"""
        # Формируем пакет (теперь length = uint32)
        msg_type = cmd
        length = len(payload)
        
        # Вычисляем CRC (header теперь 5 байт: type + len32)
        header = struct.pack('<BI', msg_type, length)
        crc_data = header + payload
        crc = calc_crc16(crc_data)
        
        # Отправляем пакет
        packet = PROTOCOL_MAGIC + header + payload + struct.pack('<H', crc)
        self.ser.write(packet)
        self.ser.flush()
    
    def receive_packet(self, timeout=2.0):
        """Получить пакет от устройства"""
        start_time = time.time()
        
        # Ищем magic bytes
        while (time.time() - start_time) < timeout:
            byte = self.ser.read(1)
            if not byte:
                continue
                
            if byte == b'\xAA':
                byte2 = self.ser.read(1)
                if byte2 == b'\x55':
                    # Нашли magic!
                    break
        else:
            raise TimeoutError("Magic bytes not found")
        
        # Читаем заголовок (type(1) + length(4) = 5 байт)
        header = self.ser.read(5)
        if len(header) < 5:
            raise ValueError("Incomplete header")
        
        msg_type, length = struct.unpack('<BI', header)
        
        # Читаем payload
        payload = b''
        if length > 0:
            payload = self.ser.read(length)
            if len(payload) < length:
                raise ValueError("Incomplete payload")
        
        # Читаем CRC
        crc_bytes = self.ser.read(2)
        if len(crc_bytes) < 2:
            raise ValueError("Incomplete CRC")
        
        crc_received = struct.unpack('<H', crc_bytes)[0]
        
        # Проверяем CRC
        crc_data = header + payload
        crc_calculated = calc_crc16(crc_data)
        
        if crc_received != crc_calculated:
            raise ValueError(f"CRC mismatch: {crc_received:04X} != {crc_calculated:04X}")
        
        # Диагностика: показываем текстовые логи от ESP32
        if msg_type == MSG_TEXT:
            text = payload.decode('utf-8', errors='ignore')
            print(f"[ESP32 LOG] {text}")
        
        return msg_type, payload
    
    def get_adc_data(self):
        """Получить ADC буфер (теперь приходит одним пакетом с uint32 length)"""
        print("Запрос ADC данных...")
        self.send_command(CMD_GET_ADC)
        
        # Читаем пакеты, пропуская логи
        start_time = time.time()
        timeout = 5.0
        
        while (time.time() - start_time) < timeout:
            try:
                remaining_timeout = timeout - (time.time() - start_time)
                if remaining_timeout <= 0:
                    break
                    
                msg_type, payload = self.receive_packet(timeout=remaining_timeout)
                
                # MSG_TEXT уже выводится в receive_packet
                if msg_type == MSG_TEXT:
                    continue  # Пропускаем логи, идём дальше
                
                print(f"DEBUG: msg_type = 0x{msg_type:02X}, payload size = {len(payload)} bytes")
                
                if msg_type == MSG_ADC_DATA:
                    # Парсим данные (весь буфер одним пакетом)
                    samples = np.frombuffer(payload, dtype=np.int16)
                    print(f"✓ Получено {len(samples)} сэмплов")
                    print(f"  Первые 10 значений: {samples[:10]}")
                    print(f"  Min={samples.min()}, Max={samples.max()}, Mean={samples.mean():.1f}")
                    return samples
                    
                elif msg_type == MSG_ERROR:
                    print(f"✗ Ошибка: {payload.decode()}")
                    return None
                else:
                    print(f"✗ Неожиданный тип сообщения: 0x{msg_type:02X}")
                    
            except TimeoutError:
                break
        
        print("✗ Таймаут: ADC данные не получены")
        return None
    
    def get_status(self):
        """Получить статус устройства (включая имя текущего пресета и gain)"""
        self.send_command(CMD_GET_STATUS)
        msg_type, payload = self.receive_packet()
        
        if msg_type == MSG_STATUS:
            # Парсим статус: [struct 11 bytes] + [preset_name string]
            if len(payload) < 11:
                print(f"✗ Неверная длина статуса: {len(payload)}")
                return None
            
            # Распаковываем фиксированную часть (11 байт)
            # uint32_t adc_samples, uint16_t adc_rate, float gain, uint8_t error_flags
            adc_samples, adc_rate, gain, error_flags = struct.unpack('<IHfB', payload[:11])
            
            # Читаем имя пресета (остаток пакета)
            preset_name = ""
            if len(payload) > 11:
                preset_name = payload[11:].decode('utf-8', errors='ignore')
            
            status = {
                'adc_samples': adc_samples,
                'adc_rate': adc_rate,
                'gain': gain,
                'error_flags': error_flags,
                'preset_name': preset_name
            }
            return status
        elif msg_type == MSG_ERROR:
            print(f"✗ Ошибка: {payload.decode()}")
            return None
    
    def set_gain(self, gain):
        """Установить коэффициент усиления (gain)"""
        if gain < 0.0:
            print(f"✗ Неверный gain: {gain} (должен быть ≥ 0.0)")
            return False
        
        payload = struct.pack('<f', gain)
        self.send_command(CMD_SET_GAIN, payload)
        msg_type, payload = self.receive_packet()
        
        if msg_type == MSG_ACK:
            print(f"✓ Gain установлен: {gain:.2f}")
            return True
        elif msg_type == MSG_ERROR:
            print(f"✗ Ошибка: {payload.decode()}")
            return False
    
    def get_gain(self):
        """Получить текущий gain"""
        self.send_command(CMD_GET_GAIN)
        msg_type, payload = self.receive_packet()
        
        if msg_type == MSG_ACK and len(payload) == 4:
            gain = struct.unpack('<f', payload)[0]
            print(f"✓ Текущий gain: {gain:.2f}")
            return gain
        elif msg_type == MSG_ERROR:
            print(f"✗ Ошибка: {payload.decode()}")
            return None
    
    def upload_dac_buffer(self, samples_mono, preset_name="Custom preset"):
        """
        Загрузить новый DAC буфер (пресет)
        
        Args:
            samples_mono: numpy array int16, МОНО (16000 samples)
            preset_name: str, имя пресета (например, "tACS 640Hz 1mA")
        
        Формат: [MONO buffer 32KB] + [preset_name string]
        """
        if len(samples_mono) != 16000:
            print(f"✗ Неверная длина буфера: {len(samples_mono)} (ожидается 16000)")
            return False
        
        # Формируем payload: МОНО буфер + имя
        buffer_bytes = samples_mono.astype(np.int16).tobytes()
        name_bytes = preset_name.encode('utf-8')
        payload = buffer_bytes + name_bytes
        
        print(f"Загрузка пресета '{preset_name}' ({len(samples_mono)} MONO samples, {len(payload)} bytes)...")
        self.send_command(CMD_SET_DAC, payload)
        
        msg_type, response = self.receive_packet(timeout=3.0)
        if msg_type == MSG_ACK:
            print(f"✓ Пресет загружен!")
            return True
        elif msg_type == MSG_ERROR:
            print(f"✗ Ошибка: {response.decode()}")
            return False
    
    def listen_messages(self, duration=5.0):
        """Слушать текстовые сообщения от устройства"""
        print(f"Слушаем сообщения {duration} секунд...")
        start_time = time.time()
        
        while (time.time() - start_time) < duration:
            try:
                msg_type, payload = self.receive_packet(timeout=0.5)
                
                if msg_type == MSG_TEXT:
                    print(f"[LOG] {payload.decode()}")
                elif msg_type == MSG_ERROR:
                    print(f"[ERROR] {payload.decode()}")
                elif msg_type == MSG_STATUS:
                    print("[STATUS] Получен статус")
                    
            except TimeoutError:
                continue
            except Exception as e:
                print(f"Ошибка: {e}")


# === ПРИМЕРЫ ИСПОЛЬЗОВАНИЯ ===

def example_basic():
    """Базовый пример"""
    dev = tRNSDevice('/dev/ttyACM0')
    
    # Получить статус
    print("\n=== Статус устройства ===")
    status = dev.get_status()
    if status:
        for key, value in status.items():
            print(f"  {key}: {value}")
        print(f"\nТекущий пресет: '{status['preset_name']}'")
        print(f"Gain: {status['gain']}")
    
    # Управление gain (коэффициент усиления)
    print("\n=== Управление gain ===")
    # dev.set_gain(3.5)  # Увеличить амплитуду в 1.5 раза
    # time.sleep(0.1)
    # gain = dev.get_gain()
    
    # dev.set_gain(1.5)  # Уменьшить амплитуду в 2 раза
    # time.sleep(0.1)
    # gain = dev.get_gain()
    
    # dev.set_gain(1.0)  # Вернуть к исходной
    # time.sleep(0.1)
    
    # Получить ADC данные
    print("\n=== ADC данные ===")
    adc_data = dev.get_adc_data()
    if adc_data is not None:
        print(f"Среднее: {np.mean(adc_data):.1f}")
        print(f"Min: {np.min(adc_data)}, Max: {np.max(adc_data)}")
        print(f"Напряжение (0-1.1V): {np.mean(adc_data)/4095*1.1:.3f}V")


def example_generate_trns():
    """Пример генерации и загрузки tRNS сигнала"""
    dev = tRNSDevice('/dev/ttyACM0')
    
    # Генерируем tRNS (гауссовский шум 100-640 Hz)
    sample_rate = 8000
    duration = 2.0  # 2 секунды
    n_samples = int(sample_rate * duration)
    
    # Белый шум
    noise = np.random.randn(n_samples)
    
    # Фильтр 100-640 Hz (грубая реализация через FFT)
    fft = np.fft.rfft(noise)
    freqs = np.fft.rfftfreq(n_samples, 1/sample_rate)
    fft[(freqs < 100) | (freqs > 640)] = 0
    noise_filtered = np.fft.irfft(fft, n_samples)
    
    # Нормализация
    noise_filtered /= np.max(np.abs(noise_filtered))
    noise_filtered *= 0.8  # 80% амплитуды
    
    # Конвертируем в int16 МОНО (только правый канал - сигнал!)
    # Левый канал = константа (-0.5V), формируется на ESP32
    signal_mono = np.int16(noise_filtered * 32767 * (1.0/3.1))
    
    # Загружаем в устройство
    print("\n=== Загрузка tRNS пресета ===")
    dev.upload_dac_buffer(signal_mono, "tRNS 100-640Hz custom")
    
    # Проверяем статус
    time.sleep(0.2)
    status = dev.get_status()
    if status:
        print(f"Активный пресет: '{status['preset_name']}'")


def example_generate_tacs():
    """Пример генерации и загрузки tACS сигнала (синус)"""
    dev = tRNSDevice('/dev/ttyACM0')
    
    # Генерируем tACS (синус 10 Гц)
    sample_rate = 8000
    duration = 2.0  # 2 секунды
    n_samples = int(sample_rate * duration)
    freq = 10  # 10 Гц
    
    # Синус
    t = np.arange(n_samples) / sample_rate
    sine = np.sin(2 * np.pi * freq * t)
    
    # Конвертируем в int16 МОНО
    signal_mono = np.int16(sine * 32767 * (1.0/3.1))
    
    # Загружаем в устройство
    print("\n=== Загрузка tACS пресета ===")
    dev.upload_dac_buffer(signal_mono, f"tACS {freq}Hz 1mA")
    
    # Проверяем статус
    time.sleep(0.2)
    status = dev.get_status()
    if status:
        print(f"Активный пресет: '{status['preset_name']}'")


if __name__ == '__main__':
    print("=== tRNS/tACS Device Test ===\n")
    
    try:
        example_basic()
        # example_generate_tacs()   # Загрузить синус 10 Гц
        # example_generate_trns()   # Загрузить шум 100-640 Гц
        
    except serial.SerialException as e:
        print(f"Ошибка порта: {e}")
        print("Проверь что устройство подключено!")
    except KeyboardInterrupt:
        print("\nПрервано пользователем")

