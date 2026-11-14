/* od_hal_time, od_hal_nvs and od_hal_radio over tlsr_port.h. */

#include "od_hal_nvs.h"
#include "od_hal_radio.h"
#include "od_hal_time.h"

#include "od_config_store.h"
#include "od_txq.h"
#include "tlsr_port.h"

#include <stddef.h>

/* ------------------------------------------------------------------------------ time --- */

/* The 16 MHz tick counter is 32 bits and wraps every ~268 s. Every call folds the elapsed ticks
 * into a millisecond count, so correctness needs one call per wrap; od_tlsr_poll() makes one on
 * every main-loop pass and the stack wakes at least once per advertising interval. */
static uint32_t s_last_tick;
static uint32_t s_ms;
static uint32_t s_rem_ticks;

uint32_t od_hal_uptime_ms(void)
{
    uint32_t now = tlsr_port_ticks();
    uint32_t elapsed = (now - s_last_tick) + s_rem_ticks;

    s_last_tick = now;
    s_ms += elapsed / TLSR_PORT_TICKS_PER_MS;
    s_rem_ticks = elapsed % TLSR_PORT_TICKS_PER_MS;
    return s_ms;
}

void od_hal_delay_us(uint32_t us)
{
    tlsr_port_delay_us(us);
}

/* ------------------------------------------------------------------------------- nvs --- */

/* Two 4 KB sectors at 0x7A000, clear of every SDK-owned sector on a 512 KB part (SMP 0x74000,
 * MAC 0x76000, calibration 0x77000, central pairing 0x78000) and of ATC's tag-type block at
 * 0x79000, which is kept so the tag can be moved back to ATC firmware.
 * Layout: u32 record length (erased = 0xFFFFFFFF = no record), then the record. */
#define NVS_BASE      0x7A000u
#define NVS_SECTORS   2u
#define NVS_CAP       (NVS_SECTORS * 4096u - 4u)

typedef char nvs_fits_record[(OD_CONFIG_STORE_MAX_RECORD <= NVS_CAP) ? 1 : -1];

int od_hal_nvs_init(void)
{
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_size(uint32_t *len_out)
{
    uint32_t len;

    if (len_out == NULL) {
        return OD_HAL_NVS_EIO;
    }
    tlsr_port_flash_read(NVS_BASE, 4u, (uint8_t *)&len);
    if (len == 0xFFFFFFFFu || len == 0u) {
        return OD_HAL_NVS_ENOENT;
    }
    if (len > NVS_CAP) {
        return OD_HAL_NVS_EIO;
    }
    *len_out = len;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    uint32_t size;
    int rc = od_hal_nvs_size(&size);

    if (rc != OD_HAL_NVS_OK) {
        return rc;
    }
    if (buf == NULL || offset > size || len > size - offset) {
        return OD_HAL_NVS_E2BIG;
    }
    tlsr_port_flash_read(NVS_BASE + 4u + offset, len, (uint8_t *)buf);
    return OD_HAL_NVS_OK;
}

static void program(uint32_t addr, const uint8_t *p, uint32_t len)
{
    while (len != 0u) {
        uint32_t n = 256u - (addr & 0xFFu);          /* one program op never crosses a page */
        if (n > len) {
            n = len;
        }
        tlsr_port_flash_program(addr, n, p);
        addr += n;
        p += n;
        len -= n;
    }
}

int od_hal_nvs_erase(void)
{
    uint32_t i;
    for (i = 0; i < NVS_SECTORS; ++i) {
        tlsr_port_flash_erase_4k(NVS_BASE + i * 4096u);
    }
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    if (record == NULL || len == 0u || len > NVS_CAP) {
        return OD_HAL_NVS_E2BIG;
    }
    (void)od_hal_nvs_erase();
    program(NVS_BASE + 4u, (const uint8_t *)record, len);
    /* Length last: power lost mid-write leaves an erased length word, which reads as "no
     * record" rather than as a record with a torn body. od_config_store's CRC is the second
     * line of defence. */
    program(NVS_BASE, (const uint8_t *)&len, 4u);
    return OD_HAL_NVS_OK;
}

/* ----------------------------------------------------------------------------- radio --- */

uint32_t od_tlsr_link_tag(void);
bool od_tlsr_notify_subscribed(void);

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    return origin == OD_ORIGIN_BLE && tlsr_port_connected() && tag == od_tlsr_link_tag();
}

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    /* Malformed or wrong-origin frames concern this frame only: ERROR, never GONE, because GONE
     * makes od_txq drop every frame queued for the tag. There is no LAN transport here. */
    tlsr_port_crumb(7);
    if (frame == NULL || len == 0u || origin != OD_ORIGIN_BLE) {
        return OD_RADIO_ERROR;
    }
    if (!od_hal_radio_tag_is_live(origin, tag)) {
        return OD_RADIO_GONE;
    }
    /* Retrying while unsubscribed would park this entry forever: a CCC write is the only thing
     * that changes it, and nothing guarantees one arrives. */
    if (!od_tlsr_notify_subscribed()) {
        return OD_RADIO_ERROR;
    }
    switch (tlsr_port_notify(frame, len)) {
    case TLSR_PORT_NOTIFY_SENT:
        return OD_RADIO_SENT;
    case TLSR_PORT_NOTIFY_BUSY:
        return OD_RADIO_RETRY;
    default:
        return OD_RADIO_ERROR;
    }
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp;
    (void)len;
    (void)why;
}
