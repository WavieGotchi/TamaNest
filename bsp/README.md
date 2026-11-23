# Board support (Waveshare ESP32-S3-Touch-AMOLED-2.16)

Copy of Waveshare's BSP component `waveshare/esp32_s3_touch_amoled_2_16` 2.0.1 (Apache-2.0, see its LICENSE),
used instead of the downloaded one (`override_path` in main/idf_component.yml) because TamaNest changes it:

- `bsp_display_get_io()` added (panel IO handle).
- The microphone (I2S RX) channel is not enabled: its power lock kept the chip out of light sleep.
- LVGL draw band 16 rows instead of 50 (each menu screen flush needs a 15 KB internal DMA block, not 48 KB).
- `bsp_sdcard_init_raw()` added: starts the SD card without its file system (USB DRIVE can then show a card TamaNest
  cannot read, e.g. exFAT, so it can be reformatted on a computer).
