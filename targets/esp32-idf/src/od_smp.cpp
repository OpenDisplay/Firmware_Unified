/* mcumgr SMP server: the image and OS commands that nRF OTA tooling uses.
 *
 * Upload writes the inactive app slot, a state write marks it for test, reset boots it, and
 * od_smp_confirm_running() keeps it. Slot 0 is always reported as the running image and slot 1 as
 * the other one, which is the shape mcumgr clients expect whichever partition is executing.
 */
#include "od_claim_mode.h"
#include "od_smp.h"
#include "od_health_app.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "od_log.h"

#define SMP_HDR       8u
#define SMP_BUF_SIZE  8192u
#define SMP_OP_READ   0u
#define SMP_OP_WRITE  2u
#define GROUP_OS      0u
#define GROUP_IMAGE   1u
#define OS_RESET      5u
#define OS_PARAMS     6u
#define IMG_STATE     0u
#define IMG_UPLOAD    1u

#define RC_OK         0u
#define RC_UNKNOWN    1u
#define RC_INVAL      3u
#define RC_BADSTATE   6u
#define RC_NOTSUP     8u
#define RC_CORRUPT    9u
#define RC_DENIED     11u

/* Allocated at the first SMP frame, so a device that never updates pays no RAM for it. */
#define SMP_RX_CAP (SMP_BUF_SIZE + 64u)
static uint8_t *s_rx;
static uint8_t s_tx[512];
static size_t s_rx_len;
static volatile bool s_busy;
static bool s_unlocked;
static uint16_t s_conn;
static TaskHandle_t s_task;
static bool s_link_tuned;

static esp_ota_handle_t s_ota;
static const esp_partition_t *s_ota_part;
static uint32_t s_ota_off;
static uint32_t s_ota_len;

typedef struct {
    uint8_t *p;
    size_t n;
    size_t cap;
    bool overflow;
} wr_t;

static void put_head(wr_t *w, uint8_t major, uint32_t v)
{
    uint8_t b[5];
    size_t k;

    if (v < 24u) {
        b[0] = (uint8_t)((major << 5) | v);
        k = 1u;
    } else if (v <= 0xFFu) {
        b[0] = (uint8_t)((major << 5) | 24u);
        b[1] = (uint8_t)v;
        k = 2u;
    } else if (v <= 0xFFFFu) {
        b[0] = (uint8_t)((major << 5) | 25u);
        b[1] = (uint8_t)(v >> 8);
        b[2] = (uint8_t)v;
        k = 3u;
    } else {
        b[0] = (uint8_t)((major << 5) | 26u);
        b[1] = (uint8_t)(v >> 24);
        b[2] = (uint8_t)(v >> 16);
        b[3] = (uint8_t)(v >> 8);
        b[4] = (uint8_t)v;
        k = 5u;
    }
    if (w->n + k > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->n, b, k);
    w->n += k;
}

static void put_raw(wr_t *w, const void *d, size_t n)
{
    if (w->n + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->n, d, n);
    w->n += n;
}

static void put_str(wr_t *w, const char *s)
{
    const size_t n = strlen(s);

    put_head(w, 3u, (uint32_t)n);
    put_raw(w, s, n);
}

static void put_bool(wr_t *w, bool v)
{
    const uint8_t b = v ? 0xF5u : 0xF4u;

    put_raw(w, &b, 1u);
}

typedef struct {
    bool has_off, has_len, has_hash;
    bool confirm;
    uint32_t off, len;
    const uint8_t *data;
    size_t data_n;
    const uint8_t *hash;
    size_t hash_n;
} req_t;

static bool get_head(const uint8_t **pp, const uint8_t *end, uint8_t *major, uint32_t *v)
{
    const uint8_t *p = *pp;
    uint8_t info;
    size_t extra;

    if (p >= end) {
        return false;
    }
    *major = (uint8_t)(*p >> 5);
    info = (uint8_t)(*p & 0x1Fu);
    p++;
    if (info < 24u) {
        *v = info;
        *pp = p;
        return true;
    }
    if (info > 26u) {
        return false;
    }
    extra = (size_t)1u << (info - 24u);
    if ((size_t)(end - p) < extra) {
        return false;
    }
    *v = 0u;
    for (size_t i = 0u; i < extra; i++) {
        *v = (*v << 8) | p[i];
    }
    *pp = p + extra;
    return true;
}

/* A flat map of text keys is all these commands send. Anything else is refused. */
static bool parse_req(const uint8_t *p, const uint8_t *end, req_t *r)
{
    uint8_t major;
    uint32_t count;

    memset(r, 0, sizeof(*r));
    if (p == end) {
        return true;
    }
    if (!get_head(&p, end, &major, &count) || major != 5u) {
        return false;
    }
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t klen;
        uint32_t v;
        const uint8_t *key;

        if (!get_head(&p, end, &major, &klen) || major != 3u || (size_t)(end - p) < klen) {
            return false;
        }
        key = p;
        p += klen;
        if (p >= end) {
            return false;
        }
        if (!get_head(&p, end, &major, &v)) {
            return false;
        }
        if (major == 2u || major == 3u) {
            if ((size_t)(end - p) < v) {
                return false;
            }
            if (major == 2u && klen == 4u && memcmp(key, "data", 4u) == 0) {
                r->data = p;
                r->data_n = v;
            } else if (major == 2u && (klen == 4u || klen == 3u) &&
                       memcmp(key, "hash", klen) == 0) {
                r->hash = p;
                r->hash_n = v;
                r->has_hash = true;
            }
            p += v;
        } else if (major == 0u) {
            if (klen == 3u && memcmp(key, "off", 3u) == 0) {
                r->off = v;
                r->has_off = true;
            } else if (klen == 3u && memcmp(key, "len", 3u) == 0) {
                r->len = v;
                r->has_len = true;
            }
        } else if (major == 7u) {
            if (klen == 7u && memcmp(key, "confirm", 7u) == 0) {
                r->confirm = v == 21u;
            }
        } else if (major != 1u) {
            return false;
        }
    }
    return true;
}

static void send_reply(const uint8_t *req_hdr, uint8_t op, wr_t *w)
{
    const size_t body = w->overflow ? 0u : w->n;
    const size_t total = SMP_HDR + body;
    size_t off = 0u;

    s_tx[0] = (uint8_t)((req_hdr[0] & 0x18u) | (op + 1u));
    s_tx[1] = 0u;
    s_tx[2] = (uint8_t)(body >> 8);
    s_tx[3] = (uint8_t)body;
    memcpy(&s_tx[4], &req_hdr[4], 4u);
    while (off < total) {
        const int sent = od_ble_smp_notify(s_conn, s_tx + off, total - off);

        if (sent <= 0) {
            return;
        }
        off += (size_t)sent;
    }
}

static void reply_rc(const uint8_t *hdr, uint8_t op, uint8_t rc)
{
    wr_t w = { s_tx + SMP_HDR, 0u, sizeof(s_tx) - SMP_HDR, false };

    put_head(&w, 5u, 1u);
    put_str(&w, "rc");
    put_head(&w, 0u, rc);
    send_reply(hdr, op, &w);
}

static const esp_partition_t *other_slot(void)
{
    return esp_ota_get_next_update_partition(NULL);
}

static bool slot_hash(const esp_partition_t *part, uint8_t out[32])
{
    return part != NULL && esp_partition_get_sha256(part, out) == ESP_OK;
}

static void put_image(wr_t *w, uint32_t slot, const char *version, const uint8_t hash[32],
                      bool pending, bool confirmed, bool active)
{
    put_head(w, 5u, 7u);
    put_str(w, "slot");
    put_head(w, 0u, slot);
    put_str(w, "version");
    put_str(w, version);
    put_str(w, "hash");
    put_head(w, 2u, 32u);
    put_raw(w, hash, 32u);
    put_str(w, "bootable");
    put_bool(w, true);
    put_str(w, "pending");
    put_bool(w, pending);
    put_str(w, "confirmed");
    put_bool(w, confirmed);
    put_str(w, "active");
    put_bool(w, active);
}

static void reply_state(const uint8_t *hdr, uint8_t op)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = other_slot();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_VALID;
    esp_app_desc_t desc;
    uint8_t run_hash[32];
    uint8_t other_hash[32];
    const bool have_other = other != NULL &&
                            esp_ota_get_partition_description(other, &desc) == ESP_OK &&
                            slot_hash(other, other_hash);
    wr_t w = { s_tx + SMP_HDR, 0u, sizeof(s_tx) - SMP_HDR, false };

    if (!slot_hash(run, run_hash)) {
        reply_rc(hdr, op, RC_UNKNOWN);
        return;
    }
    (void)esp_ota_get_state_partition(run, &st);
    put_head(&w, 5u, 1u);
    put_str(&w, "images");
    put_head(&w, 4u, have_other ? 2u : 1u);
    put_image(&w, 0u, esp_app_get_description()->version, run_hash, false,
              st != ESP_OTA_IMG_PENDING_VERIFY, true);
    if (have_other) {
        put_image(&w, 1u, desc.version, other_hash, boot == other, false, false);
    }
    send_reply(hdr, op, &w);
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_restart(void)
{
    (void)od_claim_mode_before_reboot();   /* on the caller's task, not the timer's */
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "od_smp_rst" };

    if (timer != NULL || esp_timer_create(&args, &timer) == ESP_OK) {
        (void)esp_timer_start_once(timer, 300000u);
    }
}

static void ota_abort(void)
{
    if (s_ota_part != NULL) {
        (void)esp_ota_abort(s_ota);
        s_ota_part = NULL;
    }
    s_ota_off = 0u;
    s_ota_len = 0u;
}

static void handle_upload(const uint8_t *hdr, const req_t *r)
{
    wr_t w = { s_tx + SMP_HDR, 0u, sizeof(s_tx) - SMP_HDR, false };

    if (!r->has_off) {
        reply_rc(hdr, SMP_OP_WRITE, RC_INVAL);
        return;
    }
    if (r->off == 0u) {
        const esp_partition_t *part = other_slot();

        ota_abort();
        if (part == NULL) {
            reply_rc(hdr, SMP_OP_WRITE, RC_NOTSUP);
            return;
        }
        if (!r->has_len || r->len == 0u || r->len > part->size) {
            reply_rc(hdr, SMP_OP_WRITE, RC_INVAL);
            return;
        }
        if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) != ESP_OK) {
            reply_rc(hdr, SMP_OP_WRITE, RC_UNKNOWN);
            return;
        }
        s_ota_part = part;
        s_ota_len = r->len;
        od_log_info("smp: upload started, %u bytes", (unsigned)r->len);
    }
    if (s_ota_part == NULL) {
        reply_rc(hdr, SMP_OP_WRITE, RC_BADSTATE);
        return;
    }
    if (r->off == s_ota_off && r->data_n > 0u) {
        if (r->data_n > s_ota_len - s_ota_off ||
            esp_ota_write(s_ota, r->data, r->data_n) != ESP_OK) {
            ota_abort();
            reply_rc(hdr, SMP_OP_WRITE, RC_UNKNOWN);
            return;
        }
        s_ota_off += (uint32_t)r->data_n;
    }
    const uint32_t off = s_ota_off;

    if (s_ota_off == s_ota_len) {
        const esp_err_t err = esp_ota_end(s_ota);

        s_ota_part = NULL;
        s_ota_off = 0u;
        s_ota_len = 0u;
        if (err != ESP_OK) {
            od_log_error("smp: image rejected: %s", esp_err_to_name(err));
            reply_rc(hdr, SMP_OP_WRITE, RC_CORRUPT);
            return;
        }
        od_log_info("smp: upload complete");
    }
    put_head(&w, 5u, 2u);
    put_str(&w, "rc");
    put_head(&w, 0u, RC_OK);
    put_str(&w, "off");
    put_head(&w, 0u, off);
    send_reply(hdr, SMP_OP_WRITE, &w);
}

static void handle_state_write(const uint8_t *hdr, const req_t *r)
{
    const esp_partition_t *other = other_slot();
    uint8_t hash[32];

    if (r->has_hash && r->hash_n == 32u && slot_hash(other, hash) &&
        memcmp(hash, r->hash, 32u) == 0) {
        if (esp_ota_set_boot_partition(other) != ESP_OK) {
            reply_rc(hdr, SMP_OP_WRITE, RC_CORRUPT);
            return;
        }
        od_log_info("smp: slot 1 marked for boot");
    } else if (r->confirm && (!r->has_hash ||
               (r->hash_n == 32u && slot_hash(esp_ota_get_running_partition(), hash) &&
                memcmp(hash, r->hash, 32u) == 0))) {
        od_smp_confirm_running();
    } else {
        reply_rc(hdr, SMP_OP_WRITE, RC_INVAL);
        return;
    }
    reply_state(hdr, SMP_OP_WRITE);
}

static void handle_frame(void)
{
    const uint8_t *hdr = s_rx;
    const uint8_t op = (uint8_t)(hdr[0] & 7u);
    const uint16_t group = (uint16_t)((hdr[4] << 8) | hdr[5]);
    const uint8_t id = hdr[7];
    req_t r;

    if (op != SMP_OP_READ && op != SMP_OP_WRITE) {
        return;
    }
    od_smp_note_activity();
    if (od_smp_encryption_on() && !s_unlocked) {
        reply_rc(hdr, op, RC_DENIED);
        return;
    }
    if (!parse_req(s_rx + SMP_HDR, s_rx + s_rx_len, &r)) {
        reply_rc(hdr, op, RC_INVAL);
        return;
    }
    if (group == GROUP_OS && id == OS_PARAMS && op == SMP_OP_READ) {
        wr_t w = { s_tx + SMP_HDR, 0u, sizeof(s_tx) - SMP_HDR, false };

        put_head(&w, 5u, 2u);
        put_str(&w, "buf_size");
        put_head(&w, 0u, SMP_BUF_SIZE);
        put_str(&w, "buf_count");
        put_head(&w, 0u, 1u);
        send_reply(hdr, op, &w);
    } else if (group == GROUP_OS && id == OS_RESET && op == SMP_OP_WRITE) {
        wr_t w = { s_tx + SMP_HDR, 0u, sizeof(s_tx) - SMP_HDR, false };

        put_head(&w, 5u, 0u);
        send_reply(hdr, op, &w);
        schedule_restart();
    } else if (group == GROUP_IMAGE && id == IMG_STATE && op == SMP_OP_READ) {
        reply_state(hdr, op);
    } else if (group == GROUP_IMAGE && id == IMG_STATE && op == SMP_OP_WRITE) {
        handle_state_write(hdr, &r);
    } else if (group == GROUP_IMAGE && id == IMG_UPLOAD && op == SMP_OP_WRITE) {
        handle_upload(hdr, &r);
    } else {
        reply_rc(hdr, op, RC_NOTSUP);
    }
}

/* Flash writes and image hashing do not belong on the NimBLE host stack. */
static void smp_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        handle_frame();
        s_rx_len = 0u;
        s_busy = false;
    }
}

void od_smp_rx(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    size_t want;

    if (s_busy || data == NULL) {
        return;
    }
    if (s_task == NULL && xTaskCreate(smp_task, "od_smp", 6144, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return;
    }
    if (s_rx == NULL) {
        s_rx = (uint8_t *)malloc(SMP_RX_CAP);
        if (s_rx == NULL) {
            return;
        }
    }
    if (s_rx_len + len > SMP_RX_CAP) {
        s_rx_len = 0u;
        return;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;
    if (s_rx_len < SMP_HDR) {
        return;
    }
    want = SMP_HDR + (((size_t)s_rx[2] << 8) | s_rx[3]);
    if (want > SMP_RX_CAP) {
        s_rx_len = 0u;
    } else if (s_rx_len >= want) {
        s_rx_len = want;
        /* Each upload request is a full round trip, so the connection interval sets the rate. */
        if (!s_link_tuned) {
            s_link_tuned = true;
            od_ble_smp_fast_link(conn_handle);
        }
        s_conn = conn_handle;
        s_busy = true;
        xTaskNotifyGive(s_task);
    }
}

void od_smp_link_lost(void)
{
    s_link_tuned = false;
    if (!s_busy) {
        s_rx_len = 0u;
        ota_abort();
    }
}

void od_smp_unlock(void)
{
    s_unlocked = true;
}

void od_smp_confirm_running(void)
{
    od_health_app_confirmed_externally();
    (void)esp_ota_mark_app_valid_cancel_rollback();
}
