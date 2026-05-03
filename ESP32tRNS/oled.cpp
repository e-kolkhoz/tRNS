#include "oled.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <stdarg.h>

namespace {
constexpr int W = 128;
constexpr int H = 64;
Adafruit_SSD1306 s_d(W, H, &Wire, -1);
bool s_ready = false;
}

bool OLED::begin() {
    if (s_ready) return true;
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    Wire.setClock(400000);
    if (!s_d.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) return false;
    s_d.clearDisplay();
    s_d.setTextSize(1);
    s_d.setTextColor(SSD1306_WHITE);
    s_d.setCursor(0, 0);
    s_d.display();
    s_ready = true;
    return true;
}

bool OLED::isReady() { return s_ready; }

void OLED::clear() {
    if (!s_ready) return;
    s_d.clearDisplay();
    s_d.setCursor(0, 0);
    s_d.display();
}

void OLED::printLine(const char* fmt, ...) {
    if (!s_ready) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_d.println(buf);
    s_d.display();
}

void OLED::flush() {
    if (!s_ready) return;
    s_d.display();
}
