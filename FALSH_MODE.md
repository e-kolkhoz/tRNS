Как войти в UF2-режим (вручную):

На Waveshare ESP32-S3-Zero нет double-tap reset (PIN_DOUBLE_RESET_RC не задан). Механизм такой:
Нажать RST (сброс)
Когда RGB-светодиод загорится фиолетовым — нажать и держать BOOT (GPIO0)