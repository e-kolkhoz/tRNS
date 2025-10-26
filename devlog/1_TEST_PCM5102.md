lОтлично, давай начнём с самого простого и системно. Вот как пойти по шагам.

---

## ✅ Шаг 1: Определить, что за плата ESP32

Скорее всего, у тебя **ESP-WROOM-32 DevKit v1** — самая распространённая. Опознать её можно по этим признакам:

| Характеристика    | Как проверить                                    |
| ----------------- | ------------------------------------------------ |
| Надпись на модуле | Должно быть `ESP-WROOM-32`                       |
| Кол-во ног        | 2 ряда по 19 пинов (всего 38)                    |
| USB на борту      | MicroUSB на USB-UART чипе (CP2102, CH340 и т.п.) |
| Питание           | 5V через USB, логика на 3.3V                     |

Если хочешь — скинь фото, я точно скажу, какая у тебя плата.

---

## ✅ Шаг 2: Установить всё для программирования

Тебе нужно:

1. **Arduino IDE (рекомендуется 2.x)**: [https://www.arduino.cc/en/software](https://www.arduino.cc/en/software)
2. Установить поддержку ESP32:

   * Зайди в «Файл» → «Настройки» → и добавь в поле "Дополнительные ссылки для менеджера плат":

     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   * Потом открой «Инструменты» → «Плата» → «Менеджер плат» → найди **ESP32 by Espressif Systems** и установи.
3. Выбери плату: **ESP32 Dev Module**

---

## ✅ Шаг 3: Подключить PCM5102A

PCM5102A подключается по **I2S** (это не I2C!):

| ESP32             | PCM5102A Module          |
| ----------------- | ------------------------ |
| GPIO25 (DAC\_OUT) | DIN (Data in)            |
| GPIO26            | BCK (Bit clock)          |
| GPIO22            | LRCK (Word select / LRC) |
| GND               | GND                      |
| 3.3V / 5V         | VCC                      |





## ✅ Шаг 4: Синус на выход

Вот минимальный скетч, который выдаёт **1 Гц синус с частотой дискретизации 8 кГц**:

```cpp
#include <driver/i2s.h>
#include <math.h>

#define SAMPLE_RATE 8000
#define I2S_NUM     I2S_NUM_0
#define SINE_FREQ   1    // Частота синусоиды в Гц

// Настройка пинов для PCM5102A
#define I2S_BCLK    26   // BCK пин
#define I2S_WCLK    22   // LCK пин (WS)
#define I2S_DOUT    25   // DIN пин на PCM5102A

void setup() {
  Serial.begin(115200);
  Serial.println("Инициализация генератора синуса 1 Гц для PCM5102A...");

  // Настройка I2S для работы с PCM5102A
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // Стерео формат
    // PCM5102A работает с Phillips I2S стандартом (старший бит первым)
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_WCLK,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  // Установка I2S драйвера
  esp_err_t result = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  if (result != ESP_OK) {
    Serial.println("Ошибка установки I2S драйвера: " + String(result));
    while(1);
  }
  
  result = i2s_set_pin(I2S_NUM, &pin_config);
  if (result != ESP_OK) {
    Serial.println("Ошибка конфигурации пинов I2S: " + String(result));
    while(1);
  }

  Serial.println("I2S инициализирован успешно");
  Serial.println("Генерация синуса частотой 1 Гц начата");
}

void loop() {
  // Количество отсчетов на один период синуса
  const int samples_per_cycle = SAMPLE_RATE / SINE_FREQ;
  
  // Буфер для отправки в PCM5102A
  // Для ESP32 нужно использовать DMA-совместимую память
  int16_t* samples = (int16_t*)malloc(2 * samples_per_cycle * sizeof(int16_t));
  
  if (samples == NULL) {
    Serial.println("Ошибка выделения памяти");
    delay(1000);
    return;
  }
  
  // Заполняем оба канала синусоидой
  for (int i = 0; i < samples_per_cycle; i++) {
    float t = (float)i / samples_per_cycle;
    int16_t value = (int16_t)(sinf(2.0 * PI * t) * 32767);
    
    // Заполняем левый и правый каналы (чередуются)
    samples[i*2] = value;     // Левый канал
    samples[i*2+1] = value;   // Правый канал
  }

  // Отправляем данные на ЦАП
  size_t bytes_written = 0;
  esp_err_t result = i2s_write(I2S_NUM, samples, 2 * samples_per_cycle * sizeof(int16_t), 
                               &bytes_written, portMAX_DELAY);
                               
  if (result != ESP_OK) {
    Serial.println("Ошибка записи в I2S: " + String(result));
  }
  
  free(samples);
  
  // Синусоида длительностью в 1 секунду уже отправлена,
  // просто небольшая пауза перед повтором
  delay(10);
}

```

---

## ✅ Итого

Ты получаешь:

* Чистый 1 Гц синус на **DIN PCM5102A**
* С дискретизацией **8 кГц**
* Амплитуда — 16 бит, ±32767 → выдаёт **биполярный аналоговый выход**

---

Хочешь — могу помочь построить генератор шумов, псевдослучайных импульсов, или амплитудно-модулированных синусов — уже на основе этого кода.
