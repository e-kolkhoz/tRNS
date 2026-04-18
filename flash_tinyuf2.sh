#!/bin/bash
python -m esptool --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 ./tinyuf2/combined.bin