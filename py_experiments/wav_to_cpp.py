#!/usr/bin/env python3
"""
Конвертер WAV файлов в C++ массивы для встраивания в прошивку ESP32
"""

import wave
import struct
import sys
import os

def wav_to_cpp_array(wav_path, var_name):
    """Конвертирует WAV в массив int16_t для C++"""
    with wave.open(wav_path, 'rb') as wav:
        n_channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        framerate = wav.getframerate()
        n_frames = wav.getnframes()
        
        print(f"// WAV: {os.path.basename(wav_path)}")
        print(f"// Channels: {n_channels}, Rate: {framerate}Hz, Frames: {n_frames}")
        
        if n_channels != 1:
            print(f"ERROR: Only mono WAV supported, got {n_channels} channels", file=sys.stderr)
            sys.exit(1)
        
        if sample_width != 2:
            print(f"ERROR: Only 16-bit WAV supported, got {sample_width*8}-bit", file=sys.stderr)
            sys.exit(1)
        
        raw_data = wav.readframes(n_frames)
        samples = struct.unpack(f'<{n_frames}h', raw_data)
        
        print(f"const int16_t {var_name}[] PROGMEM = {{")
        
        # Выводим по 12 сэмплов в строке для читабельности
        for i in range(0, len(samples), 12):
            chunk = samples[i:i+12]
            line = "  " + ", ".join(f"{s:6d}" for s in chunk)
            if i + 12 < len(samples):
                line += ","
            print(line)
        
        print("};")
        print(f"const size_t {var_name}_SIZE = {len(samples)};")
        print()
        
        return len(samples)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: wav_to_cpp.py <wav_file> [var_name]")
        print("Example: wav_to_cpp.py noise_100_640_8000Hz_16bit.wav PRESET_NOISE_100_640")
        sys.exit(1)
    
    wav_path = sys.argv[1]
    
    if len(sys.argv) >= 3:
        var_name = sys.argv[2]
    else:
        # Генерируем имя переменной из имени файла
        var_name = "PRESET_" + os.path.splitext(os.path.basename(wav_path))[0].upper().replace("-", "_")
    
    if not os.path.exists(wav_path):
        print(f"ERROR: File not found: {wav_path}", file=sys.stderr)
        sys.exit(1)
    
    wav_to_cpp_array(wav_path, var_name)

