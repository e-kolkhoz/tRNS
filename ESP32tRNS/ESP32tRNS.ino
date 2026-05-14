// ============================================================================
// === ESP32-S3FH4R2 ===
// ============================================================================
//
// Цикл загрузки:
//   1. CustomPresets::checkAll() — FFat, список валидных WAV-пресетов.
//   2. USBFlash::mount() — USB MSC, раздел как флешка на ПК.
//   3. После успешного mount — I2C OLED + энкодер.
//
// Neopixel:
//   синий пульс — жив
//   красный     — MSC не поднялся
//   жёлтый      — MSC ок, валидных пресетов нет
//   сиреневый   — MSC ок + есть хотя бы один валидный пресет

#include "config.h"
#include "version.h"
#include "custom_presets.h"
#include "usb_flash.h"

#include <Wire.h>
#include <U8g2lib.h>
#include <EncButton.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "boot_control.h"

static std::vector<PresetInfo> g_presets;

#if OLED_IS_SH1106
static U8G2_SH1106_128X64_NONAME_F_HW_I2C  oled(U8G2_R0, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif

static EncButton enc(ENC_A, ENC_B, ENC_S);

// ============================================================
// === МЕНЮ ===
// ============================================================
// Пункты: 0-wavs  1-usb  2-charging  3-bat_V  4-go_sleep  5-uf2
static const int8_t MENU_N        = 6;
static const int8_t MENU_GO_SLEEP = 4;
static const int8_t MENU_UF2      = 5;

static int8_t menu_idx = 0;

// --- Напряжение батареи: делитель R_top=100k / R_bot=360k ---
// V_bat = V_adc * (R_top + R_bot) / R_bot = V_adc * 460/360
// V_adc = raw * 3.3 / 4095
static float read_bat_v() {
  return analogRead(PLUS_BAT_ADC) * (3.3f / 4095.0f) * (460.0f / 360.0f);
}

// --- Глубокий сон, пробуждение по нажатию ENC_S ---
static void go_sleep() {
  digitalWrite(EN_WAKEUP, LOW);            // выключаем аналоговые модули
  rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);     // гасим неопиксель
  oled.setPowerSave(1);                    // гасим экран
  // Ждём пока кнопка точно отпущена
  while (digitalRead(ENC_S) == LOW) { delay(10); }
  delay(200);  // дебаунс отпускания

  // RTC pull-up: обычный INPUT_PULLUP отключается во сне → пин плавал в LOW
  rtc_gpio_pullup_en((gpio_num_t)ENC_S);
  rtc_gpio_pulldown_dis((gpio_num_t)ENC_S);
  // ESP32-S3: ext1 (ANY_LOW = любой из пинов маски пошёл в LOW)
  esp_sleep_enable_ext1_wakeup(1ULL << ENC_S, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
  // после пробуждения ESP32 перезагружается, выполняет setup() заново
}

// --- Отрисовка меню ---
static void draw_menu() {
  const int8_t MAX_VIS = 5;    // видимых пунктов на экране
  const int8_t ITEM_H  = 10;   // шаг строки, px
  const int8_t Y_START = 12;   // y первого пункта (под заголовком)

  // Строки пунктов
  char lines[MENU_N][22];
  snprintf(lines[0], sizeof(lines[0]), "wavs: %u",     (unsigned)g_presets.size());
  snprintf(lines[1], sizeof(lines[1]), "usb: %s",      digitalRead(USB_DET)   ? "on" : "off");
  snprintf(lines[2], sizeof(lines[2]), "charging: %s", digitalRead(VBUS_STAT) ? "on" : "off");
  snprintf(lines[3], sizeof(lines[3]), "bat_V: %.2f",  read_bat_v());
  snprintf(lines[4], sizeof(lines[4]), "go_sleep");
  snprintf(lines[5], sizeof(lines[5]), "uf2");

  // Смещение прокрутки
  int8_t scroll = 0;
  if (menu_idx >= MAX_VIS) {
    scroll = menu_idx - MAX_VIS + 1;
  }
  if (scroll > MENU_N - MAX_VIS) {
    scroll = MENU_N - MAX_VIS;
  }

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);

  // Заголовок
  oled.drawStr(0, 0, "== tRNS ==");

  // Пункты
  int8_t vis = (MENU_N < MAX_VIS) ? MENU_N : MAX_VIS;
  for (int8_t i = 0; i < vis; i++) {
    int8_t idx = scroll + i;
    int8_t y   = Y_START + i * ITEM_H;
    if (idx == menu_idx) {
      oled.drawStr(0, y, ">");
    }
    oled.drawStr(10, y, lines[idx]);
  }

  // Индикаторы прокрутки справа (треугольники)
  if (MENU_N > MAX_VIS) {
    if (scroll > 0) {
      oled.drawTriangle(124, 14, 120, 18, 128, 18);  // ▲
    }
    if (scroll < MENU_N - MAX_VIS) {
      oled.drawTriangle(124, 62, 120, 58, 128, 58);  // ▼
    }
  }

  oled.sendBuffer();
}

// ============================================================
static void neo_restore_idle() {
  if (g_presets.empty()) {
    rgbLedWrite(NEOPIXEL_PIN, 40, 25, 0);
  } else {
    rgbLedWrite(NEOPIXEL_PIN, 30, 0, 30);
  }
}

void IRAM_ATTR enc_isr() {
  enc.tickISR();
}

static void init_enc_oled() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(OLED_I2C_HZ);
  oled.setI2CAddress(DISPLAY_ADDR << 1);
  oled.begin();
  oled.enableUTF8Print();
  oled.setFontPosTop();
  oled.setContrast(255);
  oled.setPowerSave(0);

  enc.setEncType(EB_STEP4_LOW);
  enc.setDebTimeout(30);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_S, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_A), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), enc_isr, CHANGE);

  draw_menu();
}

// ============================================================
void setup() {
  Serial.begin(115200);
  BootControl::init();   // снять GPIO hold если остался с прошлой сессии UF2

  // Аналоговые модули включаем сразу после старта
  pinMode(EN_WAKEUP, OUTPUT);
  digitalWrite(EN_WAKEUP, HIGH);

  pinMode(USB_DET,   INPUT);
  pinMode(VBUS_STAT, INPUT_PULLDOWN);

  rgbLedWrite(NEOPIXEL_PIN, 0, 0, 40);
  delay(300);
  rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);

  delay(1500);

  g_presets = CustomPresets::checkAll(CustomPresets::DEFAULT_RATE);

  if (!USBFlash::mount()) {
    rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);
    return;
  }

  if (!g_presets.empty()) {
    rgbLedWrite(NEOPIXEL_PIN, 30, 0, 30);
  } else {
    rgbLedWrite(NEOPIXEL_PIN, 40, 25, 0);
  }

  init_enc_oled();
}

void loop() {
  if (!USBFlash::isMounted()) {
    return;
  }

  enc.tick();

  bool ch = false;

  if (enc.right()) {
    menu_idx = constrain(menu_idx + 1, 0, MENU_N - 1);
    ch = true;
    rgbLedWrite(NEOPIXEL_PIN, 35, 0, 20);
  }
  if (enc.left()) {
    menu_idx = constrain(menu_idx - 1, 0, MENU_N - 1);
    ch = true;
    rgbLedWrite(NEOPIXEL_PIN, 0, 30, 35);
  }
  if (enc.click()) {
    if (menu_idx == MENU_GO_SLEEP) {
      go_sleep();       // уходим в спячку, пробуждение по ENC_S
    } else if (menu_idx == MENU_UF2) {
      BootControl::rebootToUF2();   // перезагрузка в UF2 bootloader
    }
    ch = true;
    rgbLedWrite(NEOPIXEL_PIN, 40, 40, 15);
  }

  // Перерисовываем при изменении или раз в 500 мс (обновление bat_V)
  static uint32_t t_draw;
  if (ch || millis() - t_draw > 500) {
    t_draw = millis();
    draw_menu();
  }

  static uint32_t neo_idle;
  if (ch) {
    neo_idle = millis() + 120;
  } else if (neo_idle && (int32_t)(millis() - neo_idle) >= 0) {
    neo_idle = 0;
    neo_restore_idle();
  }
}
