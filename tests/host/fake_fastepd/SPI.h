/* Host stand-in for targets/esp32-idf/vendor/fastepd/SPI.h: the names display_fastepd.cpp uses,
 * doing nothing. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

#define MSBFIRST  1
#define SPI_MODE0 0

class SPISettings {
public:
    SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class SPIClass {
public:
    void beginTransaction(const SPISettings &) {}
    void endTransaction() {}
    uint16_t transfer16(uint16_t) { return 0; }
    void writeBytes(const uint8_t *, size_t) {}
    void end() {}
};

extern SPIClass SPI;
