#include "od_boot_payload.h"

#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>

static void od_boot_bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    if (out == NULL || out_size == 0u) {
        return;
    }
    if (in == NULL || len > (SIZE_MAX - 1u) / 2u || out_size < len * 2u + 1u) {
        out[0] = '\0';
        return;
    }
    for (i = 0u; i < len; ++i) {
        out[i * 2u] = hex[(in[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = hex[in[i] & 0x0fu];
    }
    out[len * 2u] = '\0';
}

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

enum od_boot_key_state od_boot_key_state(const struct SecurityConfig *sec)
{
    if (sec == NULL || sec->encryption_enabled == 0u ||
        od_boot_key_is_zero(sec->encryption_key)) {
        return OD_BOOT_KEY_NOT_SET;
    }
    return (sec->flags & OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN) != 0u
        ? OD_BOOT_KEY_SHOWN : OD_BOOT_KEY_HIDDEN;
}

void od_boot_format_key_display(const uint8_t key[OD_BOOT_KEY_SIZE], bool show_key,
                                char out[OD_BOOT_KEY_HEX_SIZE])
{
    if (out == NULL) {
        return;
    }
    if (od_boot_key_is_zero(key)) {
        memset(out, '-', OD_BOOT_KEY_HEX_SIZE - 1u);
        out[OD_BOOT_KEY_HEX_SIZE - 1u] = '\0';
    } else if (!show_key) {
        memset(out, 'X', OD_BOOT_KEY_HEX_SIZE - 1u);
        out[OD_BOOT_KEY_HEX_SIZE - 1u] = '\0';
    } else {
        od_boot_bytes_to_hex(key, OD_BOOT_KEY_SIZE, out, OD_BOOT_KEY_HEX_SIZE);
    }
}

void od_boot_format_key_line(const char *label, const uint8_t *key_part, size_t key_part_len,
                             bool key_is_zero, bool show_key, char *out, size_t out_size)
{
    char hex[OD_BOOT_KEY_HEX_SIZE];

    if (out == NULL || out_size == 0u) {
        return;
    }
    if (label == NULL) {
        label = "";
    }
    if (key_is_zero) {
        (void)snprintf(out, out_size, "%s not set", label);
    } else if (!show_key) {
        (void)snprintf(out, out_size, "%s hidden", label);
    } else {
        od_boot_bytes_to_hex(key_part, key_part_len, hex, sizeof(hex));
        (void)snprintf(out, out_size, "%s %s", label, hex);
    }
}

void od_boot_payload_build(uint16_t legacy_tag_type, uint32_t device_id24,
                           const uint8_t key[OD_BOOT_KEY_SIZE], bool show_key,
                           uint16_t manufacturer_id,
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
    if (show_key && key != NULL) {
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
                       const uint8_t key[OD_BOOT_KEY_SIZE], bool show_key,
                       uint16_t manufacturer_id, char *out, size_t out_size)
{
    static const char prefix[] = "https://opendisplay.org/l/?";
    uint8_t payload[OD_BOOT_PAYLOAD_SIZE];
    char encoded[33];
    size_t enc_len;

    if (out == NULL || out_size == 0u) {
        return false;
    }
    od_boot_payload_build(legacy_tag_type, device_id24, key, show_key, manufacturer_id, payload);
    if (od_boot_base64url_encode(payload, sizeof(payload), encoded, sizeof(encoded)) == 0u) {
        out[0] = '\0';
        return false;
    }
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
