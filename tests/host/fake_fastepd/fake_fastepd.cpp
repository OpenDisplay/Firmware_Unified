/* Host stand-in for the FastEPD library methods display_fastepd.cpp calls. The real header
 * (third_party/FastEPD/src/FastEPD.h) is used as is; only these bodies are fake.
 *
 * initPanel keeps the library's allocation rule: pCurrent is native_width * native_height / 2
 * bytes (the 4-bpp size), pPrevious points (w/4)*h bytes into it, and a second initPanel
 * drops the old pointer and allocates again. Allocation goes through the fake heap, so a
 * write past the buffer shows up in fake_esp32_guards_intact(). */
#include <FastEPD.h>
#include <SPI.h>

#include "esp_heap_caps.h"
#include "fake_fastepd.h"

SPIClass SPI;

int fake_fastepd_init_panel_calls;
int fake_fastepd_init_rc = BBEP_SUCCESS;
int fake_fastepd_updates;

static void panel_size(int panel, int *w, int *h)
{
    switch (panel) {
    case BB_PANEL_M5PAPERS3:   *w = 960;  *h = 540; break;
    case BB_PANEL_INKPLATE5V2: *w = 1280; *h = 720; break;
    case BB_PANEL_INKPLATE10:  *w = 1200; *h = 825; break;
    default:                   *w = 0;    *h = 0;   break;
    }
}

int FASTEPD::initPanel(int iPanelType, uint32_t)
{
    int w;
    int h;

    ++fake_fastepd_init_panel_calls;
    if (fake_fastepd_init_rc != BBEP_SUCCESS) {
        return fake_fastepd_init_rc;
    }
    panel_size(iPanelType, &w, &h);
    if (w == 0) {
        return BBEP_ERROR_BAD_PARAMETER;
    }
    _state.iPanelType = iPanelType;
    _state.mode = BB_MODE_1BPP;
    _state.pCurrent = NULL;
    _state.width = _state.native_width = w;
    _state.height = _state.native_height = h;
    _state.pCurrent = (uint8_t *)heap_caps_aligned_alloc(16, (size_t)w * h / 2, MALLOC_CAP_SPIRAM);
    if (_state.pCurrent == NULL) {
        return BBEP_ERROR_NO_MEMORY;
    }
    _state.pPrevious = &_state.pCurrent[(w / 4) * h];
    return BBEP_SUCCESS;
}

int FASTEPD::initIT8951(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t)
{
    return BBEP_ERROR_NOT_SUPPORTED;
}

int FASTEPD::setPanelSize(int) { return BBEP_ERROR_NOT_SUPPORTED; }
int FASTEPD::setMode(int iMode) { _state.mode = (uint8_t)iMode; return BBEP_SUCCESS; }
int FASTEPD::einkPower(int bOn) { _state.pwr_on = (uint8_t)bOn; return BBEP_SUCCESS; }
int FASTEPD::fullUpdate(int, bool, BB_RECT *) { ++fake_fastepd_updates; return BBEP_SUCCESS; }
int FASTEPD::fastUpdate(bool) { ++fake_fastepd_updates; return BBEP_SUCCESS; }
void FASTEPD::backupPlane(void) {}

void it8951WaitForReady(FASTEPDSTATE *) {}
void it8951WaitForLUTReady(FASTEPDSTATE *) {}
void it8951SetImgBufBaseAddr(FASTEPDSTATE *) {}
void it8951LoadImgAreaStart(FASTEPDSTATE *, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t,
                            uint16_t, uint16_t) {}
void it8951WriteCmdCode(FASTEPDSTATE *, uint16_t) {}
void it8951DisplayArea1Bit(FASTEPDSTATE *, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t,
                           uint8_t, uint8_t) {}
void bbepPCALDigitalWrite(uint8_t, uint8_t) {}
