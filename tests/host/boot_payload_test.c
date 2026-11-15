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

    /* No key: the 2.x key field is zero (SECURITY_V2 10.7, every state but SHOWN). */
    od_boot_payload_build(0x1234u, 0xabcdefu, NULL, 0x7856u, payload);
    CHECK(payload[0] == 0x12u && payload[1] == 0x34u);
    CHECK(payload[2] == 0xabu && payload[3] == 0xcdu && payload[4] == 0xefu);
    CHECK(memcmp(&payload[5], zero, sizeof(zero)) == 0);
    CHECK(payload[21] == 0x78u && payload[22] == 0x56u);
    /* SHOWN: bytes 5..20 carry the key, as on 2.x. */
    od_boot_payload_build(0x1234u, 0xabcdefu, key, 0x7856u, payload);
    CHECK(memcmp(&payload[5], key, sizeof(key)) == 0);
    CHECK(payload[21] == 0x78u && payload[22] == 0x56u);

    /* The zone-layout key lines (2.x format). */
    od_boot_format_key_line("KEY1:", OD_BOOT_KEY_HIDDEN, key, 8u, line, sizeof(line));
    CHECK(strcmp(line, "KEY1: hidden") == 0);
    od_boot_format_key_line("KEY1:", OD_BOOT_KEY_NOT_SET, key, 8u, line, sizeof(line));
    CHECK(strcmp(line, "KEY1: not set") == 0);
    od_boot_format_key_line("KEY1:", OD_BOOT_KEY_SHOWN, key, 8u, line, sizeof(line));
    CHECK(strcmp(line, "KEY1: 0011223344556677") == 0);
    od_boot_format_key_line("KEY2:", OD_BOOT_KEY_SHOWN, key + 8, 8u, line, sizeof(line));
    CHECK(strcmp(line, "KEY2: 8899AABBCCDDEEFF") == 0);
    od_boot_format_key_line("KEY1:", OD_BOOT_KEY_SHOWN, key, 8u, line, 22u);   /* one short */
    CHECK(line[0] == '\0');
    od_boot_format_key_line("KEY1:", OD_BOOT_KEY_SHOWN, key, 8u, line, 23u);
    CHECK(strcmp(line, "KEY1: 0011223344556677") == 0);
    od_boot_format_key_line("KEY:", OD_BOOT_KEY_NOT_SET, NULL, 0u, line, 6u);
    CHECK(line[0] == '\0');
    od_boot_format_key_line("KEY1:", OD_BOOT_KEY_SHOWN, NULL, 8u, line, sizeof(line));
    CHECK(line[0] == '\0');

    /* The small-layout hex. */
    {
        char hex[OD_BOOT_KEY_HEX_SIZE];

        od_boot_format_key_display(OD_BOOT_KEY_NOT_SET, key, hex);
        CHECK(strcmp(hex, "--------------------------------") == 0);
        od_boot_format_key_display(OD_BOOT_KEY_HIDDEN, key, hex);
        CHECK(strcmp(hex, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX") == 0);
        od_boot_format_key_display(OD_BOOT_KEY_SHOWN, key, hex);
        CHECK(strcmp(hex, "00112233445566778899AABBCCDDEEFF") == 0);
    }

    /* The policy, every input combination (SECURITY_V2 10.7): only CLAIMED + bit 1 shows. */
    {
        static const struct {
            bool claimed, locked;
            uint8_t flags;
            enum od_boot_key_state want;
        } cases[] = {
            {false, false, 0u,                                   OD_BOOT_KEY_NOT_SET},
            {false, false, OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {false, false, 0xFFu,                                OD_BOOT_KEY_NOT_SET},
            {true,  false, 0u,                                   OD_BOOT_KEY_HIDDEN},
            {true,  false, (uint8_t)~OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN, OD_BOOT_KEY_HIDDEN},
            {true,  false, OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_SHOWN},
            {true,  false, 0xFFu,                                OD_BOOT_KEY_SHOWN},
            {false, true,  0u,                                   OD_BOOT_KEY_HIDDEN},
            {false, true,  OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_HIDDEN},
            {true,  true,  OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_HIDDEN},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            if (od_boot_key_policy(cases[i].claimed, cases[i].locked, cases[i].flags) != cases[i].want) {
                fprintf(stderr, "FAIL key_policy case %u\n", i);
                ++failures;
            }
        }
    }

    CHECK(od_boot_url_build(0x1234u, 0xabcdefu, NULL, 0x7856u, url, sizeof(url)));
    CHECK(strcmp(url, "https://opendisplay.org/l/?EjSrze8AAAAAAAAAAAAAAAAAAAAAeFY") == 0);
    CHECK(od_boot_url_build(0x1234u, 0xabcdefu, key, 0x7856u, url, sizeof(url)));
    CHECK(strcmp(url, "https://opendisplay.org/l/?EjSrze8AESIzRFVmd4iZqrvM3e7_eFY") == 0);
    CHECK(!od_boot_url_build(0x1234u, 0xabcdefu, key, 0x7856u, line, 8u));

    /* The setup URL (SECURITY_V2 7.8.1, format 1): vectors U1-U4, then the edges. */
    {
        static const struct {
            char kind; uint32_t id; uint8_t t, ma, mi, pa; uint16_t b, d, p;
            const char *url; size_t bytes;
        } U[] = {
            {'s', 0x1A2B3Cu, 3u, 3u, 0u, 0u, 16u, 10u, 2u,
             "https://opendisplay.org/l/?f=1&k=1&i=1A2B3C&t=3&v=3.0.0&b=16&d=10&p=2", 69u},
            {'r', 0x00ABCDu, 1u, 3u, 0u, 0u, 0u, 0u, 0u,
             "https://opendisplay.org/l/?f=1&k=2&i=00ABCD&t=1&v=3.0.0", 55u},
            {'r', 0xFFFFFFu, 99u, 255u, 255u, 255u, 9999u, 9999u, 9999u,
             "https://opendisplay.org/l/?f=1&k=2&i=FFFFFF&t=99&v=255.255.255&b=9999&d=9999&p=9999", 83u},
            {'s', 0x000001u, 5u, 3u, 1u, 12u, 10000u, 0u, 7u,
             "https://opendisplay.org/l/?f=1&k=1&i=000001&t=5&v=3.1.12&p=7", 60u},
        };
        char su[OD_SETUP_URL_SIZE];

        for (unsigned i = 0; i < sizeof U / sizeof U[0]; ++i) {
            const bool ok = od_setup_url_build(U[i].kind, U[i].id, U[i].t, U[i].ma, U[i].mi, U[i].pa,
                                               U[i].b, U[i].d, U[i].p, su, sizeof su);
            if (!ok || strcmp(su, U[i].url) != 0 || strlen(su) != U[i].bytes) {
                fprintf(stderr, "FAIL U%u: got \"%s\"\n", i + 1u, su);
                ++failures;
            }
        }
        /* U3 is the worst case: one byte less of room is refused, and leaves "" */
        CHECK(!od_setup_url_build('r', 0xFFFFFFu, 99u, 255u, 255u, 255u, 9999u, 9999u, 9999u, su,
                                  OD_SETUP_URL_WORST));
        CHECK(su[0] == '\0');
        CHECK(od_setup_url_build('r', 0xFFFFFFu, 99u, 255u, 255u, 255u, 9999u, 9999u, 9999u, su,
                                 OD_SETUP_URL_WORST + 1u));
        /* No input exceeds the worst case: out-of-range fields are omitted, the id is masked. */
        CHECK(od_setup_url_build('r', 0xFFFFFFFFu, 255u, 255u, 255u, 255u, 65535u, 65535u, 65535u,
                                 su, sizeof su));
        CHECK(strcmp(su, "https://opendisplay.org/l/?f=1&k=2&i=FFFFFF&v=255.255.255") == 0);
        CHECK(od_setup_url_build('s', 1u, 0u, 1u, 2u, 3u, 1u, 9999u, 10000u, su, sizeof su));
        CHECK(strcmp(su, "https://opendisplay.org/l/?f=1&k=1&i=000001&v=1.2.3&b=1&d=9999") == 0);
        CHECK(od_setup_url_build('s', 1u, 100u, 0u, 0u, 0u, 0u, 0u, 0u, su, sizeof su));
        CHECK(strcmp(su, "https://opendisplay.org/l/?f=1&k=1&i=000001&v=0.0.0") == 0);
        /* f is first, so the /l/ page can tell it from a legacy 31-character token */
        CHECK(strncmp(su, "https://opendisplay.org/l/?f=", 29u) == 0);
        CHECK(!od_setup_url_build('x', 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, su, sizeof su));
        CHECK(!od_setup_url_build('1', 1u, 1u, 1u, 1u, 1u, 0u, 0u, 0u, su, sizeof su));
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
        CHECK(strcmp(c.url, "https://opendisplay.org/l/?f=1&k=1&i=1A2B3C&t=3&v=3.0.0&b=16&d=10&p=2") == 0);
        CHECK(od_setup_content_build(OD_SETUP_SCREEN_RESET, false, 0u, 0x1A2B3Cu, 3u, 3u, 0u, 0u,
                                     0u, 0u, 0u, &c));
        CHECK(c.qr && c.n == 2u && strcmp(c.line[0], "RESET") == 0);
        CHECK(strncmp(c.url, "https://opendisplay.org/l/?f=1&k=2&", 35u) == 0);
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
