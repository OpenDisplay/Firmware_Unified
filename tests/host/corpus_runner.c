/* corpus_runner.c -- replay tests/vectors/dispatch.json through the production
 * od_dispatch_frame(), and check the reply half the Python runner structurally cannot.
 *
 * WHAT THIS ADDS. tests/host/replay_vectors.py drives the same corpus through py-opendisplay's
 * public API and states outright that firmware replies are "never checkable here"
 * (replay_vectors.py:18, :44). Every h2d `expect.reply` in the corpus has therefore been unchecked
 * against firmware since the corpus was authored. This is the other end of that contract: same
 * file, same vectors, the device side.
 *
 * SILENCE IS AN ASSERTION. `expect.reply: null` means the device emits NOTHING, and that is the
 * property with teeth -- an unknown opcode that starts replying holds an exclusive link open, and a
 * replayed PIPE frame that starts replying kills an upload. A runner that only compared frames it
 * received would pass all of those.
 *
 * THE ORACLE PROBLEM. A profile's fakes never see an expected reply: the generated table is
 * included here and nowhere else, and profiles get corpus_runner.h instead. If a vector cannot be
 * reached without copying its answer into a fixture, the honest outcome is to fail rather than to
 * add the copy -- see the C12 plan, section 4.2.
 */

#include "corpus_runner.h"

#include "od_config_read.h"
#include "od_dispatch.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_txq.h"
#include "sec2_client.h"
#include "session_fake.h"

#include <stdio.h>
#include <string.h>

/* The generated table. Included by THIS translation unit only. */
#include "dispatch_vectors.inc"

static unsigned g_failures;
static unsigned g_checks;

#define FAILV(vec, fmt, ...)                                                   \
    do {                                                                       \
        ++g_failures;                                                          \
        printf("FAIL [%s] " fmt "\n", (vec)->id, ##__VA_ARGS__);               \
    } while (0)

/* ------------------------------------------------------------------------------ fake radio --- */

#define SENT_MAX 16u
#define SENT_LEN 320u
static struct { uint16_t len; uint8_t data[SENT_LEN]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static unsigned g_pop;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].len = len;
        memcpy(g_sent[g_sent_n].data, frame, (len < SENT_LEN) ? len : SENT_LEN);
        ++g_sent_n;
    }
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return true;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

/* The SEC2 client reads its handshake replies from the same capture. */
bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_pop >= g_sent_n || g_pop >= SENT_MAX) {
        return false;
    }
    memcpy(buf, g_sent[g_pop].data, g_sent[g_pop].len);
    *len = g_sent[g_pop].len;
    ++g_pop;
    return true;
}

/* ------------------------------------------------------------------------- od_sec2_app hooks --- */

static uint32_t g_now_ms = 1000u;

uint32_t od_sec2_app_now_ms(void) { return g_now_ms; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

/* The dispatcher's own policy hook. Nothing here reads it, but every caller of od_dispatch_frame()
 * must call it exactly once, so the runner does what a target does. */
void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
    (void)rp; (void)outcome;
}

struct od_corpus_knobs od_corpus_knobs;

/* --------------------------------------------------------------------------------- helpers --- */

static void hex(const uint8_t *b, size_t n, char *out, size_t cap)
{
    size_t i;
    size_t o = 0;
    for (i = 0; i < n && o + 3u < cap; ++i) {
        o += (size_t)snprintf(out + o, cap - o, "%02x", b[i]);
    }
    if (i < n && cap > 4u) {
        snprintf(out + (cap - 4u), 4u, "...");
    }
    out[(o < cap) ? o : (cap - 1u)] = '\0';
}

/* `sec_enabled` is a claimed device (a 2.x key migrated at boot), and a live session is a real
 * SEC_AUTH handshake through the dispatcher -- not a flag set behind od_sec2's back, which would
 * test a state the firmware cannot actually be in. */
static bool arm(const od_vec_t *v)
{
    static struct sec2c client;

    fake_reset();
    sec_init(0);
    od_config_read_cancel();
    od_txq_reset();
    g_sent_n = 0u;
    g_pop = 0u;
    (void)od_sec2_boot(v->sec_enabled ? &g_sec : NULL);

    od_corpus_knobs.xfer_active = v->xfer_active;
    od_corpus_knobs.storage_ok = v->storage_ok;
    od_corpus_knobs.fw_patch_byte = v->fw_patch_byte;
    od_corpus_knobs.fw_sha = v->fw_sha;
    od_corpus_knobs.caps = od_corpus_profile_caps();
    od_corpus_profile_reset(v);

    if (v->session_live) {
        const od_reply_t rp = { OD_ORIGIN_BLE, 9u };

        sec2c_init(&client, MASTER, UID);
        if (sec2c_auth(&client, &rp) != 0) {
            return false;
        }
        /* The handshake itself answers; those bytes are not this vector's subject. */
        g_sent_n = 0u;
        g_pop = 0u;
        od_txq_reset();
    }
    return true;
}

/* Vectors whose expected bytes are v1 session behaviour, which SEC2 replaced. Each needs a
 * corpus update in ../opendisplay-protocol; until then they are counted, not run.
 *   dispatch/decrypt-failure  a v1 CCM envelope answered [00][cmd][FF]; SEC2 records are
 *                             [ctr][C][T] and refuse with a sealed [FF][cmd][F1..F3]. */
static const char *const k_superseded[] = {
    "dispatch/decrypt-failure",
};

static bool superseded(const od_vec_t *v)
{
    size_t i;

    for (i = 0u; i < sizeof k_superseded / sizeof k_superseded[0]; ++i) {
        if (strcmp(v->id, k_superseded[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------------------------- cases --- */

struct totals {
    unsigned discovered, executed, d2h_only, excluded, excluded_historical, superseded;
    unsigned by_proof[3];
};

static void run_vector(const od_vec_t *v, struct totals *t)
{
    const unsigned caps = od_corpus_profile_caps();
    unsigned si;

    ++t->discovered;

    if ((v->requires_caps & ~caps) != 0u || (v->forbids_caps & caps) != 0u) {
        /* Counted, not skipped. A predicate exclusion is a statement about the profile, and an
         * unreported one is indistinguishable from coverage. */
        ++t->excluded;
        /* EXCEPT for a target-production expectation in a production profile. That vector claims
         * firmware emits those bytes, and this executable is the only thing that can show it -- so
         * a capability predicate quietly putting it out of reach would leave the claim standing
         * with nothing behind it. Either the predicate is wrong or the classification is. */
        if (v->proof == OD_PROOF_TARGET_PRODUCTION && od_corpus_profile_is_production()) {
            FAILV(v, "classified target-production but excluded by a capability predicate here; "
                     "nothing proves its bytes");
        }
        return;
    }

    if (v->proof == OD_PROOF_HISTORICAL_FIXTURE && od_corpus_profile_is_production()) {
        /* A shape current production does not emit. Running it here could only pass by making the
         * firmware or the vector agree with the other, which is the edit this classification
         * exists to forbid; the portable profile covers it instead. */
        ++t->excluded_historical;
        return;
    }

    if (superseded(v)) {
        ++t->superseded;
        return;
    }

    if (!arm(v)) {
        FAILV(v, "could not establish the declared initial state");
        return;
    }

    for (si = 0u; si < v->step_n; ++si) {
        const od_vec_step_t *s = &v->steps[si];
        od_reply_t rp;
        unsigned i;

        if (s->d2h) {
            /* An unsolicited device->host notification is not an input to a dispatcher. Counted as
             * direction-only rather than pretended to have been replayed. */
            ++t->d2h_only;
            continue;
        }

        g_sent_n = 0u;
        rp.origin = s->origin;
        rp.tag = 9u;
        {
            const od_frame_outcome_t outcome =
                od_dispatch_frame(&rp, od_span_make(s->frame, (uint16_t)s->frame_len));
            od_core_frame_done(&rp, outcome);
        }
        (void)od_txq_process();
        ++t->executed;
        ++g_checks;

        if (s->silent) {
            if (g_sent_n != 0u) {
                char got[80];
                hex(g_sent[0].data, g_sent[0].len, got, sizeof got);
                FAILV(v, "step %u expected SILENCE, got %u frame(s), first %s",
                      si, g_sent_n, got);
            }
            continue;
        }

        if (g_sent_n != s->reply_n) {
            FAILV(v, "step %u expected %u reply frame(s), got %u", si, s->reply_n, g_sent_n);
            for (i = 0u; i < g_sent_n; ++i) {
                char got[80];
                hex(g_sent[i].data, g_sent[i].len, got, sizeof got);
                printf("       got[%u] = %s\n", i, got);
            }
            continue;
        }
        for (i = 0u; i < s->reply_n; ++i) {
            if (g_sent[i].len != s->replies[i].len ||
                memcmp(g_sent[i].data, s->replies[i].bytes, s->replies[i].len) != 0) {
                char got[80], want[80];
                hex(g_sent[i].data, g_sent[i].len, got, sizeof got);
                hex(s->replies[i].bytes, s->replies[i].len, want, sizeof want);
                FAILV(v, "step %u reply %u: want %s, got %s", si, i, want, got);
            }
        }
    }
    ++t->by_proof[v->proof];
}

int main(void)
{
    struct totals t;
    unsigned i;

    memset(&t, 0, sizeof t);
    for (i = 0u; i < sizeof k_vectors / sizeof k_vectors[0]; ++i) {
        run_vector(&k_vectors[i], &t);
    }

    printf("corpus[%s]: %u discovered, %u h2d steps executed, %u d2h direction-only, "
           "%u excluded by predicate, %u historical (not production-provable), "
           "%u superseded by SEC2\n",
           od_corpus_profile_name(), t.discovered, t.executed, t.d2h_only, t.excluded,
           t.excluded_historical, t.superseded);
    printf("corpus[%s]: shared=%u target-production=%u historical-fixture=%u\n",
           od_corpus_profile_name(), t.by_proof[OD_PROOF_SHARED],
           t.by_proof[OD_PROOF_TARGET_PRODUCTION], t.by_proof[OD_PROOF_HISTORICAL_FIXTURE]);
    printf("corpus[%s]: %u checks, %u failures\n", od_corpus_profile_name(), g_checks, g_failures);

    /* A runner that found nothing is a runner that proves nothing, and it is the exact shape of
     * rot TEST_OWNERSHIP.md indicts in fixture fallbacks elsewhere. */
    if (t.discovered == 0u || t.executed == 0u) {
        printf("FAIL no vectors discovered or none executed\n");
        return 1;
    }
    return (g_failures == 0u) ? 0 : 1;
}
