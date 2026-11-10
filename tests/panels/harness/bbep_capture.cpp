/* Host capture of the bb_epaper command stream for every panel enum.
 *
 * Reproduces the call sequence of the 2.x firmware (Firmware src/display_service.cpp,
 * split_panel.cpp) for a single display at rotation 0, starting at bbepWakeUp (README D4):
 *
 *   full     wake, pInitFull, [align], addr window, plane writes, bbepRefresh(FULL), BUSY
 *   fast     same, bbepRefresh(FAST)                      (only when pInitFast exists)
 *   partial  wake, pInitPart, [align], window+PLANE_1 (old), window+PLANE_0 (new),
 *            partial_trigger_refresh(PARTIAL), BUSY      (mono panels with pInitPart)
 *   sleep    [SSD16xx discharge], bbepSleep(1)
 *
 * Firmware overlays reproduced here (they are 2.x behaviour, not bb_epaper's):
 *   epdAlignCustomPartialRamMode (EP397 / EP426 RAM windows after init),
 *   epdSsd16xxPowerOffDischarge (SSD16xx except EP426), partial_set_addr_window and
 *   partial_trigger_refresh (EP397 Y-decrement, EP426 X-decrement and custom partial LUT),
 *   the split-panel (dual controller) frame and power-off.
 *
 * Output: one block per panel, "P <enum> <w> <h> <chip> <flags> <xoff> <fast> <part>",
 * then "L <16 colour-lookup bytes>", then "F <flow>" ... events ... "E" per flow (event grammar: bbep_rec_io.inl).
 *
 * `bbep_capture rects` instead emits, for every panel with a partial flow, the partial flow for
 * three sub-rectangles ("F partial_r<k> <x> <y> <w> <h>"), as partial_prepare_panel_ram +
 * partial_set_addr_window do it: white fill of both planes, then the rect window per plane.
 * The generator diffs them to find which window bytes are geometry.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bb_epaper.h"
#include "bbep_rec_io.inl"
#include "bb_ep.inl"
#include "bb_ep_gfx.inl"

static BBEPDISP bbep;
/* Partial rectangle; the whole panel unless main() selects a sub-rect. */
static int g_rx, g_ry, g_rw = -1, g_rh = -1;

static int is_type(int t) { return bbep.type == t; }
static bool is397(void) { return is_type(EP397_800x480) || is_type(EP397_800x480_4GRAY); }
static bool is426(void) { return is_type(EP426_800x480) || is_type(EP426_800x480_4GRAY); }

static void setup_panel(int idx)
{
    memset(&bbep, 0, sizeof(bbep));
    bbepSetPanelType(&bbep, idx);
    bbepSetRotation(&bbep, 0);
    bbepInitIO(&bbep, REC_PIN_DC, REC_PIN_RST, REC_PIN_BUSY, REC_PIN_CS, 6, 7, 8000000);
    if (bbep.iFlags & BBEP_SPLIT_BUFFER) {
        bbepSetCS2(&bbep, REC_PIN_CS2);
    }
    bbep.is_awake = 1;   /* state after bbepInitIO's own init (README D4) */
    g_busy_idle_high = (bbep.chip_type == BBEP_CHIP_UC81xx) ? 1 : 0;
}

/* Firmware epdAlignCustomPartialRamMode() */
static void fw_align(void)
{
    uint8_t uc[4];
    if (is397()) {
        bbepCMD2(&bbep, SSD1608_DATA_MODE, 0x01);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXPOS);
        uc[0] = 0x00; uc[1] = 0x00; uc[2] = 0x1f; uc[3] = 0x03; bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYPOS);
        uc[0] = 0xdf; uc[1] = 0x01; uc[2] = 0x00; uc[3] = 0x00; bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXCOUNT);
        uc[0] = 0x00; uc[1] = 0x00; bbepWriteData(&bbep, uc, 2);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYCOUNT);
        uc[0] = 0x00; uc[1] = 0x00; bbepWriteData(&bbep, uc, 2);
    } else if (is426()) {
        bbepCMD2(&bbep, SSD1608_DATA_MODE, 0x02);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXPOS);
        uc[0] = 0x1f; uc[1] = 0x03; uc[2] = 0x00; uc[3] = 0x00; bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYPOS);
        uc[0] = 0x00; uc[1] = 0x00; uc[2] = 0xdf; uc[3] = 0x01; bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXCOUNT);
        uc[0] = 0x1f; uc[1] = 0x03; bbepWriteData(&bbep, uc, 2);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYCOUNT);
        uc[0] = 0x00; uc[1] = 0x00; bbepWriteData(&bbep, uc, 2);
    }
}

static void image(int n)
{
    static uint8_t buf[256];
    g_in_image = 1;
    bbepWriteData(&bbep, buf, n);   /* bytes are opaque; only the count is recorded */
    g_in_image = 0;
}

static int plane_bytes(void) { return ((bbep.native_width + 7) / 8) * bbep.native_height; }

/* Direct-write plane framing (directWriteActivatePanel + streamGray4Bytes). */
static void write_frame(void)
{
    const int w = bbep.native_width, h = bbep.native_height;
    if (bbep.iFlags & BBEP_SPLIT_BUFFER) {
        /* split_panel.cpp splitPanelBeginFrame / advanceToRightHalf / CloseFrame */
        const int half = (w / 4) * h;
        bbep.cs_mode = CMD_CS_NONE;
        digitalWrite(bbep.iCSPin, LOW); digitalWrite(bbep.iCS2Pin, HIGH);
        bbepWriteCmd(&bbep, UC8151_DTM1);
        image(half);
        digitalWrite(bbep.iCSPin, HIGH); digitalWrite(bbep.iCS2Pin, LOW);
        bbepWriteCmd(&bbep, UC8151_DTM1);
        image(half);
        digitalWrite(bbep.iCSPin, HIGH); digitalWrite(bbep.iCS2Pin, HIGH);
        bbep.cs_mode = CMD_CS1;
        return;
    }
    bbepSetAddrWindow(&bbep, 0, 0, w, h);
    if (bbep.iFlags & (BBEP_3COLOR | BBEP_4GRAY)) {
        bbepStartWrite(&bbep, (bbep.iFlags & BBEP_3COLOR) ? PLANE_0 : PLANE_1);
        bbepSetAddrWindow(&bbep, 0, 0, w, h);
        bbepStartWrite(&bbep, PLANE_0);
        image(plane_bytes());
        bbepSetAddrWindow(&bbep, 0, 0, w, h);
        bbepStartWrite(&bbep, PLANE_1);
        image(plane_bytes());
    } else if (bbep.iFlags & BBEP_7COLOR) {
        bbepStartWrite(&bbep, PLANE_1);
        image(((w + 1) / 2) * h);
    } else if (bbep.iFlags & BBEP_4COLOR) {
        bbepStartWrite(&bbep, PLANE_1);
        image(((w + 3) / 4) * h);
    } else {
        bbepStartWrite(&bbep, PLANE_0);
        image(plane_bytes());
    }
}

/* Firmware partial_set_addr_window() for the g_r* rect. */
static void fw_partial_window(void)
{
    const int x = g_rx, y = g_ry, cy = g_rh;
    int cx = (g_rw + 7) & 0xfff8;
    uint8_t uc[4];
    if (!(is397() || is426())) {
        bbepSetAddrWindow(&bbep, x, y, g_rw, cy);
        return;
    }
    int px0 = x, px1 = x + cx - 1;
    if (is426()) { px0 = (bbep.native_width - 1) - x; px1 = (bbep.native_width - 1) - (x + cx - 1); }
    bbepWriteCmd(&bbep, SSD1608_SET_RAMXPOS);
    uc[0] = (uint8_t)px0; uc[1] = (uint8_t)(px0 >> 8); uc[2] = (uint8_t)px1; uc[3] = (uint8_t)(px1 >> 8);
    bbepWriteData(&bbep, uc, 4);
    bbepWriteCmd(&bbep, SSD1608_SET_RAMXCOUNT);
    uc[0] = (uint8_t)px0; uc[1] = (uint8_t)(px0 >> 8);
    bbepWriteData(&bbep, uc, 2);
    if (is426()) {
        int yl = y + cy - 1;
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYPOS);
        uc[0] = (uint8_t)y; uc[1] = (uint8_t)(y >> 8); uc[2] = (uint8_t)yl; uc[3] = (uint8_t)(yl >> 8);
        bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYCOUNT);
        uc[0] = (uint8_t)y; uc[1] = (uint8_t)(y >> 8);
        bbepWriteData(&bbep, uc, 2);
    } else {
        int ys = (bbep.native_height - 1) - y, ye = (bbep.native_height - 1) - (y + cy - 1);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYPOS);
        uc[0] = (uint8_t)ys; uc[1] = (uint8_t)(ys >> 8); uc[2] = (uint8_t)ye; uc[3] = (uint8_t)(ye >> 8);
        bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYCOUNT);
        uc[0] = (uint8_t)ys; uc[1] = (uint8_t)(ys >> 8);
        bbepWriteData(&bbep, uc, 2);
    }
    bbepWaitBusy(&bbep);
}

static const uint8_t epd426_partial_lut[105] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x32, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22,
};

/* Firmware partial_trigger_refresh(REFRESH_PARTIAL) */
static void fw_partial_refresh(void)
{
    if (is397() || is426()) {
        if (is_type(EP426_800x480)) {
            uint8_t ctrl1[2] = {0x00, 0x00};
            uint8_t src[3] = {0x41, 0xa8, 0x32};
            bbepWriteCmd(&bbep, SSD1608_WRITE_LUT);
            bbepWriteData(&bbep, (uint8_t *)epd426_partial_lut, (int)sizeof(epd426_partial_lut));
            bbepCMD2(&bbep, SSD1608_GATE_VOLTAGE, 0x17);
            bbepWriteCmd(&bbep, SSD1608_SOURCE_VOLTAGE);
            bbepWriteData(&bbep, src, 3);
            bbepCMD2(&bbep, SSD1608_WRITE_VCOM, 0x40);
            bbepWriteCmd(&bbep, SSD1608_DISP_CTRL1);
            bbepWriteData(&bbep, ctrl1, 2);
            bbepCMD2(&bbep, SSD1608_DISP_CTRL2, 0xdc);
            bbepWriteCmd(&bbep, SSD1608_MASTER_ACTIVATE);
            return;
        }
        bbepCMD2(&bbep, SSD1608_DISP_CTRL2, is397() ? 0xff : 0xff);
        bbepWriteCmd(&bbep, SSD1608_MASTER_ACTIVATE);
        return;
    }
    bbepRefresh(&bbep, REFRESH_PARTIAL);
}

static void flow_refresh(int mode)
{
    bbepWakeUp(&bbep);
    bbepSendCMDSequence(&bbep, bbep.pInitFull);
    fw_align();
    write_frame();
    bbepRefresh(&bbep, mode);
    bbepWaitBusy(&bbep);
    if (bbep.iFlags & BBEP_SPLIT_BUFFER) {   /* splitPanelPowerOff() */
        bbep.cs_mode = CMD_CS1_CS2;
        bbepCMD2(&bbep, UC8151_POFF, 0x00);
        bbepWaitBusy(&bbep);
        bbep.cs_mode = CMD_CS1;
    }
}

static void flow_partial(void)
{
    const bool full = g_rx == 0 && g_ry == 0 && g_rw == bbep.native_width && g_rh == bbep.native_height;
    const int rect_bytes = ((g_rw + 7) / 8) * g_rh;
    bbepWakeUp(&bbep);
    bbepSendCMDSequence(&bbep, bbep.pInitPart ? bbep.pInitPart : bbep.pInitFull);
    fw_align();
    if (!full) {   /* partial_prepare_panel_ram: planes equal outside the rect */
        bbepFill(&bbep, BBEP_WHITE, PLANE_1);
        bbepFill(&bbep, BBEP_WHITE, PLANE_0);
    }
    fw_partial_window();
    bbepStartWrite(&bbep, PLANE_1);
    image(rect_bytes);
    fw_partial_window();
    bbepStartWrite(&bbep, PLANE_0);
    image(rect_bytes);
    fw_partial_refresh();
    bbepWaitBusy(&bbep);
}

static void flow_sleep(void)
{
    if (bbep.chip_type == BBEP_CHIP_SSD16xx && bbep.is_awake && !is426()) {
        bbepCMD2(&bbep, 0x3F, 0x22);
        bbepCMD2(&bbep, SSD1608_DISP_CTRL2, 0x83);
        bbepWriteCmd(&bbep, SSD1608_MASTER_ACTIVATE);
        bbepWaitBusy(&bbep);
    }
    bbepSleep(&bbep, 1);
}

static bool has_partial(void)
{
    return bbep.pInitPart && !(bbep.iFlags & (BBEP_3COLOR | BBEP_4COLOR | BBEP_7COLOR | BBEP_4GRAY));
}

static void emit_rects(void)
{
    for (int idx = 1; idx < EP_PANEL_COUNT; idx++) {
        setup_panel(idx);
        if (!has_partial()) continue;
        const int w = bbep.native_width, h = bbep.native_height;
        /* x and width multiples of 8 (byte-addressed RAM X); the last rect touches the far
         * corner so coordinate high bytes are exercised on panels wider/taller than 256. */
        const int r[3][4] = {
            {8, 16, 32, 24},
            {((w / 2) & ~7), h / 2, 16, h / 4},
            {(((w - 16) / 8) * 8) - 8, h - 13, 16, 12},
        };
        printf("P %d %d %d %d %u %d %d %d\n", idx, w, h, bbep.chip_type, (unsigned)bbep.iFlags,
               bbep.x_offset, bbep.pInitFast != NULL, 1);
        for (int k = 0; k < 3; k++) {
            setup_panel(idx);
            g_rx = r[k][0]; g_ry = r[k][1]; g_rw = r[k][2]; g_rh = r[k][3];
            printf("F partial_r%d %d %d %d %d\n", k, g_rx, g_ry, g_rw, g_rh);
            flow_partial();
            printf("E\n");
        }
    }
}

int main(int argc, char **argv)
{
    g_rec = stdout;
    if (argc > 1 && strcmp(argv[1], "rects") == 0) {
        emit_rects();
        return 0;
    }
    for (int idx = 1; idx < EP_PANEL_COUNT; idx++) {
        setup_panel(idx);
        g_rx = 0; g_ry = 0; g_rw = bbep.native_width; g_rh = bbep.native_height;
        printf("P %d %d %d %d %u %d %d %d\n", idx, bbep.native_width, bbep.native_height,
               bbep.chip_type, (unsigned)bbep.iFlags, bbep.x_offset,
               bbep.pInitFast != NULL, bbep.pInitPart != NULL);
        printf("L");
        for (int k = 0; k < 16; k++) printf(" %d", bbep.pColorLookup ? bbep.pColorLookup[k] : k);
        printf("\n");
        printf("F full\n");  setup_panel(idx); flow_refresh(REFRESH_FULL); printf("E\n");
        if (bbep.pInitFast) { printf("F fast\n"); setup_panel(idx); flow_refresh(REFRESH_FAST); printf("E\n"); }
        if (has_partial()) {
            printf("F partial\n"); setup_panel(idx); flow_partial(); printf("E\n");
        }
        printf("F sleep\n"); setup_panel(idx); flow_sleep(); printf("E\n");
    }
    return 0;
}
