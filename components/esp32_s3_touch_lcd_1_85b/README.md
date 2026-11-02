# BSP: Waveshare ESP32-S3-Touch-LCD-1.85B (vendored)

Board support package for the Waveshare ESP32-S3-Touch-LCD-1.85B. It covers the ST77916 360×360 QSPI LCD,
the CST816S touch controller, the PWM backlight, I2C, I2S audio (ES8311 and ES7210), the microSD card (SDMMC),
SPIFFS, and the QMI8658 IMU.

Copied from
<https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B/tree/main/Examples/ESP-IDF-V5.5.3/02_lvgl_demo/components/waveshare__esp32_s3_touch_lcd_1_85B>.
License: Apache-2.0 (see LICENSE). Waveshare does not publish this BSP to the Espressif component registry.

Changed for this project (each change is commented in the source):
- `bsp_display_lcd_init`: one 40-row LVGL draw band in internal RAM (`BSP_LCD_DRAW_BUF_HEIGHT` in
  `include/bsp/display.h`), sent in one SPI transaction, instead of double PSRAM buffers that the SPI driver
  had to copy chunk by chunk.
- `bsp_audio_init`: the I2S TX (speaker) channel only. The RX (microphone) channel cost about 6 KB of internal
  DMA memory, so the microphone can't be used.
