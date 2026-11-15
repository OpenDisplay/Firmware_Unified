# Seeed reTerminal Sticky -- ESP32-S3R8 (8 MB OPI PSRAM) + W25Q256 32 MB QSPI flash.
# From platformio.ini env:esp32-s3-N32R8-extuart.
#
# Two differences from s3-n32r8, both deliberate and both from the source env:
#
#   * NO OPENDISPLAY_FASTEPD. This board's panel is an SPI panel driven from the config's drive
#     packets -- env:esp32-s3-N32R8-extuart does not list the FastEPD lib_dep either.
#   * Console on the onboard CH343P USB-UART (GPIO43/44), not native USB-CDC.
set(OD_IDF_TARGET esp32s3)
set(OD_PARTITION_CSV partitions/default_32MB.csv)
set(OD_SDKCONFIG_FRAGMENTS
    sdkconfig.defaults.esp32s3
    boards/s3-n32r8.sdkconfig
    boards/_extuart.sdkconfig)

set(OD_BOARD_DEFINES
    OPENDISPLAY_ENABLE_WIFI
    BOARD_HAS_PSRAM
    OPENDISPLAY_LOG_UART
    OPENDISPLAY_LOG_UART_RX=44
    OPENDISPLAY_LOG_UART_TX=43
)
