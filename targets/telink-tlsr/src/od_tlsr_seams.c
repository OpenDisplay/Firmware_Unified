/* The app seams shared/core calls that are not the panel's: SEC2 facts, claim-mode effects, NFC
 * and inflate. The panel half -- setup screens, the key-state redraw and the reset pin, which
 * need the pin encoding -- is od_tlsr_display.c. */
#include "od_tlsr_store.h"
#include "od_tlsr_target.h"

#include "od_advert.h"
#include "od_claim_app.h"
#include "od_config.h"
#include "od_hal_time.h"
#include "od_inflate_app.h"
#include "od_nfc_app.h"
#include "od_sec2_app.h"
#include "od_sec2_wire.h"
#include "od_xfer.h"

#include <string.h>

/* ---- od_sec2_app ---- */

uint32_t od_sec2_app_now_ms(void)
{
    return od_hal_uptime_ms();
}

/* SECURITY_V2 4.3 wants a stable per-chip identifier. This part's is its factory public
 * address, big-endian in the low six bytes. */
void od_sec2_app_uid(uint8_t uid[8])
{
    uint8_t mac[6];
    unsigned i;

    od_tlsr_mac(mac);
    uid[0] = 0u;
    uid[1] = 0u;
    for (i = 0u; i < 6u; ++i) {
        uid[2u + i] = mac[5u - i];
    }
}

uint8_t od_sec2_app_dflags(void)
{
    const struct SecurityConfig *sec = od_sec2_app_security();
    uint8_t f = 0u;

    if (od_ident_display_drivable(od_tlsr_config_loaded() ? od_tlsr_config() : NULL)) {
        f |= OD_SEC2_DFLAG_HAS_PANEL;
    }
    if (sec != NULL && (sec->flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) != 0u) {
        f |= OD_SEC2_DFLAG_HAS_RESET_PIN;
    }
    return f;
}

const struct SecurityConfig *od_sec2_app_security(void)
{
    const struct od_config *c = od_tlsr_config();
    return (od_tlsr_config_loaded() && c->security_loaded) ? &c->security : NULL;
}

bool od_sec2_app_display_busy(void)
{
    return od_xfer_active();
}

/* ---- od_claim_app ---- */

void od_claim_app_signal(enum od_claim_signal sig, uint8_t arg)
{
    (void)sig;
    (void)arg;
}

void od_claim_app_wait_ms(uint32_t ms)
{
    uint32_t start = od_hal_uptime_ms();

    while ((uint32_t)(od_hal_uptime_ms() - start) < ms) {
        od_tlsr_feed_watchdog();
    }
}

bool od_claim_app_erase_images(void)
{
    return true; /* no image store */
}

bool od_claim_app_erase_bonds(void)
{
    return true; /* no link-layer pairing, so no bonds */
}

bool od_claim_app_scrub_extras(void)
{
    return true;
}

bool od_claim_app_storage_purge(void)
{
    return od_tlsr_store_purge_all() == 0;
}

uint8_t *od_claim_app_workspace(uint32_t *cap)
{
    return od_tlsr_config_workspace(cap);
}

bool od_claim_app_workspace_busy(void)
{
    return od_tlsr_config_assembling();
}

void od_claim_app_config_changed(void)
{
    od_tlsr_config_reload();
}

/* ---- od_nfc_app: no NFC front end on these tags ---- */

bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap)
{
    (void)type;
    (void)data;
    (void)len_io;
    (void)cap;
    return false;
}

bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    (void)type;
    (void)data;
    (void)len;
    return false;
}

/* ---- od_inflate_app: the portable engine ---- */

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
