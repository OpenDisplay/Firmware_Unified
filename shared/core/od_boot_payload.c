#include "od_boot_payload.h"

#include "opendisplay_structs.h"

#include <string.h>

bool od_boot_key_is_zero(const uint8_t key[OD_BOOT_KEY_SIZE])
{
    size_t i;

    if (key == NULL) {
        return true;
    }
    for (i = 0u; i < OD_BOOT_KEY_SIZE; ++i) {
        if (key[i] != 0u) {
            return false;
        }
    }
    return true;
}

enum od_boot_key_state od_boot_key_policy(bool claimed, bool locked, uint8_t sec_flags)
{
    if (locked) {
        return OD_BOOT_KEY_HIDDEN;
    }
    if (!claimed) {
        return OD_BOOT_KEY_NOT_SET;
    }
    return (sec_flags & OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN) != 0u ? OD_BOOT_KEY_SHOWN
                                                                   : OD_BOOT_KEY_HIDDEN;
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    for (i = 0u; i < len; ++i) {
        out[i * 2u] = hex[(in[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = hex[in[i] & 0x0Fu];
    }
    out[len * 2u] = '\0';
}

/* Appends src at *pos while it fits; false once it does not. */
static bool put_str(char *out, size_t out_size, size_t *pos, const char *src)
{
    const size_t n = strlen(src);

    if (*pos + n >= out_size) {
        return false;
    }
    memcpy(out + *pos, src, n + 1u);
    *pos += n;
    return true;
}

static bool put_dec(char *out, size_t out_size, size_t *pos, uint32_t v)
{
    char tmp[11];
    size_t i = sizeof tmp - 1u;

    tmp[i] = '\0';
    do {
        tmp[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    return put_str(out, out_size, pos, &tmp[i]);
}

static void hex6(uint32_t v, char out[7])
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned i;

    for (i = 0u; i < 6u; ++i) {
        out[5u - i] = hex[(v >> (4u * i)) & 0x0Fu];
    }
    out[6] = '\0';
}

void od_boot_format_key_display(enum od_boot_key_state st, const uint8_t key[OD_BOOT_KEY_SIZE],
                                char out[OD_BOOT_KEY_HEX_SIZE])
{
    if (out == NULL) {
        return;
    }
    if (st == OD_BOOT_KEY_SHOWN && key != NULL) {
        bytes_to_hex(key, OD_BOOT_KEY_SIZE, out);
        return;
    }
    memset(out, st == OD_BOOT_KEY_NOT_SET ? '-' : 'X', OD_BOOT_KEY_HEX_SIZE - 1u);
    out[OD_BOOT_KEY_HEX_SIZE - 1u] = '\0';
}

void od_boot_format_key_line(const char *label, enum od_boot_key_state st,
                             const uint8_t *key_part, size_t key_part_len,
                             char *out, size_t out_size)
{
    char hex[OD_BOOT_KEY_HEX_SIZE + 1u];
    size_t pos = 0u;
    const char *tail;

    if (out == NULL || out_size == 0u) {
        return;
    }
    out[0] = '\0';
    if (st == OD_BOOT_KEY_SHOWN) {
        if (key_part == NULL || key_part_len > OD_BOOT_KEY_SIZE) {
            return;
        }
        hex[0] = ' ';
        bytes_to_hex(key_part, key_part_len, hex + 1);
        tail = hex;
    } else {
        tail = st == OD_BOOT_KEY_HIDDEN ? " hidden" : " not set";
    }
    if (!put_str(out, out_size, &pos, label != NULL ? label : "") ||
        !put_str(out, out_size, &pos, tail)) {
        out[0] = '\0';
    }
}

void od_boot_payload_build(uint16_t legacy_tag_type, uint32_t device_id24,
                           const uint8_t key[OD_BOOT_KEY_SIZE], uint16_t manufacturer_id,
                           uint8_t out[OD_BOOT_PAYLOAD_SIZE])
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, OD_BOOT_PAYLOAD_SIZE);
    out[0] = (uint8_t)(legacy_tag_type >> 8);
    out[1] = (uint8_t)legacy_tag_type;
    out[2] = (uint8_t)(device_id24 >> 16);
    out[3] = (uint8_t)(device_id24 >> 8);
    out[4] = (uint8_t)device_id24;
    if (key != NULL) {
        memcpy(&out[5], key, OD_BOOT_KEY_SIZE);
    }
    out[21] = (uint8_t)(manufacturer_id >> 8);
    out[22] = (uint8_t)manufacturer_id;
}

size_t od_boot_base64url_encode(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0u;
    size_t out_len = 0u;

    if (data == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }
    while (i + 3u <= len) {
        uint32_t value = ((uint32_t)data[i] << 16) |
                         ((uint32_t)data[i + 1u] << 8) |
                         data[i + 2u];
        if (out_len + 4u >= out_size) {
            return 0u;
        }
        i += 3u;
        out[out_len++] = table[(value >> 18) & 63u];
        out[out_len++] = table[(value >> 12) & 63u];
        out[out_len++] = table[(value >> 6) & 63u];
        out[out_len++] = table[value & 63u];
    }
    if (len - i == 1u) {
        uint32_t value = (uint32_t)data[i] << 16;
        if (out_len + 2u >= out_size) {
            return 0u;
        }
        out[out_len++] = table[(value >> 18) & 63u];
        out[out_len++] = table[(value >> 12) & 63u];
    } else if (len - i == 2u) {
        uint32_t value = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1u] << 8);
        if (out_len + 3u >= out_size) {
            return 0u;
        }
        out[out_len++] = table[(value >> 18) & 63u];
        out[out_len++] = table[(value >> 12) & 63u];
        out[out_len++] = table[(value >> 6) & 63u];
    }
    if (out_len >= out_size) {
        return 0u;
    }
    out[out_len] = '\0';
    return out_len;
}

bool od_boot_url_build(uint16_t legacy_tag_type, uint32_t device_id24,
                       const uint8_t key[OD_BOOT_KEY_SIZE], uint16_t manufacturer_id,
                       char *out, size_t out_size)
{
    static const char prefix[] = "https://opendisplay.org/l/?";
    uint8_t payload[OD_BOOT_PAYLOAD_SIZE];
    char encoded[33];
    size_t enc_len;

    if (out == NULL || out_size == 0u) {
        return false;
    }
    od_boot_payload_build(legacy_tag_type, device_id24, key, manufacturer_id, payload);
    if (od_boot_base64url_encode(payload, sizeof(payload), encoded, sizeof(encoded)) == 0u) {
        out[0] = '\0';
        memset(payload, 0, sizeof payload);
        return false;
    }
    memset(payload, 0, sizeof payload);   /* it may hold K_dev */
    /* Concatenated by hand rather than with snprintf: on EFR32BG22 this is the last caller of
     * the libc formatter once its diagnostics are compiled out. */
    enc_len = strlen(encoded);
    if (sizeof(prefix) - 1u + enc_len >= out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, prefix, sizeof(prefix) - 1u);
    memcpy(out + sizeof(prefix) - 1u, encoded, enc_len + 1u);
    return true;
}

/* ------------------------------------------------- the setup screen family (7.8) --- */

bool od_setup_url_build(char kind, uint32_t device_id24, uint8_t target,
                        uint8_t fw_major, uint8_t fw_minor, uint8_t fw_patch,
                        uint16_t driver_index, uint16_t display_index, uint16_t power_index,
                        char *out, size_t out_size)
{
    char id[7];
    size_t pos = 0u;
    bool ok;

    if (out == NULL || out_size == 0u) {
        return false;
    }
    out[0] = '\0';
    if (kind != 's' && kind != 'r') {
        return false;
    }
    hex6(device_id24 & 0xFFFFFFu, id);
    /* SECURITY_V2 7.8.1 format 1: f first, then the core k and i, then the rest in fixed order. */
    ok = put_str(out, out_size, &pos, "https://opendisplay.org/l/?f=1&k=") &&
         put_str(out, out_size, &pos, kind == 's' ? "1" : "2") &&
         put_str(out, out_size, &pos, "&i=") && put_str(out, out_size, &pos, id);
    if (ok && target >= 1u && target <= OD_SETUP_URL_TARGET_MAX) {
        ok = put_str(out, out_size, &pos, "&t=") && put_dec(out, out_size, &pos, target);
    }
    ok = ok &&
         put_str(out, out_size, &pos, "&v=") && put_dec(out, out_size, &pos, fw_major) &&
         put_str(out, out_size, &pos, ".") && put_dec(out, out_size, &pos, fw_minor) &&
         put_str(out, out_size, &pos, ".") && put_dec(out, out_size, &pos, fw_patch);
    if (ok && driver_index != 0u && driver_index <= OD_SETUP_URL_INDEX_MAX) {
        ok = put_str(out, out_size, &pos, "&b=") && put_dec(out, out_size, &pos, driver_index);
    }
    if (ok && display_index != 0u && display_index <= OD_SETUP_URL_INDEX_MAX) {
        ok = put_str(out, out_size, &pos, "&d=") && put_dec(out, out_size, &pos, display_index);
    }
    if (ok && power_index != 0u && power_index <= OD_SETUP_URL_INDEX_MAX) {
        ok = put_str(out, out_size, &pos, "&p=") && put_dec(out, out_size, &pos, power_index);
    }
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

bool od_setup_content_build(enum od_setup_screen kind, bool full, uint32_t code,
                            uint32_t device_id24, uint8_t target,
                            uint8_t fw_major, uint8_t fw_minor, uint8_t fw_patch,
                            uint16_t driver_index, uint16_t display_index, uint16_t power_index,
                            struct od_setup_content *out)
{
    char id[7];
    size_t pos = 0u;
    uint8_t n = 0u;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->big = OD_SETUP_NO_BIG;
    if (kind != OD_SETUP_SCREEN_SETUP && kind != OD_SETUP_SCREEN_CODE &&
        kind != OD_SETUP_SCREEN_RESET) {
        return false;
    }
    hex6(device_id24 & 0xFFFFFFu, id);
    (void)put_str(out->id_line, sizeof out->id_line, &pos, full ? "ID:  OD" : "OD");
    (void)put_str(out->id_line, sizeof out->id_line, &pos, id);

    if (kind == OD_SETUP_SCREEN_CODE) {
        unsigned i;

        code %= 1000000u;
        for (i = 0u; i < 7u; ++i) {
            out->code_line[i] = ' ';
        }
        for (i = 0u; i < 6u; ++i) {
            const unsigned at = i < 3u ? 6u - i : 2u - (i - 3u);
            out->code_line[at] = (char)('0' + (code % 10u));
            code /= 10u;
        }
        out->code_line[7] = '\0';
        if (full) {
            out->line[n++] = "OpenDisplay";
            out->line[n++] = "CLAIM CODE";
            out->big = n;
            out->line[n++] = out->code_line;
            out->line[n++] = out->id_line;
            out->line[n++] = "Type this code in your app.";
            out->line[n++] = "Did not start a claim? Ignore.";
        } else {
            out->line[n++] = "CODE";
            out->big = n;
            out->line[n++] = out->code_line;
            out->line[n++] = out->id_line;
        }
        out->n = n;
        return true;
    }

    if (!od_setup_url_build(kind == OD_SETUP_SCREEN_SETUP ? 's' : 'r', device_id24, target,
                            fw_major, fw_minor, fw_patch, driver_index, display_index,
                            power_index, out->url, sizeof out->url)) {
        return false;
    }
    out->qr = true;
    if (kind == OD_SETUP_SCREEN_SETUP) {
        if (full) {
            out->line[n++] = "OpenDisplay";
            out->line[n++] = "SETUP MODE";
            out->line[n++] = out->id_line;
            out->line[n++] = "Open the OpenDisplay app,";
            out->line[n++] = "or scan:";
            out->line[n++] = "Closes after 10 minutes";
        } else {
            out->line[n++] = "SETUP";
            out->line[n++] = out->id_line;
        }
    } else {
        if (full) {
            out->line[n++] = "OpenDisplay";
            out->line[n++] = "FACTORY RESET";
            out->line[n++] = "Settings and key erased";
            out->line[n++] = out->id_line;
            out->line[n++] = "Scan to set up again";
        } else {
            out->line[n++] = "RESET";
            out->line[n++] = out->id_line;
        }
    }
    out->n = n;
    return true;
}
