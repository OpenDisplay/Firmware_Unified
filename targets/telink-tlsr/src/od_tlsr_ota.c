/* The updater state machine (od_tlsr_ota.h). Plain C over od_tlsr_flash.h, no SDK header, so
 * tests/host/tlsr_ota_test.c runs it against a simulated flash. */
#include "od_tlsr_ota.h"

#include "od_config_store.h"
#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"

#include <string.h>

#if OD_TLSR_IMAGE_MAX > OD_TLSR_MULTI_BOOT_B
#error "an image past 0x40000 needs the slot check for OD_TLSR_MULTI_BOOT_B too"
#endif

/* Delay between acknowledging INSTALL and starting it, so the notification leaves the radio. */
#define INSTALL_DELAY_MS 1500u
#define VERIFY_STEP      0x1000u

static struct {
    enum od_tlsr_ota_state state;
    enum od_tlsr_ota_err   err;
    uint32_t len;
    uint32_t next;        /* ERASING: next sector offset; RECEIVING: next byte; VERIFYING: next byte */
    uint32_t crc;
    uint8_t  flag;        /* the held-back byte at OD_TLSR_HDR_FLAG_OFF */
    bool     install_due;
    uint32_t install_at;
} s;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t reply(uint8_t out[OD_TLSR_OTA_REPLY_MAX], uint8_t op, uint8_t status, uint32_t arg)
{
    out[0] = (uint8_t)(op | 0x80u);
    out[1] = status;
    wr32(&out[2], arg);
    return 6u;
}

static void fail(enum od_tlsr_ota_err e)
{
    s.state = OD_TLSR_OTA_FAILED;
    s.err = e;
}

void od_tlsr_ota_reset(void)
{
    memset(&s, 0, sizeof s);
}

enum od_tlsr_ota_state od_tlsr_ota_state(void)
{
    return s.state;
}

static bool s_unlocked;

void od_tlsr_ota_unlock(void)
{
    s_unlocked = true;
}

void od_tlsr_ota_link_closed(void)
{
    s_unlocked = false;
    if (s.state != OD_TLSR_OTA_VERIFIED && s.state != OD_TLSR_OTA_INSTALLING) {
        od_tlsr_ota_reset();
    }
}

static uint8_t on_begin(const uint8_t *d, uint16_t n, uint8_t *out)
{
    uint32_t len;

    if (n != 5u) {
        return reply(out, 0x01u, OD_TLSR_OTA_E_LENGTH, 0u);
    }
    if (s.state == OD_TLSR_OTA_INSTALLING) {
        return reply(out, 0x01u, OD_TLSR_OTA_E_STATE, 0u);
    }
    len = rd32(&d[1]);
    if (len < OD_TLSR_IMAGE_MIN || len > OD_TLSR_IMAGE_MAX) {
        od_tlsr_ota_reset();
        return reply(out, 0x01u, OD_TLSR_OTA_E_LENGTH, len);
    }
    od_tlsr_ota_reset();
    if (!od_tlsr_flash_supply_ok()) {
        return reply(out, 0x01u, OD_TLSR_OTA_E_SUPPLY, len);
    }
    s.state = OD_TLSR_OTA_ERASING;
    s.len = len;
    s.next = 0u;
    return 0u; /* the reply follows when the erase completes */
}

static uint8_t on_data(const uint8_t *d, uint16_t n, uint8_t *out)
{
    uint8_t buf[256];
    uint32_t off, cnt;

    (void)out;
    if (s.state != OD_TLSR_OTA_RECEIVING) {
        if (s.state != OD_TLSR_OTA_FAILED) {
            fail(OD_TLSR_OTA_E_STATE);
        }
        return 0u;
    }
    if (n < 5u || (uint32_t)(n - 5u) > sizeof buf) {
        fail(OD_TLSR_OTA_E_LENGTH);
        return 0u;
    }
    off = rd32(&d[1]);
    cnt = (uint32_t)(n - 5u);
    if (off != s.next || cnt == 0u || cnt > s.len - s.next) {
        fail(OD_TLSR_OTA_E_OFFSET);
        return 0u;
    }
    if (!od_tlsr_flash_supply_ok()) {
        fail(OD_TLSR_OTA_E_SUPPLY);
        return 0u;
    }
    memcpy(buf, &d[5], cnt);
    if (off <= OD_TLSR_HDR_FLAG_OFF && OD_TLSR_HDR_FLAG_OFF < off + cnt) {
        s.flag = buf[OD_TLSR_HDR_FLAG_OFF - off];
        buf[OD_TLSR_HDR_FLAG_OFF - off] = 0xFFu;
    }
    if (!od_tlsr_flash_program(OD_TLSR_STAGE_BASE + off, buf, cnt)) {
        fail(OD_TLSR_OTA_E_FLASH);
        return 0u;
    }
    s.next += cnt;
    return 0u;
}

/* Header checks that need no full pass: boot flag, "NLT", the length field, and no boot flag
 * at a multi-boot slot inside the image (od_tlsr_layout.h). */
static bool header_ok(void)
{
    uint8_t h[0x1Cu];
    uint8_t slot;

    od_tlsr_flash_read(OD_TLSR_STAGE_BASE, h, sizeof h);
    h[OD_TLSR_HDR_FLAG_OFF] = s.flag;
    if (s.len > OD_TLSR_MULTI_BOOT_A + OD_TLSR_HDR_FLAG_OFF) {
        od_tlsr_flash_read(OD_TLSR_STAGE_BASE + OD_TLSR_MULTI_BOOT_A + OD_TLSR_HDR_FLAG_OFF, &slot, 1u);
        if (slot == OD_TLSR_HDR_FLAG) {
            return false;
        }
    }
    return h[8] == OD_TLSR_HDR_FLAG && h[9] == 'N' && h[10] == 'L' && h[11] == 'T' &&
           rd32(&h[OD_TLSR_HDR_SIZE_OFF]) == s.len;
}

static uint8_t on_finish(uint16_t n, uint8_t *out)
{
    if (n != 1u) {
        return reply(out, 0x03u, OD_TLSR_OTA_E_LENGTH, 0u);
    }
    if (s.state == OD_TLSR_OTA_FAILED) {
        return reply(out, 0x03u, (uint8_t)s.err, s.next);
    }
    if (s.state != OD_TLSR_OTA_RECEIVING) {
        return reply(out, 0x03u, OD_TLSR_OTA_E_STATE, 0u);
    }
    if (s.next != s.len) {
        fail(OD_TLSR_OTA_E_OFFSET);
        return reply(out, 0x03u, OD_TLSR_OTA_E_OFFSET, s.next);
    }
    if (!header_ok()) {
        fail(OD_TLSR_OTA_E_HEADER);
        return reply(out, 0x03u, OD_TLSR_OTA_E_HEADER, 0u);
    }
    s.state = OD_TLSR_OTA_VERIFYING;
    s.next = 0u;
    s.crc = 0u;
    return 0u; /* the reply follows when the CRC pass completes */
}

uint8_t od_tlsr_ota_write(const uint8_t *data, uint16_t len, uint8_t out[OD_TLSR_OTA_REPLY_MAX])
{
    if (data == NULL || len == 0u) {
        return 0u;
    }
    if (data[0] != 0x05u && !s_unlocked && od_tlsr_ota_app_needs_unlock()) {
        return reply(out, data[0] & 0x7Fu, OD_TLSR_OTA_E_LOCKED, 0u);
    }
    switch (data[0]) {
    case 0x01u:
        return on_begin(data, len, out);
    case 0x02u:
        return on_data(data, len, out);
    case 0x03u:
        return on_finish(len, out);
    case 0x04u:
        if (s.state != OD_TLSR_OTA_VERIFIED) {
            return reply(out, 0x04u, OD_TLSR_OTA_E_STATE, 0u);
        }
        if (!od_tlsr_flash_supply_ok()) {
            return reply(out, 0x04u, OD_TLSR_OTA_E_SUPPLY, 0u);
        }
        s.state = OD_TLSR_OTA_INSTALLING;
        s.install_due = false; /* armed by the next service() call, which knows the time */
        return reply(out, 0x04u, OD_TLSR_OTA_OK, s.len);
    case 0x05u:
        out[0] = 0x85u;
        out[1] = (uint8_t)s.state;
        wr32(&out[2], s.next);
        wr32(&out[6], s.len);
        out[10] = (uint8_t)s.err;
        od_tlsr_ota_app_boot_info(&out[11]);
        return 13u;
    case 0x06u:
        if (s.state == OD_TLSR_OTA_INSTALLING) {
            return reply(out, 0x06u, OD_TLSR_OTA_E_STATE, 0u);
        }
        od_tlsr_ota_reset();
        return reply(out, 0x06u, OD_TLSR_OTA_OK, 0u);
    default:
        return reply(out, data[0] & 0x7Fu, OD_TLSR_OTA_E_OPCODE, 0u);
    }
}

static void verify_step(void)
{
    uint8_t buf[64];
    uint32_t body = s.len - 4u;
    uint32_t end = s.next + VERIFY_STEP < body ? s.next + VERIFY_STEP : body;
    uint8_t out[OD_TLSR_OTA_REPLY_MAX];

    while (s.next < end) {
        uint32_t n = end - s.next < sizeof buf ? end - s.next : (uint32_t)sizeof buf;
        od_tlsr_flash_read(OD_TLSR_STAGE_BASE + s.next, buf, n);
        if (s.next <= OD_TLSR_HDR_FLAG_OFF && OD_TLSR_HDR_FLAG_OFF < s.next + n) {
            buf[OD_TLSR_HDR_FLAG_OFF - s.next] = s.flag;
        }
        s.crc = od_config_store_crc32_update(s.crc, buf, n);
        s.next += n;
    }
    if (s.next < body) {
        return;
    }
    od_tlsr_flash_read(OD_TLSR_STAGE_BASE + body, buf, 4u);
    /* Telink's image CRC is the CRC-32 without its final inversion. */
    if (rd32(buf) != (s.crc ^ 0xFFFFFFFFu)) {
        fail(OD_TLSR_OTA_E_CRC);
        od_tlsr_ota_app_notify(out, reply(out, 0x03u, OD_TLSR_OTA_E_CRC, 0u));
        return;
    }
    if (!od_tlsr_flash_supply_ok()) {
        fail(OD_TLSR_OTA_E_SUPPLY);
        od_tlsr_ota_app_notify(out, reply(out, 0x03u, OD_TLSR_OTA_E_SUPPLY, 0u));
        return;
    }
    if (!od_tlsr_flash_program(OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF, &s.flag, 1u)) {
        fail(OD_TLSR_OTA_E_FLASH);
        od_tlsr_ota_app_notify(out, reply(out, 0x03u, OD_TLSR_OTA_E_FLASH, 0u));
        return;
    }
    od_tlsr_flash_read(OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF, buf, 1u);
    if (buf[0] != s.flag) {
        fail(OD_TLSR_OTA_E_FLASH);
        od_tlsr_ota_app_notify(out, reply(out, 0x03u, OD_TLSR_OTA_E_FLASH, 0u));
        return;
    }
    s.state = OD_TLSR_OTA_VERIFIED;
    s.next = s.len;
    od_tlsr_ota_app_notify(out, reply(out, 0x03u, OD_TLSR_OTA_OK, s.len));
}

void od_tlsr_ota_service(uint32_t now_ms)
{
    uint8_t out[OD_TLSR_OTA_REPLY_MAX];

    switch (s.state) {
    case OD_TLSR_OTA_ERASING:
        if (!od_tlsr_flash_supply_ok()) {
            fail(OD_TLSR_OTA_E_SUPPLY);
            od_tlsr_ota_app_notify(out, reply(out, 0x01u, OD_TLSR_OTA_E_SUPPLY, s.next));
            return;
        }
        if (!od_tlsr_flash_erase(OD_TLSR_STAGE_BASE + s.next)) {
            fail(OD_TLSR_OTA_E_FLASH);
            od_tlsr_ota_app_notify(out, reply(out, 0x01u, OD_TLSR_OTA_E_FLASH, s.next));
            return;
        }
        s.next += OD_TLSR_SECTOR;
        if (s.next >= s.len) {
            s.state = OD_TLSR_OTA_RECEIVING;
            s.next = 0u;
            s.flag = 0xFFu;
            od_tlsr_ota_app_notify(out, reply(out, 0x01u, OD_TLSR_OTA_OK, s.len));
        }
        return;
    case OD_TLSR_OTA_VERIFYING:
        verify_step();
        return;
    case OD_TLSR_OTA_INSTALLING:
        if (!s.install_due) {
            s.install_due = true;
            s.install_at = now_ms + INSTALL_DELAY_MS;
            return;
        }
        if ((int32_t)(now_ms - s.install_at) >= 0) {
            /* The last chance to refuse: after this the running image is erased. */
            if (!od_tlsr_flash_supply_ok()) {
                s.state = OD_TLSR_OTA_VERIFIED;
                s.install_due = false;
                od_tlsr_ota_app_notify(out, reply(out, 0x04u, OD_TLSR_OTA_E_SUPPLY, 0u));
                return;
            }
            od_tlsr_ota_app_before_install();
            od_tlsr_install(s.len);
        }
        return;
    default:
        return;
    }
}
