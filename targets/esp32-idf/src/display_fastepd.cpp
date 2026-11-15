#if defined(OPENDISPLAY_FASTEPD)

#include "display_fastepd.h"
#include "display_it8951.h"
#include "od_controller_esp.h"
#include "od_sensor_sht40.h"
#include "display_service.h"
#include "od_log.h"
#include "structs.h"
/* No <Arduino.h>: this file never used one. Its only Arduino-shaped name -- the SPI object --
 * comes from the FastEPD vendor adapter below, which is permanent and deliberately not counted
 * by the arduino-free check in tools/check.sh. */
#include <SPI.h>
#include <string.h>
#include <FastEPD.h>

extern struct od_config globalConfig;

// FastEPD's cached PCAL6416A write
extern void bbepPCALDigitalWrite(uint8_t pin, uint8_t value);
static void fastepd_inkplate_wakeup_off(void);

class OdFastEPD : public FASTEPD {
public:
    FASTEPDSTATE* state() { return &_state; }
};

// The controller description (0xB3 display_controller, read by
// od_controller_esp.cpp) -> FastEPD native parallel panel id, -1 if not a parallel panel.
// FastEPD owns the Inkplate bus/PMIC/IO-expanders internally.
static int fastepd_parallel_panel(void) {
    const struct od_controller_desc* c = od_controller_esp();
    if (c->cls != OD_CTRL_CLASS_PARALLEL) return -1;
    switch (c->parallel_panel) {
        case OD_CTRL_PARALLEL_INKPLATE5V2:   return BB_PANEL_INKPLATE5V2;
        case OD_CTRL_PARALLEL_INKPLATE10:    return BB_PANEL_INKPLATE10;
        case OD_CTRL_PARALLEL_M5PAPERS3:     return BB_PANEL_M5PAPERS3;
        default:                             return -1;
    }
}

static int8_t s_mosi = 9;
static int8_t s_miso = 8;
static int8_t s_sclk = 7;
static int8_t s_cs = 10;
static int8_t s_rst = 12;
static int8_t s_busy = 13;
static int8_t s_en = 11;
static int8_t s_ite_en = 21;

static bool s_init_failed = false;

extern "C" {

/* SCLK/MOSI/CS/BUSY/RST from the DisplayConfig; MISO, EN and ITE_EN from the 0xB3 controller
 * description. A pin left unset keeps the E1003 default above. */
void opendisplay_fastepd_load_pins_from_display(const struct DisplayConfig* d, const struct SystemConfig* sys, uint16_t panel_ic_type) {
    (void)sys;
    (void)panel_ic_type;
    if (!d) return;
    const struct od_controller_desc* c = od_controller_esp();
    if (c->cls != OD_CTRL_CLASS_IT8951) return;
    if (d->clk_pin != 0xFF) s_sclk = (int8_t)d->clk_pin;
    if (d->data_pin != 0xFF) s_mosi = (int8_t)d->data_pin;
    if (d->cs_pin != 0xFF) s_cs = (int8_t)d->cs_pin;
    if (d->reset_pin != 0xFF) s_rst = (int8_t)d->reset_pin;
    if (d->busy_pin != 0xFF) s_busy = (int8_t)d->busy_pin;
    if (c->miso_pin != OD_CTRL_PIN_NONE) s_miso = (int8_t)c->miso_pin;
    s_en = c->en_pin != OD_CTRL_PIN_NONE ? (int8_t)c->en_pin : -1;
    s_ite_en = c->ite_en_pin != OD_CTRL_PIN_NONE ? (int8_t)c->ite_en_pin : -1;
}

bool fastepd_init_failed(void) {
    return s_init_failed || od_it8951_init_failed();
}

} // extern "C"

static bool panel_is_it8951(void) {
    return globalConfig.display_count > 0 && od_controller_esp()->cls == OD_CTRL_CLASS_IT8951;
}

static bool fastepd_panel_is_4gray(void);

// Mounting and VCOM from the controller description (ED103TC2: mirrored, 1500 mV; M5Paper
// ED047TC1: unmirrored, stored VCOM).
static void it8951_load_panel(void) {
    const struct od_controller_desc* c = od_controller_esp();
    od_it8951_set_layout((c->flags & OD_CTRL_FLAG_MIRRORED) != 0u, c->vcom_mv);
    od_it8951_set_panel(s_mosi, s_miso, s_sclk, s_cs, s_busy, s_rst, s_en, s_ite_en,
                        globalConfig.displays[0].pixel_width,
                        globalConfig.displays[0].pixel_height, fastepd_panel_is_4gray());
}

static void update_it8951_temperature(void) {
    const struct od_controller_desc* c = od_controller_esp();
    int temp_c = 0;
    bool valid = (c->flags & OD_CTRL_FLAG_TEMP_SENSOR) != 0u &&
                 od_sensor_sht40_read_temp_c(&globalConfig, &temp_c);

    if (!valid && c->default_temp_c != OD_CTRL_TEMP_DEFAULT) {
        temp_c = c->default_temp_c;
        valid = true;
    }
    od_it8951_set_temperature(valid, temp_c);
}

static OdFastEPD g_epd;
static uint32_t s_direct_offset;
static bool s_hw_initialized = false;
/* The panel FastEPD's buffer was allocated for (fastepd_parallel_panel() value). FastEPD sizes
 * that buffer once, from the panel's native size, and cannot free or re-allocate it, so a
 * config that names another panel or another size takes a reboot. */
static int s_buf_panel = -2;

static uint16_t s_partial_x;
static uint16_t s_partial_y;
static uint16_t s_partial_w;
static uint16_t s_partial_h;
static uint32_t s_partial_plane_size;
static uint32_t s_partial_bytes_written;
static uint32_t s_partial_expected;

// True when the framebuffer is 4bpp (16-level gray): ED103TC2 gray or GRAY16.
static bool fastepd_panel_is_4gray(void) {
    if (globalConfig.display_count < 1) return false;
    const struct DisplayConfig& d = globalConfig.displays[0];
    if ((od_controller_esp()->flags & OD_CTRL_FLAG_GRAY16) != 0u) return true;
    return d.color_scheme == OD_COLOR_SCHEME_GRAY16;
}

// The pixel mode follows color_scheme, which a CONFIG_WRITE can change between transfers.
// FastEPD's mode is sticky per hardware init, so the warm paths re-assert it or incoming data
// is read at the previous config's depth.
static void fastepd_apply_mode(void) {
    if (!s_hw_initialized || !g_epd.currentBuffer()) return;
    const int want = fastepd_panel_is_4gray() ? BB_MODE_4BPP : BB_MODE_1BPP;
    if (g_epd.getMode() != want) {
        g_epd.setMode(want);
        g_epd.setPreviousMode((uint8_t)g_epd.getMode());
    }
}

// Every size below is FastEPD's native one, i.e. the size of the allocated buffer.
static unsigned row_pitch_bytes(void) {
    const unsigned w = (unsigned)g_epd.state()->native_width;
    return fastepd_panel_is_4gray() ? (w + 1u) / 2u : (w + 7u) / 8u;
}

static size_t fb_byte_size(void) {
    return (size_t)row_pitch_bytes() * (size_t)g_epd.state()->native_height;
}

// True when the config still describes the panel the buffer was allocated for. A mismatch is
// a config written since boot; it fails the display until a reboot re-allocates.
static bool fb_matches_config(void) {
    const FASTEPDSTATE* st = g_epd.state();
    if (globalConfig.display_count < 1) return false;
    const struct DisplayConfig& d = globalConfig.displays[0];
    return fastepd_parallel_panel() == s_buf_panel &&
           d.pixel_width == (uint32_t)st->native_width &&
           d.pixel_height == (uint32_t)st->native_height;
}

// The framebuffer, or NULL (and the driver marked failed) when the config no longer matches it.
static uint8_t* fb(void) {
    uint8_t* p = g_epd.currentBuffer();
    if (!p) return nullptr;
    if (!fb_matches_config()) {
        if (!s_init_failed) {
            od_log_error("fastepd: config is %ux%u panel %d, buffer is %dx%d panel %d; reboot to apply",
                         (unsigned)globalConfig.displays[0].pixel_width,
                         (unsigned)globalConfig.displays[0].pixel_height, fastepd_parallel_panel(),
                         g_epd.state()->native_width, g_epd.state()->native_height, s_buf_panel);
        }
        s_init_failed = true;
        return nullptr;
    }
    return p;
}

static void blit_1bpp_rect(uint8_t* dst, const uint8_t* src, uint32_t src_len,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint32_t* io_offset) {
    if (!dst || !src || !io_offset) return;
    const unsigned panel_pitch = ((unsigned)g_epd.state()->native_width + 7u) / 8u;
    const unsigned rect_pitch = (w + 7u) / 8u;
    uint32_t off = *io_offset;
    uint32_t end = off + src_len;
    uint32_t src_i = 0;
    while (off < end && src_i < src_len) {
        uint32_t row = off / rect_pitch;
        uint32_t col = off % rect_pitch;
        if (row >= h) break;
        uint32_t chunk = rect_pitch - col;
        if (chunk > end - off) chunk = end - off;
        memcpy(dst + (size_t)(y + row) * panel_pitch + (x / 8u) + col, src + src_i, chunk);
        off += chunk;
        src_i += chunk;
    }
    *io_offset = off;
}

void fastepd_prepare_hardware(void) {
    if (panel_is_it8951()) {
        opendisplay_fastepd_load_pins_from_display(&globalConfig.displays[0], &globalConfig.system_config,
                                                   globalConfig.displays[0].panel_ic_type);
        it8951_load_panel();
        od_it8951_prepare_hardware();
        return;
    }
    if (globalConfig.display_count < 1) {
        return;
    }
    const struct DisplayConfig& d = globalConfig.displays[0];
    opendisplay_fastepd_load_pins_from_display(&d, &globalConfig.system_config, d.panel_ic_type);
}

void fastepd_epaper_begin(void) {
    if (panel_is_it8951()) {
        opendisplay_fastepd_load_pins_from_display(&globalConfig.displays[0], &globalConfig.system_config,
                                                   globalConfig.displays[0].panel_ic_type);
        it8951_load_panel();
        od_it8951_epaper_begin();
        return;
    }
    fastepd_prepare_hardware();
    s_init_failed = false;
    initOrRestoreWireForOpenDisplay();

    // Native parallel panels (Inkplate): initPanel() sets up the bus/PMIC and
    // allocates the buffers. Guard on currentBuffer() — initPanel() re-allocs
    // without freeing, so a warm re-init would leak; deInit() keeps the bus up.
    int parallel = fastepd_parallel_panel();
    if (parallel >= 0) {
        if (!g_epd.currentBuffer()) {
            int prc = g_epd.initPanel(parallel);
            if (prc != BBEP_SUCCESS || !g_epd.currentBuffer()) {
                s_init_failed = true;
                s_hw_initialized = false;
                return;
            }
            s_buf_panel = parallel;
            g_epd.setMode(fastepd_panel_is_4gray() ? BB_MODE_4BPP : BB_MODE_1BPP);
            g_epd.setPreviousMode((uint8_t)g_epd.getMode());
            if (!fb_matches_config()) {
                (void)fb();   // logs and marks the failure
                s_hw_initialized = false;
                return;
            }
        } else if (!fb_matches_config()) {
            (void)fb();
            s_hw_initialized = false;
            return;
        } else {
            g_epd.einkPower(1);
            s_hw_initialized = true;   // fastepd_apply_mode() requires it
            fastepd_apply_mode();
        }
        s_hw_initialized = true;
        return;
    }

    // Neither IT8951 nor a known parallel panel: nothing here can drive it. IT8951 panels go
    // through display_it8951.cpp above, never through FastEPD's own IT8951 path.
    od_log_error("fastepd: 0xB3 names no panel this driver knows");
    s_init_failed = true;
    s_hw_initialized = false;
}

// REFRESH_FULL: parallel panels flash-clear to flush ghosting. PaperS3 needs the
// library-default 10-pass clear: CLEAR_FAST's 8 passes leave residual banding there.
// Inkplates keep the faster clear they were tuned with.
static void fastepd_full_refresh_impl(void) {
    const int clear = (od_controller_esp()->parallel_panel == OD_CTRL_PARALLEL_M5PAPERS3)
                          ? CLEAR_SLOW
                          : CLEAR_FAST;
    g_epd.fullUpdate(clear, true, NULL);
}

void fastepd_full_update(void) {
    if (panel_is_it8951()) { update_it8951_temperature(); od_it8951_full_update(); return; }
    if (!fb()) return;
    fastepd_full_refresh_impl();
    g_epd.backupPlane();
}

bool fastepd_wait_refresh(int timeout_sec) {
    if (panel_is_it8951()) return od_it8951_wait_refresh(timeout_sec);
    (void)timeout_sec;
    return !s_init_failed;
}

void fastepd_sleep_after_refresh(void) {
    if (panel_is_it8951()) { od_it8951_sleep_after_refresh(); return; }
    g_epd.einkPower(0);
    fastepd_inkplate_wakeup_off();
    g_epd.deInit();
}

void fastepd_boot_write_row(uint16_t y, const uint8_t* row, unsigned pitch) {
    if (panel_is_it8951()) { od_it8951_boot_write_row(y, row, pitch); return; }
    uint8_t* p = fb();
    if (!p || !row || y >= (unsigned)g_epd.state()->native_height) return;
    unsigned rp = row_pitch_bytes();
    if (pitch < rp) return;
    memcpy(p + (size_t)y * rp, row, rp);
}

void fastepd_boot_skip_planes(void) {
    if (panel_is_it8951()) { od_it8951_boot_skip_planes(); return; }
}

void fastepd_direct_write_reset(void) {
    if (panel_is_it8951()) { od_it8951_direct_write_reset(); return; }
    fastepd_prepare_hardware();
    if (!s_hw_initialized) {
        fastepd_epaper_begin();
    } else {
        g_epd.einkPower(1);
        fastepd_apply_mode();
    }
    s_direct_offset = 0;
    uint8_t* p = fb();
    if (p) {
        memset(p, 0xFF, fb_byte_size());
    }
}

bool fastepd_direct_write_chunk(const uint8_t* data, uint32_t len) {
    if (panel_is_it8951()) return od_it8951_direct_write_chunk(data, len);
    if (!data || len == 0) return true;
    uint8_t* base = fb();
    if (!base || s_init_failed || !s_hw_initialized) return false;
    size_t maxb = fb_byte_size();
    size_t room = (s_direct_offset < maxb) ? (maxb - s_direct_offset) : 0;
    size_t n = (len > room) ? room : (size_t)len;
    if (n) {
        memcpy(base + s_direct_offset, data, n);
        s_direct_offset += n;
    }
    return n == len;
}

void fastepd_direct_refresh(int refresh_mode) {
    if (panel_is_it8951()) { update_it8951_temperature(); od_it8951_direct_refresh(refresh_mode); return; }
    if (!fb()) return;
    if (refresh_mode == 1) {
        // FAST: 1bpp uses FastEPD's native diff. 4bpp has no fast path (fastUpdate is
        // 1bpp-only) -> fall back to full.
        if (fastepd_panel_is_4gray()) fastepd_full_refresh_impl();
        else g_epd.fastUpdate(true);
    } else {
        fastepd_full_refresh_impl();
    }
    g_epd.backupPlane();
}

// FastEPD's Inkplate einkPower(0) drops the rails but leaves the TPS65186 WAKEUP
// asserted, so the PMIC stays awake and the board idles high.
static void fastepd_inkplate_wakeup_off(void) {
    const uint8_t p = od_controller_esp()->parallel_panel;
    if (od_controller_esp()->cls != OD_CTRL_CLASS_PARALLEL ||
        (p != OD_CTRL_PARALLEL_INKPLATE5V2 && p != OD_CTRL_PARALLEL_INKPLATE10)) return;
    FASTEPDSTATE* st = g_epd.state();
    if (st) bbepPCALDigitalWrite(st->panelDef.ioShiftSTR, 0); // WAKEUP low -> PMIC shutdown
}

void fastepd_direct_sleep(void) {
    if (panel_is_it8951()) { od_it8951_direct_sleep(); return; }
    g_epd.einkPower(0);
    fastepd_inkplate_wakeup_off();
    g_epd.deInit();
}

void fastepd_mark_hw_deinitialized(void) {
    if (panel_is_it8951()) { od_it8951_mark_hw_deinitialized(); return; }
    s_hw_initialized = false;
}

void fastepd_partial_prepare(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (panel_is_it8951()) { od_it8951_partial_prepare(x, y, w, h); return; }
    fastepd_prepare_hardware();
    if (!s_hw_initialized) {
        fastepd_epaper_begin();
    } else {
        g_epd.einkPower(1);
        fastepd_apply_mode();
    }
    s_partial_x = x;
    s_partial_y = y;
    s_partial_w = w;
    s_partial_h = h;
    s_partial_plane_size = ((uint32_t)(w + 7u) / 8u) * h;
    s_partial_expected = s_partial_plane_size * 2u;
    s_partial_bytes_written = 0;
}

bool fastepd_partial_write_chunk(const uint8_t* data, uint32_t len) {
    if (panel_is_it8951()) return od_it8951_partial_write_chunk(data, len);
    if (!data || len == 0) return true;
    if (s_partial_bytes_written >= s_partial_expected) return false;
    if (len > s_partial_expected - s_partial_bytes_written) return false;

    uint8_t* prev = g_epd.previousBuffer();
    uint8_t* cur = fb();
    if (!cur) return false;
    const FASTEPDSTATE* st = g_epd.state();
    if ((uint32_t)s_partial_x + s_partial_w > (uint32_t)st->native_width ||
        (uint32_t)s_partial_y + s_partial_h > (uint32_t)st->native_height) return false;

    uint32_t remaining = len;
    const uint8_t* p = data;

    if (s_partial_bytes_written < s_partial_plane_size) {
        uint32_t into_old = s_partial_plane_size - s_partial_bytes_written;
        if (into_old > remaining) into_old = remaining;
        if (prev) {
            uint32_t off = s_partial_bytes_written;
            blit_1bpp_rect(prev, p, into_old, s_partial_x, s_partial_y, s_partial_w, s_partial_h, &off);
        }
        s_partial_bytes_written += into_old;
        p += into_old;
        remaining -= into_old;
    }

    if (remaining > 0 && s_partial_bytes_written >= s_partial_plane_size) {
        uint32_t new_off = s_partial_bytes_written - s_partial_plane_size;
        uint32_t off = new_off;
        blit_1bpp_rect(cur, p, remaining, s_partial_x, s_partial_y, s_partial_w, s_partial_h, &off);
        s_partial_bytes_written += remaining;
    }

    return true;
}

bool fastepd_partial_refresh(int refresh_mode) {
    if (panel_is_it8951()) { update_it8951_temperature(); return od_it8951_partial_refresh(refresh_mode); }
    if (s_partial_bytes_written != s_partial_expected) return false;
    if (!fb()) return false;

    if (refresh_mode == 0 || fastepd_panel_is_4gray()) {
        // Rect already blitted into the framebuffer; 4bpp has no fast path.
        fastepd_full_refresh_impl();
    } else {
        g_epd.fastUpdate(true);
    }
    g_epd.backupPlane();
    return true;
}


// The panel SPI bus release, called from main.cpp's deep-sleep teardown via
// display_service.h. It lives HERE, not in display_service.cpp, because SPI is the FastEPD
// vendor adapter's object and this is the file that owns it -- moving it is what took
// display_service.cpp off the adapter entirely (main/CMakeLists.txt per-source grants).
//
// Early return when the parallel driver is in use: that path drives the S3 LCD peripheral and
// never took the SPI bus. Behaviour is unchanged from when this lived in display_service.cpp.
// display_service.cpp carries a no-op fallback for boards that compile no FastEPD.
void displayReleaseSpiBus(void) {
    if (panel_is_it8951()) {
        od_it8951_release_bus();
        return;
    }
    if (fastepd_driver_used()) {
        return;   // parallel driver: the SPI bus was never taken
    }
    SPI.end();
}

#endif
