# Classic ESP32 (WROVER-E), 4 MB flash, 8 MB PSRAM, FastEPD. From platformio.ini
# env:esp32-wrover-e-N4R8: the build for Soldered Inkplate 5 V2 / 10 (FastEPD native parallel)
# and the original M5Stack M5Paper (IT8951 over SPI).
#
# Like esp32-n4 there is no WiFi/LAN transport, and the 16-frame PIPE window applies: PSRAM
# does not enlarge the static DRAM that the reorder queue and TX/RX rings live in.
set(OD_IDF_TARGET esp32)
set(OD_PARTITION_CSV partitions/min_spiffs_4MB.csv)
set(OD_SDKCONFIG_FRAGMENTS sdkconfig.defaults.esp32 boards/esp32-wrover-e-n4r8.sdkconfig)

set(OD_BOARD_DEFINES
    BOARD_HAS_PSRAM
    OPENDISPLAY_FASTEPD
    OD_PIPE_MAX_W=16u
    OD_PIPE_MAX_N=16u
    OD_TXQ_SLOTS=18u
    OD_RXQ_SLOTS=18u
)
