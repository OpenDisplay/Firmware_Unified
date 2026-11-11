#include "od_boot_payload.h"

#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

int main(void)
{
    static const uint8_t key[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    uint8_t zero[16] = {0};
    uint8_t payload[OD_BOOT_PAYLOAD_SIZE];
    char line[32];
    char url[OD_BOOT_URL_SIZE];

    /* The 2.x key field is always zero: no QR carries the key (SECURITY_V2 10.7). */
    od_boot_payload_build(0x1234u, 0xabcdefu, 0x7856u, payload);
    CHECK(payload[0] == 0x12u && payload[1] == 0x34u);
    CHECK(payload[2] == 0xabu && payload[3] == 0xcdu && payload[4] == 0xefu);
    CHECK(memcmp(&payload[5], zero, sizeof(zero)) == 0);
    CHECK(payload[21] == 0x78u && payload[22] == 0x56u);

    od_boot_format_key_line("KEY:", true, line, sizeof(line));
    CHECK(strcmp(line, "KEY: hidden") == 0);
    od_boot_format_key_line("KEY:", false, line, sizeof(line));
    CHECK(strcmp(line, "KEY: not set") == 0);
    od_boot_format_key_line("KEY:", false, line, 6u);
    CHECK(line[0] == '\0');

    /* The key-state tree, every input combination. SHOW_KEY_ON_SCREEN is reserved: it never
     * changes the answer. */
    {
        static const struct {
            uint8_t enabled;
            bool key_set;
            uint8_t flags;
            enum od_boot_key_state want;
        } cases[] = {
            {0u, false, 0u,                                   OD_BOOT_KEY_NOT_SET},
            {0u, false, OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {0u, true,  0u,                                   OD_BOOT_KEY_NOT_SET},
            {0u, true,  OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {1u, false, 0u,                                   OD_BOOT_KEY_NOT_SET},
            {1u, false, OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {1u, true,  0u,                                   OD_BOOT_KEY_HIDDEN},
            {1u, true,  OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_HIDDEN},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct SecurityConfig sec;
            memset(&sec, 0, sizeof(sec));
            sec.encryption_enabled = cases[i].enabled;
            sec.flags = cases[i].flags;
            if (cases[i].key_set) {
                memcpy(sec.encryption_key, key, sizeof(sec.encryption_key));
            }
            if (od_boot_key_state(&sec) != cases[i].want) {
                fprintf(stderr, "FAIL key_state case %u: enabled=%u key_set=%d flags=0x%02X\n",
                        i, cases[i].enabled, (int)cases[i].key_set, cases[i].flags);
                ++failures;
            }
        }
        /* An absent config is a device with nothing configured, not a crash. */
        CHECK(od_boot_key_state(NULL) == OD_BOOT_KEY_NOT_SET);
        /* One non-zero byte is a key. The zero test is over the whole 16, not a prefix. */
        {
            struct SecurityConfig sec;
            memset(&sec, 0, sizeof(sec));
            sec.encryption_enabled = 1u;
            sec.encryption_key[15] = 0x01u;
            CHECK(od_boot_key_state(&sec) == OD_BOOT_KEY_HIDDEN);
        }
    }

    CHECK(od_boot_url_build(0x1234u, 0xabcdefu, 0x7856u, url, sizeof(url)));
    CHECK(strcmp(url, "https://opendisplay.org/l/?EjSrze8AAAAAAAAAAAAAAAAAAAAAeFY") == 0);
    CHECK(!od_boot_url_build(0x1234u, 0xabcdefu, 0x7856u, line, 8u));

    /* The setup URL (SECURITY_V2 7.8): fixed parameter order, b/d/p omitted when 0. */
    {
        char su[OD_SETUP_URL_SIZE];

        CHECK(od_setup_url_build('s', 0x1A2B3Cu, 3u, 3u, 0u, 0u, 16u, 10u, 2u, su, sizeof su));
        CHECK(strcmp(su, "https://opendisplay.org/s/?k=s&i=1A2B3C&t=3&v=3.0.0&b=16&d=10&p=2") == 0);
        CHECK(strlen(su) == 65u);
        CHECK(od_setup_url_build('r', 0x1A2B3Cu, 1u, 3u, 0u, 0u, 0u, 0u, 0u, su, sizeof su));
        CHECK(strcmp(su, "https://opendisplay.org/s/?k=r&i=1A2B3C&t=1&v=3.0.0") == 0);
        CHECK(od_setup_url_build('s', 0x0000ABu, 2u, 1u, 2u, 3u, 0u, 7u, 0u, su, sizeof su));
        CHECK(strcmp(su, "https://opendisplay.org/s/?k=s&i=0000AB&t=2&v=1.2.3&d=7") == 0);
        /* the worst case is 83 bytes and fits; the id is masked to 24 bits */
        CHECK(od_setup_url_build('r', 0xFFFFFFFFu, 255u, 255u, 255u, 255u, 65535u, 65535u, 65535u,
                                 su, sizeof su));
        CHECK(strlen(su) == 83u);
        CHECK(strstr(su, "i=FFFFFF&") != NULL);
        CHECK(!od_setup_url_build('r', 1u, 255u, 255u, 255u, 255u, 65535u, 65535u, 65535u, su, 83u));
        CHECK(su[0] == '\0');
        CHECK(od_setup_url_build('r', 1u, 255u, 255u, 255u, 255u, 65535u, 65535u, 65535u, su, 84u));
        CHECK(!od_setup_url_build('x', 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, su, sizeof su));
    }

    /* The screen content: fixed firmware text, the id, the code. */
    {
        struct od_setup_content c;

        CHECK(od_setup_content_build(OD_SETUP_SCREEN_CODE, true, 123456u, 0x1A2B3Cu, 3u, 3u, 0u, 0u,
                                     0u, 0u, 0u, &c));
        CHECK(c.n == 6u && c.big == 2u && !c.qr);
        CHECK(strcmp(c.line[0], "OpenDisplay") == 0 && strcmp(c.line[1], "CLAIM CODE") == 0);
        CHECK(strcmp(c.line[2], "123 456") == 0 && strcmp(c.line[3], "ID:  OD1A2B3C") == 0);
        CHECK(od_setup_content_build(OD_SETUP_SCREEN_CODE, false, 7u, 0x1A2B3Cu, 3u, 3u, 0u, 0u,
                                     0u, 0u, 0u, &c));
        CHECK(c.n == 3u && c.big == 1u && strcmp(c.line[1], "000 007") == 0);
        CHECK(strcmp(c.line[0], "CODE") == 0 && strcmp(c.line[2], "OD1A2B3C") == 0);
        CHECK(od_setup_content_build(OD_SETUP_SCREEN_CODE, false, 1999999u, 1u, 3u, 3u, 0u, 0u,
                                     0u, 0u, 0u, &c));
        CHECK(strcmp(c.line[1], "999 999") == 0);

        CHECK(od_setup_content_build(OD_SETUP_SCREEN_SETUP, true, 0u, 0x1A2B3Cu, 3u, 3u, 0u, 0u,
                                     16u, 10u, 2u, &c));
        CHECK(c.qr && c.big == OD_SETUP_NO_BIG && c.n == 6u);
        CHECK(strcmp(c.line[1], "SETUP MODE") == 0);
        CHECK(strcmp(c.url, "https://opendisplay.org/s/?k=s&i=1A2B3C&t=3&v=3.0.0&b=16&d=10&p=2") == 0);
        CHECK(od_setup_content_build(OD_SETUP_SCREEN_RESET, false, 0u, 0x1A2B3Cu, 3u, 3u, 0u, 0u,
                                     0u, 0u, 0u, &c));
        CHECK(c.qr && c.n == 2u && strcmp(c.line[0], "RESET") == 0);
        CHECK(strncmp(c.url, "https://opendisplay.org/s/?k=r&", 31u) == 0);
        CHECK(od_setup_content_build(OD_SETUP_SCREEN_RESET, true, 0u, 1u, 3u, 3u, 0u, 0u,
                                     0u, 0u, 0u, &c));
        CHECK(c.n == 5u && strcmp(c.line[1], "FACTORY RESET") == 0);
        CHECK(!od_setup_content_build(OD_SETUP_SCREEN_NORMAL, true, 0u, 1u, 3u, 3u, 0u, 0u,
                                      0u, 0u, 0u, &c));
    }

    if (failures != 0) {
        return 1;
    }
    puts("boot_payload: ok");
    return 0;
}
