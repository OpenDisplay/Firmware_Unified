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
#define OD_BOOT_KEY_HEX_SIZE 33u
#define OD_BOOT_URL_SIZE 64u

bool od_boot_key_is_zero(const uint8_t key[OD_BOOT_KEY_SIZE]);

/* What the NORMAL boot screen says about the key, as one decision (SECURITY_V2 10.7). Every
 * renderer asks this rather than re-deriving it: the panel layouts differ, the policy does not.
 *   unclaimed                                  -> NOT_SET   ("not set", "----", QR key zero)
 *   claimed, SHOW_KEY_ON_SCREEN clear (default) -> HIDDEN    ("hidden", "XXXX", QR key zero)
 *   claimed, SHOW_KEY_ON_SCREEN set            -> SHOWN     (K_dev as hex, and in the QR)
 *   LOCKED                                     -> HIDDEN    (the key is unreadable)
 * The setup screen family never asks: it shows no key at all. */
enum od_boot_key_state {
    OD_BOOT_KEY_NOT_SET = 0,
    OD_BOOT_KEY_HIDDEN,
    OD_BOOT_KEY_SHOWN
};

/* The pure policy above; `sec_flags` is 0x27 SecurityConfig.flags. */
enum od_boot_key_state od_boot_key_policy(bool claimed, bool locked, uint8_t sec_flags);

/* The live answer for the renderers (od_boot_key.c, APP_SESSION tier): the policy over the
 * security state and the parsed 0x27, and K_dev into key[] when SHOWN (zeros otherwise). A
 * SHOWN whose key cannot be read is reported HIDDEN. */
enum od_boot_key_state od_boot_screen_key(uint8_t key[OD_BOOT_KEY_SIZE]);

/* The small-layout key text, 32 characters: "----..." NOT_SET, "XXXX..." HIDDEN, hex SHOWN. */
void od_boot_format_key_display(enum od_boot_key_state st, const uint8_t key[OD_BOOT_KEY_SIZE],
                                char out[OD_BOOT_KEY_HEX_SIZE]);

/* "<label> not set", "<label> hidden" or "<label> <hex of key_part>" (upper-case hex, the 2.x
 * zone-layout KEY1/KEY2 lines). Hand-built (no snprintf: BG22). */
void od_boot_format_key_line(const char *label, enum od_boot_key_state st,
                             const uint8_t *key_part, size_t key_part_len,
                             char *out, size_t out_size);

/* The 23-byte /l/ payload (2.x layout): tag type BE, device id 3 B, key 16 B, manufacturer BE.
 * `key` NULL means zeros; a renderer passes K_dev only when the state is SHOWN. */
void od_boot_payload_build(uint16_t legacy_tag_type, uint32_t device_id24,
                           const uint8_t key[OD_BOOT_KEY_SIZE], uint16_t manufacturer_id,
                           uint8_t out[OD_BOOT_PAYLOAD_SIZE]);

size_t od_boot_base64url_encode(const uint8_t *data, size_t len, char *out, size_t out_size);

bool od_boot_url_build(uint16_t legacy_tag_type, uint32_t device_id24,
                       const uint8_t key[OD_BOOT_KEY_SIZE], uint16_t manufacturer_id,
                       char *out, size_t out_size);

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
