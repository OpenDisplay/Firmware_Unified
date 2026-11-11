/* sec2_client.h -- a SEC2 host for the host suites. NOT AN INDEPENDENT IMPLEMENTATION.
 *
 * It was written from docs/SECURITY_V2.md by the same author as shared/core/od_sec2.c, in the
 * same session, on the same primitives (session_fake.c: host AES, CMAC, CCM; OpenSSL P-256). A
 * misreading of the spec common to both would pass every suite that uses it. What it does give:
 * an end-to-end drive through od_dispatch_frame(), fragmentation and pumping included, with the
 * KDF, transcripts and record layout rewritten here rather than called from shared/.
 *
 * The suite links it with one function of its own, sec2c_radio_pop(), which hands back the
 * next device->host frame its fake radio captured.
 */
#ifndef OD_TEST_SEC2_CLIENT_H
#define OD_TEST_SEC2_CLIENT_H

#include "od_cmd.h"
#include "od_span.h"
#include "od_txq.h"

#include <stdbool.h>
#include <stdint.h>

/* IMPLEMENTED BY THE SUITE: pop the oldest captured D->H frame. */
bool sec2c_radio_pop(uint8_t *buf, uint16_t *len);

/* Hand one H->D frame to the device. sec2_client_dispatch.c (in the session-fake archives) sends
 * it through od_dispatch_frame(); sec2_client_direct.c, compiled into a suite that does not link
 * the dispatcher, hands the SEC opcodes straight to od_sec2_command(). */
od_frame_outcome_t sec2c_deliver(const od_reply_t *rp, od_span_t frame);

struct sec2c {
    uint8_t  k_dev[16];
    uint8_t  uid[8];
    uint8_t  frag_max;     /* declared in BEGIN / HELLO; 20 unless a case sets it */
    uint8_t  dflags;
    uint8_t  k_h2d[16];
    uint8_t  k_d2h[16];
    uint8_t  sid[16];
    uint32_t tx_ctr;       /* last counter sealed H->D */
    uint32_t rx_ctr;       /* last device counter accepted */
    uint16_t timeout_s;
    bool     live;
    uint8_t  last_err;     /* the err byte of the last [FF][cmd][sub][err], else 0 */
    uint32_t code;         /* claim: the code this client computed */
};

void sec2c_init(struct sec2c *c, const uint8_t k_dev[16], const uint8_t uid[8]);

/* The KDF of SECURITY_V2 2.1, written again here: CMAC(K, 01 || label || 00 || ctx || 00 80). */
void sec2c_kdf(const uint8_t k[16], const char *label, const uint8_t *ctx, uint16_t ctx_len,
               uint8_t out[16]);

/* One fragmented SEC_CLAIM / SEC_AUTH exchange through od_dispatch_frame(). Returns the response
 * body length (>= 0), or -err for a 4-byte error, or -1000 when no well-formed reply came. */
int sec2c_xchg(struct sec2c *c, const od_reply_t *rp, uint16_t cmd, uint8_t sub,
               const uint8_t *body, uint8_t len, uint8_t *resp, uint8_t resp_cap);

/* HELLO + FINISH with c->k_dev. On success the client holds the record keys. Returns 0 or the
 * device's error code (or 1000 for a local failure: a wrong device tag, no reply). */
int sec2c_auth(struct sec2c *c, const od_reply_t *rp);

/* BEGIN + NONCE + CONFIRM. On success c->k_dev is the new key and a session is live.
 * corrupt_tag flips one bit of tag_c to drive AUTH_FAILED. */
int sec2c_claim(struct sec2c *c, const od_reply_t *rp, bool corrupt_tag);

/* Seal one H->D record: out = [cmd:2][ctr:4 LE][C][T:12]. Returns its length. */
uint16_t sec2c_seal(struct sec2c *c, uint16_t cmd, const uint8_t *payload, uint16_t n,
                    uint8_t *out);
/* As sec2c_seal with an explicit counter (replay / reorder cases); tx_ctr is left alone. */
uint16_t sec2c_seal_ctr(struct sec2c *c, uint16_t cmd, uint32_t ctr, const uint8_t *payload,
                        uint16_t n, uint8_t *out);

/* Open a D->H [E5][ctr][C][T]: true with the plaintext response frame in out. Enforces the
 * strictly-increasing device counter (section 9.5 client rule 1). */
bool sec2c_open(struct sec2c *c, const uint8_t *frame, uint16_t n, uint8_t *out,
                uint16_t *out_n);

/* Dispatch one frame, then drain the queue and pump until nothing more leaves. Returns how many
 * D->H frames were captured into frames[]/lens[] (at most max). */
unsigned sec2c_send(const od_reply_t *rp, const uint8_t *frame, uint16_t n,
                    uint8_t frames[][256], uint16_t *lens, unsigned max);

#endif /* OD_TEST_SEC2_CLIENT_H */
