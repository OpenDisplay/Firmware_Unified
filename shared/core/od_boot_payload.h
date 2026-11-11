#ifndef OD_BOOT_PAYLOAD_H
#define OD_BOOT_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "od_claim_app.h"   /* enum od_setup_screen */

#ifdef __cplusplus
extern "C" {
#endif

#define OD_BOOT_PAYLOAD_SIZE 23u
#define OD_BOOT_KEY_SIZE 16u
#define OD_BOOT_URL_SIZE 64u

bool od_boot_key_is_zero(const uint8_t key[OD_BOOT_KEY_SIZE]);

/* What the boot screen may say about the key, as one decision. The panel never shows the key
 * and no QR carries it (SECURITY_V2 section 10.7): SHOW_KEY_ON_SCREEN is reserved and has no
 * effect.
 *   encryption_enabled == 0, or key all-zero / absent -> NOT_SET
 *   otherwise                                          -> HIDDEN */
enum od_boot_key_state {
    OD_BOOT_KEY_NOT_SET = 0,
    OD_BOOT_KEY_HIDDEN
};

struct SecurityConfig;

enum od_boot_key_state od_boot_key_state(const struct SecurityConfig *sec);

/* "<label> not set" or "<label> hidden". */
void od_boot_format_key_line(const char *label, bool key_set, char *out, size_t out_size);

/* The 23-byte /l/ payload. Bytes 5..20 (the 2.x key field) are always zero. */
void od_boot_payload_build(uint16_t legacy_tag_type, uint32_t device_id24,
                           uint16_t manufacturer_id,
                           uint8_t out[OD_BOOT_PAYLOAD_SIZE]);

size_t od_boot_base64url_encode(const uint8_t *data, size_t len, char *out, size_t out_size);

bool od_boot_url_build(uint16_t legacy_tag_type, uint32_t device_id24,
                       uint16_t manufacturer_id, char *out, size_t out_size);

/* ------------------------------------------------- the setup screen family (7.8) --- */

/* https://opendisplay.org/s/?k=<k>&i=<6 hex>&t=<target>&v=<a.b.c>[&b=][&d=][&p=]: worst case
 * 83 bytes, inside QR v6 ECC-M (106). */
#define OD_SETUP_URL_SIZE 96u

/* kind 's' (SETUP) or 'r' (RESET). b / d / p are omitted when 0. Hand-built (no snprintf: BG22).
 * false = kind invalid or out_size too small (out is then ""). */
bool od_setup_url_build(char kind, uint32_t device_id24, uint8_t target,
                        uint8_t fw_major, uint8_t fw_minor, uint8_t fw_patch,
                        uint16_t driver_index, uint16_t display_index, uint16_t power_index,
                        char *out, size_t out_size);

#define OD_SETUP_LINES_MAX 6u
#define OD_SETUP_NO_BIG    0xFFu

/* One screen of the family, as fixed firmware text: no config string ever appears (review A1).
 * The renderers only lay this out. */
struct od_setup_content {
    const char *line[OD_SETUP_LINES_MAX];   /* top to bottom */
    uint8_t     n;
    uint8_t     big;           /* index of the line drawn at the largest scale (CODE digits) */
    bool        qr;            /* url is shown as a QR, which wins over text when space runs out */
    char        id_line[16];   /* "ID:  OD1A2B3C" (full) or "OD1A2B3C" (reduced) */
    char        code_line[8];  /* "123 456" */
    char        url[OD_SETUP_URL_SIZE];
};

/* full = the >= 400x300 logical layout; otherwise the reduced one. code is 0..999999 (CODE only;
 * larger values are taken mod 10^6). The QR inputs are those of od_setup_url_build. false = kind
 * not SETUP / CODE / RESET, or the URL did not build. */
bool od_setup_content_build(enum od_setup_screen kind, bool full, uint32_t code,
                            uint32_t device_id24, uint8_t target,
                            uint8_t fw_major, uint8_t fw_minor, uint8_t fw_patch,
                            uint16_t driver_index, uint16_t display_index, uint16_t power_index,
                            struct od_setup_content *out);

#ifdef __cplusplus
}
#endif

#endif
