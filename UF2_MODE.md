Как войти в UF2-режим (вручную):

На Waveshare ESP32-S3-Zero нет double-tap reset (PIN_DOUBLE_RESET_RC не задан). Механизм такой:
Нажать RST (сброс)
Когда RGB-светодиод загорится фиолетовым — нажать и держать BOOT (GPIO0)


Хорошие новости — Lolin S3 Mini полностью совместим по разметке:
Waveshare S3 Zero	Lolin S3 Mini
Чип	ESP32-S3FH4R2	ESP32-S3FH4R2
Partition scheme	partitions-4MB-noota.csv	partitions-4MB-noota.csv
PIN_BUTTON_UF2	GPIO0	GPIO0
Neopixel pin	GPIO21	GPIO47
Volume label	WS3ZEROBOOT	LOLIN3MBOOT
USB VID/PID	303a:81B3	303a:8169