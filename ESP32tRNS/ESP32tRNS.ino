// ============================================================================
// === ESP32-S3FH4R2 / Waveshare ESP32-S3-Zero ===
// ============================================================================

#include "config.h"
#include "version.h"
#include "custom_presets.h"
#include "oled.h"
#include "tone_gen.h"
#include "usb_flash.h"
#include <esp_err.h>

static std::vector<PresetInfo> g_presets;

// Один рабочий тестовый режим:
// Fpcm=96k, fp/fs=960/480 (ratio=2), bclk_div=8.
static const ToneGen::Config TEST_CFG = {
    96000, I2S_CLK_SRC_PLL_240M, 960, 480, 8
};

static const char* srcName(soc_periph_i2s_clk_src_t s) {
    if (s == I2S_CLK_SRC_PLL_240M) return "240";
    if (s == I2S_CLK_SRC_XTAL)     return "XTL";
    return "160";
}

// Короткое имя ошибки для OLED (≤4 символа), либо hex.
static const char* errShort(esp_err_t err) {
    static char hex[8];
    switch (err) {
        case ESP_OK:                   return "OK";
        case ESP_FAIL:                 return "FAIL";
        case ESP_ERR_NO_MEM:           return "NMEM";
        case ESP_ERR_INVALID_ARG:      return "IARG";
        case ESP_ERR_INVALID_STATE:    return "IST";
        case ESP_ERR_INVALID_SIZE:     return "ISIZ";
        case ESP_ERR_NOT_FOUND:        return "NFND";
        case ESP_ERR_NOT_SUPPORTED:    return "NSUP";
        case ESP_ERR_TIMEOUT:          return "TOUT";
        default:
            snprintf(hex, sizeof(hex), "0x%x", (unsigned)err);
            return hex;
    }
}

void setup() {
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 40);
    delay(300);
    rgbLedWrite(NEOPIXEL_PIN, 0, 0, 0);
    delay(1500);

    OLED::begin();
    OLED::clear();
    OLED::printLine("v%s", FIRMWARE_VERSION);
    OLED::printLine("96k %s 960/480/8", srcName(TEST_CFG.clk_src));
    esp_err_t err = ToneGen::begin(TEST_CFG);
    OLED::printLine("PDM %s", errShort(err));

    if (err == ESP_OK) {
        // Оба канала активны в одном режиме 96k.
        ToneGen::setSineLoop(0, 640.0f, ToneGen::ampToGainQ15(1.0f));   // GPIO1
        ToneGen::setSineLoop(1, 640.0f, ToneGen::ampToGainQ15(1.0f));   // GPIO2
        rgbLedWrite(NEOPIXEL_PIN, 0, 30, 0);
    } else {
        rgbLedWrite(NEOPIXEL_PIN, 40, 0, 0);
    }

    g_presets = CustomPresets::checkAll(CustomPresets::DEFAULT_RATE);
    USBFlash::mount();
}

void loop() {
}
