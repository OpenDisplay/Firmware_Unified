# One-shot nRF52840 migration app: erases the s140 SoftDevice (0x1000..0x27000) so the Adafruit
# bootloader starts the application at 0x1000, which is what the 3.x dual-slot MCUboot layout
# needs. Built for the stock xiao_ble board (Adafruit layout, app at 0x27000) with no MCUboot,
# and shipped as release asset opendisplay-xiao_nrf52840-usb-1-sd-erase.zip by
# tools/release/nrf52840_usb_packages.sh. src/main.c says what it touches and why it is safe
# to interrupt. docs/UPGRADING.md § nRF52840 records what was run on hardware.
#
# Build: west build -b xiao_ble/nrf52840 -d <dir> tools/nrf52840_sd_erase -- \
#          -DSB_CONF_FILE=<abs path>/tools/nrf52840_sd_erase/sysbuild.conf
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sd_erase)
target_sources(app PRIVATE src/main.c)
