#include <driver/i2s.h>
#include <math.h>
#include "esp_timer.h"

// === CONFIG ===
#define SAMPLE_RATE         8000
#define I2S_NUM             I2S_NUM_0
#define SINE_FREQ           640
#define I2S_BUFFER_SAMPLES  256

// --- I2S Pins ---
#define I2S_BCLK    12
#define I2S_WCLK    16
#define I2S_DOUT    18

// --- Globals ---
int16_t i2s_buffer[I2S_BUFFER_SAMPLES * 2];
int sine_wave_step = 0;

// === SETUP ===
void setup() {
  Serial.begin(921600);
  Serial.println("Booting...");

  // --- I2S CONFIG ---
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
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

  i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM, &pin_config);
  i2s_set_clk(I2S_NUM, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

  Serial.println("I2S Initialized.");
}

// === LOOP ===
void loop() {
  const int samples_per_cycle = SAMPLE_RATE / SINE_FREQ;
  const int16_t max_val = 32767;
  const float max_volt = 3.1;

  float ampl_volts = 1.0;
  float LEFT_CHANNEL_VOLTS = -0.5;

  int16_t LEFT_CHANNEL_VAL = (int16_t)(max_val * LEFT_CHANNEL_VOLTS / max_volt);
  int16_t ampl_val         = (int16_t)(max_val * ampl_volts / max_volt);

  // --- generate sine buffer ---
  for (int i = 0; i < I2S_BUFFER_SAMPLES; i++) {
    float t = (float)sine_wave_step / samples_per_cycle;
    int16_t right_value = (int16_t)(sinf(2.0f * M_PI * t) * ampl_val);

    i2s_buffer[i * 2]     = right_value;
    i2s_buffer[i * 2 + 1] = LEFT_CHANNEL_VAL;

    sine_wave_step = (sine_wave_step + 1) % samples_per_cycle;
  }

  // --- write to I2S with tiny timeout ---
  size_t bytes_written = 0;
  i2s_write(I2S_NUM, i2s_buffer, sizeof(i2s_buffer), &bytes_written, 10);

}
