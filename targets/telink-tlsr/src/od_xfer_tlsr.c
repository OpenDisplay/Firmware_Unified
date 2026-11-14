/* Transfer-plane seams: od_xfer's full-image hooks over the imported Firmware_NRF52 panel drivers
 * (src/epd/), plus the NFC and inflate seams.
 *
 * The stream goes to the controller unchanged, one write_ram() per chunk, exactly as
 * Firmware_NRF52's EPD_service did with the same host data -- that firmware is what py-opendisplay
 * already drives these controllers through. No framebuffer: a chunk is on the panel's RAM before
 * od_xfer asks for the next one.
 *
 * Structure follows targets/efr32bg22-slc/opendisplay_display.cpp (the plane walk, the refresh
 * barrier, power down after refresh). One deliberate difference: a flush TIMEOUT before refresh
 * proceeds, as od_txq.h specifies, where BG22 aborts. */

#include "od_boot_app.h"
#include "od_inflate_app.h"
#include "od_nfc_app.h"
#include "od_xfer_app.h"

#include "EPD_driver.h"
#include "od_color.h"
#include "od_hal_time.h"
#include "od_tlsr.h"
#include "od_txq.h"
#include "tlsr_port.h"

#include <string.h>

#define REFRESH_BARRIER_MS  2000u
#define WRITE_CHUNK_MAX     255u      /* write_ram() takes a uint8_t length */
#define SERVICE_EVERY_BYTES 2048u     /* panel bytes between stack/watchdog services */

static struct {
    bool                active;
    epd_model_t        *epd;
    od_color_geometry_t geometry;
    uint32_t            plane_size;   /* bytes in plane 0 when the layout has two planes */
    int8_t              plane;        /* plane currently open on the controller, -1 = none */
} s_xfer;

static uint32_t s_displayed_etag;
static uint8_t  s_inflate_scratch[256];

static const struct DisplayConfig *display_cfg(void)
{
    const struct od_config *cfg = od_tlsr_config();
    return cfg->display_count != 0u ? &cfg->displays[0] : NULL;
}

/* Canonical PanelIC 1000-1030 is the Firmware_NRF52 model line (model id + 999). The imported
 * map_panel_ic_to_model_id() also takes raw 1-31, a legacy nRF52 path -- but canonically 1-51 are
 * bb_epaper panels, so accepting them would drive a different controller. Anything else would hit
 * its silent fallback to a 4.2" UC8176. Refuse both here. */
static bool panel_type_known(uint16_t t)
{
    /* 1031 HS_266_BWR, 1032 HS_200_BWY, 1033 HS_350_BWY, 1034 TI_970_BWR: provisional */
    return t >= 1000u && t <= 1034u;
}

/* Panels split over two controllers, whose right half is selected by DisplayConfig.cs_pin_2. */
static bool panel_dual_cs(uint16_t t)
{
    return t == 1034u;
}

/* The second panel supply switch (ATC's enable1) of a dual-controller board, carried in
 * SystemConfig.pwr_pin_2. That field is also the power-latch pin when a latch flag is set, and 0
 * (PA0) in configs that never set it, so it is a panel enable only for such a panel on a board
 * without a latch. */
uint8_t od_tlsr_panel_pwr2(const struct od_config *cfg)
{
    if (cfg == NULL || cfg->display_count == 0u || !panel_dual_cs(cfg->displays[0].panel_ic_type) ||
        (cfg->system_config.device_flags & (OD_DEVICE_FLAG_PWR_LATCH | OD_DEVICE_FLAG_PWR_LATCH_DFF)) != 0u) {
        return TLSR_PORT_PIN_NONE;
    }
    return cfg->system_config.pwr_pin_2;
}

/* The imported lookup only knows 1000-1030 (and falls back to a 4.2" UC8176 otherwise); past
 * that the model id is simply value - 999. Either way begin_full() re-checks size and colour. */
static epd_model_id_t model_for(uint16_t t, uint8_t scheme)
{
    return t <= 1030u ? map_panel_ic_to_model_id(t, scheme) : (epd_model_id_t)(t - 999u);
}

static bool scheme_matches(uint8_t scheme, epd_color_t color)
{
    switch (scheme) {
    case OD_COLOR_SCHEME_MONO: return color == COLOR_BW;
    case OD_COLOR_SCHEME_BWR:
    case OD_COLOR_SCHEME_BWY:  return color == COLOR_BWR;
    case OD_COLOR_SCHEME_BWRY: return color == COLOR_BWRY;
    default:                   return false;
    }
}

static void panel_down(void)
{
    if (s_xfer.epd != NULL) {
        s_xfer.epd->drv->sleep(s_xfer.epd);
    }
    EPD_GPIO_Uninit();
    memset(&s_xfer, 0, sizeof(s_xfer));
    s_xfer.plane = -1;
    tlsr_port_stay_awake(TLSR_PORT_AWAKE_PANEL, false);
}

void od_xfer_app_prepare_start(void)
{
    if (s_xfer.active) {
        panel_down();
    }
}

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    const struct DisplayConfig *d = display_cfg();

    if (out == NULL || d == NULL || !panel_type_known(d->panel_ic_type)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                 &out->geometry) != OD_COLOR_OK) {
        return false;
    }
    out->width = d->pixel_width;
    out->height = d->pixel_height;
    out->partial_enabled = false;
    return true;
}

/* Power the panel up and initialise it for the configured model. Shared by od_xfer's full-image
 * start and the boot screen, so both go through the same model check. */
static bool panel_start(const od_color_geometry_t *geometry)
{
    const struct DisplayConfig *d = display_cfg();
    const struct od_config *cfg = od_tlsr_config();
    struct epd_io_pins pins;
    epd_model_t *epd;

    if (d == NULL || geometry == NULL || geometry->total_bytes == 0u ||
        geometry->layout == OD_COLOR_LAYOUT_SPLIT_HALVES || !panel_type_known(d->panel_ic_type)) {
        return false;
    }
    pins.mosi = d->data_pin;
    pins.sclk = d->clk_pin;
    pins.cs   = d->cs_pin;
    pins.dc   = d->dc_pin;
    pins.rst  = d->reset_pin;
    pins.busy = d->busy_pin;
    pins.pwr  = cfg->system_config.pwr_pin;
    pins.pwr2 = od_tlsr_panel_pwr2(cfg);
    /* cs_pin_2 was a reserved (zero) byte before protocol 1.4, and 0 is PA0: read it only for a
     * model that has a second controller. */
    pins.cs2  = panel_dual_cs(d->panel_ic_type) ? d->cs_pin_2 : TLSR_PORT_PIN_NONE;
    tlsr_port_stay_awake(TLSR_PORT_AWAKE_PANEL, true);    /* released by panel_down() */
    epd_io_configure(&pins);
    EPD_GPIO_Init();

    epd = epd_init(model_for(d->panel_ic_type, d->color_scheme));
    /* The model table fixes the controller's resolution; a config that disagrees would be streamed
     * into the wrong RAM layout. */
    if (epd->width != d->pixel_width || epd->height != d->pixel_height ||
        !scheme_matches(d->color_scheme, epd->color)) {
        s_xfer.epd = epd;
        panel_down();
        return false;
    }

    /* The controller's own temperature sensor; ignored when the read-back is implausible
     * (a panel without a readable sensor returns 0xFF or 0x00 on the idle data line). */
    {
        int8_t t = epd->drv->read_temp(epd);
        if (t > -30 && t < 70 && t != 0) {
            od_tlsr_set_temperature(t);
        }
    }

    memset(&s_xfer, 0, sizeof(s_xfer));
    s_xfer.active = true;
    s_xfer.epd = epd;
    s_xfer.geometry = *geometry;
    s_xfer.plane_size = geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
                        ? geometry->part_bytes[0] : geometry->total_bytes;
    s_xfer.plane = -1;
    return true;
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    return panel_start(geometry);
}

/* cfg byte as Firmware_NRF52's write_ram() reads it: high nibble 0 opens the plane (sends the
 * DTM/WRITE_RAM command), low nibble 0xF selects the black plane, anything else the red one. */
static void write_plane(uint8_t plane, const uint8_t *p, uint32_t n)
{
    static uint32_t s_since_service;

    while (n != 0u) {
        uint8_t len = (uint8_t)(n > WRITE_CHUNK_MAX ? WRITE_CHUNK_MAX : n);
        uint8_t cfg = (uint8_t)((s_xfer.plane == (int8_t)plane ? 0x10u : 0x00u) |
                                (plane == 0u ? 0x0Fu : 0x00u));

        s_xfer.epd->drv->write_ram(s_xfer.epd, cfg, (uint8_t *)p, len);
        s_xfer.plane = (int8_t)plane;
        p += len;
        n -= len;
        /* One compressed frame of a mostly blank image inflates to tens of KB, and bit-banged SPI
         * into a large panel then outlasts the 4 s watchdog inside a single dispatch. Service the
         * stack (which feeds it) as the refresh wait does. */
        s_since_service += len;
        if (s_since_service >= SERVICE_EVERY_BYTES) {
            s_since_service = 0u;
            tlsr_port_service_stack();
        }
    }
}

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
    uint32_t consumed = 0u;

    if (!s_xfer.active || !od_span_valid(data) || data.n == 0u ||
        stream_offset > s_xfer.geometry.total_bytes ||
        data.n > s_xfer.geometry.total_bytes - stream_offset) {
        return 0u;
    }
    while (consumed < data.n) {
        uint32_t logical = stream_offset + consumed;
        uint8_t plane = logical < s_xfer.plane_size ? 0u : 1u;
        uint32_t plane_end = plane == 0u ? s_xfer.plane_size : s_xfer.geometry.total_bytes;
        uint32_t chunk = plane_end - logical;

        if (chunk > data.n - consumed) {
            chunk = (uint32_t)data.n - consumed;
        }
        write_plane(plane, data.p + consumed, chunk);
        consumed += chunk;
    }
    return consumed;
}

od_mut_span_t od_xfer_app_inflate_scratch(void)
{
    return od_mut_span_make(s_inflate_scratch, sizeof(s_inflate_scratch));
}

void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
    (void)reason;
    if (s_xfer.active) {
        panel_down();
    }
}

/* Get the END ack onto the air before a refresh that can block for a minute. The stack has to
 * run for a notification to leave, so each drain attempt is followed by a stack pass. */
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
    uint32_t deadline = od_hal_uptime_ms() + REFRESH_BARRIER_MS;

    (void)owner;
    for (;;) {
        od_txq_status_t rc = od_txq_flush(od_hal_uptime_ms(), deadline);
        if (rc == OD_TXQ_OK || rc == OD_TXQ_TIMEOUT) {
            return OD_XFER_BARRIER_PROCEED;
        }
        tlsr_port_service_stack();
    }
}

void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
    (void)owner;
    od_xfer_app_abort(OD_XFER_ABORT_REPLY_FAILED);
}

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    (void)mode;                        /* the imported drivers have one (full) refresh */
    if (completed == NULL || !s_xfer.active) {
        return false;
    }
    s_xfer.epd->drv->refresh(s_xfer.epd);
    *completed = !epd_io_busy_timed_out();
    panel_down();
    return true;
}

uint32_t od_xfer_app_displayed_etag(void)
{
    return s_displayed_etag;
}

void od_xfer_app_set_displayed_etag(uint32_t etag)
{
    s_displayed_etag = etag;
}

uint32_t od_xfer_app_now_ms(void)
{
    return od_hal_uptime_ms();
}

/* ------------------------------------------------------------------------ boot screen --- */

/* shared/core/od_boot_screen.c renders native-orientation rows, top to bottom, plane 0 (1 = white)
 * then plane 1 (1 = colour) -- the same bytes a direct upload carries -- so the hooks are the
 * upload path fed one row at a time. Every row also services the BLE stack and the watchdog:
 * rendering a full screen in software takes long enough to matter on a 16 MHz core. */

static int s_boot_plane = -1;

/* First hook that refused, for the diagnostic byte od_tlsr_app.c advertises (0 = none). */
uint8_t od_tlsr_boot_fail;

static int boot_fail(uint8_t code)
{
    if (od_tlsr_boot_fail == 0u) {
        od_tlsr_boot_fail = code;
    }
    return -1;
}

int od_boot_app_begin_frame(uint16_t width, uint16_t height, uint8_t segments)
{
    const struct DisplayConfig *d = display_cfg();
    od_color_geometry_t geometry;

    if (d == NULL || segments != 1u || width != d->pixel_width || height != d->pixel_height ||
        od_color_direct_geometry(d->color_scheme, width, height, &geometry) != OD_COLOR_OK) {
        return boot_fail(0xE1);
    }
    s_boot_plane = -1;
    return panel_start(&geometry) ? 0 : boot_fail(0xE1);
}

int od_boot_app_begin_plane(int plane)
{
    if (!s_xfer.active || (plane != OD_BOOT_PLANE_PRIMARY && plane != OD_BOOT_PLANE_SECOND)) {
        return boot_fail(0xE2);
    }
    s_boot_plane = plane;
    return 0;
}

int od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len)
{
    (void)y;
    if (!s_xfer.active || s_boot_plane < 0 || segment != 0u || row == NULL || len == 0u) {
        return boot_fail(0xE3);
    }
    write_plane((uint8_t)s_boot_plane, row, len);
    tlsr_port_service_stack();
    return 0;
}

int od_boot_app_end_plane(int plane)
{
    (void)plane;
    s_boot_plane = -1;
    return s_xfer.active ? 0 : boot_fail(0xE4);
}

int od_boot_app_end_frame(void)
{
    bool completed = false;
    return od_xfer_app_refresh(0u, &completed) && completed ? 0 : boot_fail(0xE5);
}

int od_boot_app_bits_per_pixel(uint8_t color_scheme)
{
    return color_scheme == OD_COLOR_SCHEME_BWRY ? 2 : 1;
}

int od_boot_app_default_plane(uint8_t color_scheme)
{
    (void)color_scheme;
    return OD_BOOT_PLANE_PRIMARY;
}

bool od_boot_app_direct_2bpp(void)
{
    return true;   /* BWRY goes to the JD796xx as packed 2bpp, as a direct upload does */
}

uint8_t od_boot_app_segments(void)
{
    return 1u;
}

uint32_t od_boot_app_device_id24(void)
{
    uint8_t mac[6];
    tlsr_port_mac(mac);                      /* the same three bytes the ODxxxxxx name shows */
    return ((uint32_t)mac[2] << 16) | ((uint32_t)mac[1] << 8) | mac[0];
}

void od_boot_app_firmware_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    *major = (uint8_t)OD_TLSR_VERSION_MAJOR;
    *minor = (uint8_t)OD_TLSR_VERSION_MINOR;
    *patch = (uint8_t)OD_TLSR_VERSION_PATCH;
}

float od_tlsr_battery_volts(void);
float od_tlsr_temperature_c(void);

float od_boot_app_battery_volts(void)
{
    return od_tlsr_battery_volts();          /* < 0 prints "--V" */
}

float od_boot_app_chip_temp_c(void)
{
    return od_tlsr_temperature_c();          /* <= -900 prints "--C" */
}

/* ------------------------------------------------------------------------------- nfc --- */

/* No NFC front end on these tags. */
bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap)
{
    (void)type; (void)data; (void)len_io; (void)cap;
    return false;
}

bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    (void)type; (void)data; (void)len;
    return false;
}

/* --------------------------------------------------------------------------- inflate --- */

void od_inflate_app_reset(uint32_t expected_output_size)
{
    od_zlib_stream_reset(expected_output_size);
}

od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{
    return od_zlib_stream_push(input.p, input.n, final);
}

od_zlib_status_t od_inflate_app_poll(uint8_t *output, size_t capacity, size_t *produced)
{
    return od_zlib_stream_poll(output, capacity, produced);
}

const char *od_inflate_app_error(void)
{
    return od_zlib_stream_error();
}

uint32_t od_inflate_app_output_count(void)
{
    return od_zlib_stream_output_count();
}
