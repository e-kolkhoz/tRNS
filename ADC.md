Обратная связь токовая, униполярная.
Стартовые DEF-калибровки для atten=11dB:
- offset L = 1.25 V
- offset R = 1.25 V
- коэффициент пересчета V->mA = 0.37
ADC_SENSE1 GPIO4 LEFT  (ADC1_CH3)
ADC_SENSE2 GPIO5 RIGHT (ADC1_CH4)

ESP32-S3 continuous DMA: ТОЛЬКО GPIO1-10 (ADC1).
GPIO11, GPIO12, … GPIO20 — ADC2, continuous на S3 не работает (errata).

нужно набивать в буфер i2s непрерывно, если позволяет S3, то с оверсемплингом, дизерингом и фильтрацией
