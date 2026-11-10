# ESP32-S3, 4 MB flash, 2 MB quad PSRAM (Waveshare ESP32-S3-Zero class). From platformio.ini
# env:esp32-s3-N4R2. WiFi/LAN on, like every PSRAM S3; no FastEPD (no parallel panel fits
# this module). Dual app slots on 4 MB use the same layout as the other 4 MB boards.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/min_spiffs_4MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32s3 boards/s3-n4r2.sdkconfig)

set(OD_BOARD_DEFINES
    OPENDISPLAY_ENABLE_WIFI
    BOARD_HAS_PSRAM
)
