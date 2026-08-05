#include "encoder_pcnt.h"
#include "config.h"

#include <driver/pulse_cnt.h>
#include <esp_err.h>

static pcnt_unit_handle_t s_unit = nullptr;
static bool s_reverse = false;
static int32_t s_acc = 0;

void EncoderPcnt::init(int pin_a, int pin_b, bool reverse) {
  s_reverse = reverse;
  s_acc = 0;

  pcnt_unit_config_t unit_config = {};
  unit_config.low_limit = -32768;
  unit_config.high_limit = 32767;

  ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &s_unit));

  pcnt_glitch_filter_config_t filter_config = {
    .max_glitch_ns = ENC_PCNT_GLITCH_NS,
  };
  if (pcnt_unit_set_glitch_filter(s_unit, &filter_config) != ESP_OK) {
    filter_config.max_glitch_ns = 12000;
    pcnt_unit_set_glitch_filter(s_unit, &filter_config);
  }

  pcnt_chan_config_t chan_config = {
    .edge_gpio_num = pin_a,
    .level_gpio_num = pin_b,
  };
  pcnt_channel_handle_t chan = nullptr;
  ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_config, &chan));

  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(
      chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(
      chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

  ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
  ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
  ESP_ERROR_CHECK(pcnt_unit_start(s_unit));
}

void EncoderPcnt::setReverse(bool reverse) {
  s_reverse = reverse;
}

int32_t EncoderPcnt::readDelta() {
  if (!s_unit) return 0;

  int raw = 0;
  if (pcnt_unit_get_count(s_unit, &raw) != ESP_OK || raw == 0) return 0;

  pcnt_unit_clear_count(s_unit);

  int32_t delta = raw;
  if (s_reverse) delta = -delta;

#if ENC_PCNT_DIV > 1
  s_acc += delta;
  const int32_t steps = s_acc / ENC_PCNT_DIV;
  s_acc -= steps * ENC_PCNT_DIV;
  return steps;
#else
  return delta;
#endif
}
