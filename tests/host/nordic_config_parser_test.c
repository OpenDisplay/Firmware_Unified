/* Nordic config publication: production parser and RX predicate against fake storage. */

#include "od_config.h"
#include "od_rxq_app.h"
#include "opendisplay_config_parser.h"
#include "session_fake.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* od_rxq_app.c delegates quiet-frame policy to the transfer owner. This parser fixture does not
 * link transfer state; the real target does. */
bool od_xfer_log_quiet(uint16_t opcode)
{
    (void)opcode;
    return false;
}

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))

struct blob {
    uint8_t bytes[128];
    uint32_t len;
};

static void make_security_blob(struct blob *b, uint8_t enabled, bool key_set)
{
    struct SecurityConfig sec;
    uint16_t crc;

    memset(b, 0, sizeof(*b));
    memset(&sec, 0, sizeof(sec));
    sec.encryption_enabled = enabled;
    if (key_set) {
        sec.encryption_key[15] = 0xA5u;
    }

    b->bytes[2] = 1u;
    b->bytes[4] = 0x27u;
    memcpy(&b->bytes[5], &sec, sizeof(sec));
    b->len = 5u + (uint32_t)sizeof(sec);
    crc = od_config_tlv_crc16(od_span_make(b->bytes, b->len));
    b->bytes[b->len++] = (uint8_t)(crc & 0xFFu);
    b->bytes[b->len++] = (uint8_t)(crc >> 8);
}

static void append_truncated_display(struct blob *b)
{
    uint16_t crc;

    b->len -= 2u;                  /* replace the existing CRC */
    b->bytes[b->len++] = 0u;
    b->bytes[b->len++] = 0x20u;
    b->bytes[b->len++] = 0xA5u;   /* shorter than DisplayConfig */
    crc = od_config_tlv_crc16(od_span_make(b->bytes, b->len));
    b->bytes[b->len++] = (uint8_t)(crc & 0xFFu);
    b->bytes[b->len++] = (uint8_t)(crc >> 8);
}

static bool g_init_ok;
static bool g_load_ok;
static bool g_seen_during_init;
static bool g_seen_during_load;
static struct blob g_stored;

bool initConfigStorage(void)
{
    g_seen_during_init = od_security_enabled_snapshot();
    return g_init_ok;
}

bool loadConfig(uint8_t *data, uint32_t *len)
{
    g_seen_during_load = od_security_enabled_snapshot();
    if (!g_load_ok || data == NULL || len == NULL || *len < g_stored.len) {
        return false;
    }
    memcpy(data, g_stored.bytes, g_stored.len);
    *len = g_stored.len;
    return true;
}

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}

static void publish_blob(struct od_config *cfg, uint8_t enabled, bool key_set)
{
    struct blob b;

    make_security_blob(&b, enabled, key_set);
    CHECK(parseConfigBytes(b.bytes, b.len, cfg));
}

static void test_derived_security_rule(void)
{
    struct od_config cfg;

    CASE("enabled with a configured key publishes enabled");
    publish_blob(&cfg, 1u, true);
    CHECK(od_security_enabled_snapshot());
    CHECK(od_rxq_app_encryption_enabled());

    CASE("a clear enable flag publishes disabled");
    publish_blob(&cfg, 0u, true);
    CHECK(!od_security_enabled_snapshot());
    CHECK(!od_rxq_app_encryption_enabled());

    CASE("the shared zero-key rule publishes disabled");
    publish_blob(&cfg, 1u, false);
    CHECK(!cfg.security.encryption_enabled);
    CHECK(!od_security_enabled_snapshot());
}

/* The 2.x rule the snapshot publishes: encryption enabled and a non-zero key. Written out here
 * rather than read from the parser, so a drift in its loop shows. */
static bool key_rule(const struct SecurityConfig *sec)
{
    unsigned i;

    if (sec->encryption_enabled == 0u) {
        return false;
    }
    for (i = 0u; i < sizeof sec->encryption_key; ++i) {
        if (sec->encryption_key[i] != 0u) {
            return true;
        }
    }
    return false;
}

static void test_partial_parse_uses_the_key_rule(void)
{
    struct od_config cfg;
    struct blob b;

    CASE("a security packet before truncation matches the enabled-and-non-zero-key rule");
    make_security_blob(&b, 1u, true);
    append_truncated_display(&b);
    CHECK(!parseConfigBytes(b.bytes, b.len, &cfg));
    CHECK(!cfg.loaded);
    CHECK(cfg.security.encryption_enabled == 1u);
    CHECK(key_rule(&cfg.security));
    CHECK(od_security_enabled_snapshot());
    CHECK(od_rxq_app_encryption_enabled());
}

static void test_load_publishes_only_after_completion(void)
{
    struct od_config cfg;

    CASE("the prior coherent snapshot remains visible while storage is read");
    publish_blob(&cfg, 1u, true);
    make_security_blob(&g_stored, 0u, true);
    g_init_ok = true;
    g_load_ok = true;
    g_seen_during_init = false;
    g_seen_during_load = false;
    CHECK(loadGlobalConfig(&cfg));
    CHECK(g_seen_during_init);
    CHECK(g_seen_during_load);
    CHECK(!od_security_enabled_snapshot());

    CASE("an initialization failure publishes the reset disabled state");
    publish_blob(&cfg, 1u, true);
    g_init_ok = false;
    g_seen_during_init = false;
    CHECK(!loadGlobalConfig(&cfg));
    CHECK(g_seen_during_init);
    CHECK(!od_security_enabled_snapshot());

    CASE("a missing stored config publishes the reset disabled state");
    publish_blob(&cfg, 1u, true);
    g_init_ok = true;
    g_load_ok = false;
    g_seen_during_load = false;
    CHECK(!loadGlobalConfig(&cfg));
    CHECK(g_seen_during_load);
    CHECK(!od_security_enabled_snapshot());
}

int main(void)
{
    fake_reset();

    test_derived_security_rule();
    test_partial_parse_uses_the_key_rule();
    test_load_publishes_only_after_completion();

    printf("nordic_config_parser: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
