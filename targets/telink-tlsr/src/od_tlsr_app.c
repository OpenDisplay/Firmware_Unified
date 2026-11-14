/* OD-side application: config lifecycle, advertising payload, and the main-loop pump that feeds
 * received frames through shared dispatch. Called from the SDK side through tlsr_port.h. */

#include "od_led_tlsr.h"
#include "od_tlsr.h"

#include "epd_port.h"
#include "od_advert.h"
#include "od_boot_screen.h"
#include "od_config.h"
#include "od_config_read.h"
#include "od_config_store.h"
#include "od_core.h"
#include "od_dispatch.h"
#include "od_hal_time.h"
#include "od_rxq.h"
#include "od_rxq_app.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_txq.h"
#include "tlsr_port.h"

#include <stddef.h>
#include <string.h>

/* Three failed authentications close the link, as on BG22. */
#define OD_AUTH_ABUSE_LIMIT 3u

/* ONE OBJECT IS BOTH the chunked-write reassembly state and the config-store workspace: the
 * assembler's four state words fill exactly the 16-byte record header and both put their byte
 * array at offset 16. Same arrangement as targets/efr32bg22-slc/opendisplay_config_storage.c,
 * with the same ordering obligations: capture the length before a save, refuse before the header
 * write, and reset the assembler immediately after. */
typedef union {
    struct od_config_asm assembler;
    uint8_t              record[OD_CONFIG_STORE_MAX_RECORD];
} od_config_work_t;

typedef char od_asm_is_record[(sizeof(struct od_config_asm) == OD_CONFIG_STORE_MAX_RECORD) ? 1 : -1];

static od_config_work_t s_work;
static struct od_config s_cfg;
static struct od_session s_session;

/* Link identity: bumped on every connect, stamped into each RX slot and reply, so a frame or a
 * reply belonging to a departed central can never run or be delivered on the next one. */
static volatile uint32_t s_link_tag;
static volatile bool     s_subscribed;
static volatile bool     s_close_pending;
static uint8_t           s_auth_abuse;
static uint8_t           s_msd_counter;
static bool              s_reboot_flag = true;

/* Diagnostic block in the MSD's config-driven area (bytes 0..3 of od_advert_inputs.dynamic):
 * marker, the frame-path step the previous run died in (tlsr_port.h), the persistent
 * watchdog-reset count, and a build tag. Read it with any BLE scanner. */
#define DIAG_MARKER     0xD1u
#define DIAG_BUILD_TAG  0x06u
static uint8_t           s_dynamic[OD_ADVERT_DYNAMIC_LEN];

/* Boot screen: drawn once per boot from the main loop (never during BLE init, and never while a
 * central is connected), unless the config sets CLEAR_ON_BOOT or the previous run ended in a
 * watchdog reset -- a crash must not replace the image a user put there. Buffers are sized for a
 * row of the widest supported panel and a version-6 QR code (211 bytes). */
static bool              s_boot_pending;
static uint8_t           s_boot_row[256];
static uint8_t           s_boot_qr[256];

/* Battery: measured at boot and every BATTERY_PERIOD_MS on PowerOption.battery_sense_pin (0 mV =
 * no pin or not measured). Temperature: the panel controller's sensor, refreshed whenever the
 * panel is driven -- the chip's own sensor has no calibration in the SDK. */
#define BATTERY_PERIOD_MS 60000u
static uint16_t          s_battery_mv;
static uint32_t          s_battery_at;
static bool              s_battery_done;
static bool              s_temp_valid;
static int8_t            s_temp_c;
static uint8_t           s_msd[OD_ADVERT_MSD_LEN];

/* --------------------------------------------------------------------------- config --- */

struct od_config_asm *od_tlsr_config_assembler(void)
{
    return &s_work.assembler;
}

const struct od_config *od_tlsr_config(void)
{
    return &s_cfg;
}

struct LedConfig *od_tlsr_led(uint8_t instance)
{
    return s_cfg.loaded && instance < s_cfg.led_count ? &s_cfg.leds[instance] : NULL;
}

bool od_tlsr_config_save(const uint8_t *data, uint32_t len)
{
    enum od_config_store_result rc;

    if (data == NULL || len > OD_CONFIG_MAX_SIZE || od_config_store_init() != OD_CONFIG_STORE_OK) {
        return false;
    }
    rc = od_config_store_save(s_work.record, sizeof(s_work.record), data, len);
    od_config_asm_reset(&s_work.assembler);      /* its state words are the header now */
    return rc == OD_CONFIG_STORE_OK;
}

bool od_tlsr_config_load(uint8_t *out, uint32_t *len)
{
    return od_config_store_init() == OD_CONFIG_STORE_OK &&
           od_config_store_load(out, len) == OD_CONFIG_STORE_OK;
}

bool od_tlsr_config_clear(void)
{
    return od_config_store_clear() == OD_CONFIG_STORE_OK;
}

/* Re-parse from flash into s_cfg. Reads through the assembler's buffer, so it must only run
 * when no chunked write is in flight: at boot, and straight after a save or clear reset it. */
void od_tlsr_config_reload(void)
{
    uint32_t len = OD_CONFIG_MAX_SIZE;
    struct od_config_report report;

    od_config_reset(&s_cfg);
    if (s_work.assembler.active || !od_tlsr_config_load(s_work.assembler.buffer, &len)) {
        return;
    }
    (void)od_config_parse(&s_cfg, od_span_make(s_work.assembler.buffer, len), &report);
    epd_io_park_power(s_cfg.system_config.pwr_pin);
    epd_io_park_power(od_tlsr_panel_pwr2(&s_cfg));
}

/* ------------------------------------------------------------------------ advertising --- */

void od_tlsr_set_temperature(int8_t celsius)
{
    bool changed = !s_temp_valid || s_temp_c != celsius;

    s_temp_valid = true;
    s_temp_c = celsius;
    if (changed) {
        od_tlsr_publish_msd();
    }
}

float od_tlsr_battery_volts(void)
{
    return s_battery_mv != 0u ? (float)s_battery_mv / 1000.0f : -1.0f;
}

float od_tlsr_temperature_c(void)
{
    return s_temp_valid ? (float)s_temp_c : -1000.0f;
}

/* Measure only when idle: the ADC runs off the same supply the panel and radio load, and a reading
 * taken mid-refresh would report the sag, not the battery. */
static void battery_poll(void)
{
    uint8_t pin = s_cfg.power_option.battery_sense_pin;
    uint32_t now = od_hal_uptime_ms();
    uint16_t mv;

    if (s_cfg.loaded == false || pin == 0xFFu || tlsr_port_connected() ||
        (s_battery_done && (uint32_t)(now - s_battery_at) < BATTERY_PERIOD_MS)) {
        return;
    }
    s_battery_done = true;
    s_battery_at = now;
    mv = tlsr_port_battery_mv(pin);
    /* 10 mV granularity on the wire: republish only when the advertised value would change. */
    if (od_advert_battery_10mv_from_mv(mv) != od_advert_battery_10mv_from_mv(s_battery_mv)) {
        s_battery_mv = mv;
        od_tlsr_publish_msd();
    }
    s_battery_mv = mv;
}

void od_tlsr_publish_msd(void)
{
    struct od_advert_inputs adv;

    /* Unmeasured values go out as 0 V and -40 C rather than invented ones. */
    memset(&adv, 0, sizeof(adv));
    adv.dynamic = s_dynamic;
    adv.chip_temperature_c = s_temp_valid ? (float)s_temp_c : OD_ADVERT_TEMP_MIN_C;
    adv.battery_10mv = od_advert_battery_10mv_from_mv(s_battery_mv);
    adv.reboot_flag = s_reboot_flag;
    adv.loop_counter = s_msd_counter;
    od_advert_build(&adv, s_msd);
    s_msd_counter = od_advert_advance_counter(s_msd_counter);
    tlsr_port_set_adv_msd(s_msd);
}

void od_tlsr_copy_msd(uint8_t out[16])
{
    memcpy(out, s_msd, OD_ADVERT_MSD_LEN);
}

/* -------------------------------------------------------------------- link identity --- */

uint32_t od_tlsr_link_tag(void)
{
    return s_link_tag;
}

bool od_tlsr_notify_subscribed(void)
{
    return s_subscribed;
}

static bool rx_tag_is_live(uint32_t tag, void *context)
{
    (void)context;
    return tlsr_port_connected() && tag == s_link_tag;
}

/* ------------------------------------------------------------------ SDK-side callbacks --- */

void od_tlsr_init(void)
{
    uint8_t resets = 0u;
    uint8_t died_in = tlsr_port_crumb_boot(&resets);

    s_dynamic[0] = DIAG_MARKER;
    s_dynamic[1] = died_in;
    s_dynamic[2] = resets;
    s_dynamic[3] = DIAG_BUILD_TAG;
    s_boot_pending = (died_in == 0u);
    od_session_init(&s_session, 0u);
    od_config_asm_reset(&s_work.assembler);
    od_tlsr_config_reload();
    od_core_reset();
    od_tlsr_publish_msd();
    if (s_cfg.display_count == 0u ||
        (s_cfg.displays[0].transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u) {
        s_boot_pending = false;
    }
    s_dynamic[4] = s_boot_pending ? 0xB0u : 0x00u;
    od_tlsr_publish_msd();
    od_led_tlsr_boot_blink();
}

/* MSD byte 4: boot-screen outcome (0xB0 pending, 0xB1 started, 0xB2 drawn, 0xE1..0xE5 the first
 * od_boot_app hook that refused, 0xEF refused before any hook ran, 0x00 not attempted). */
extern uint8_t od_tlsr_boot_fail;

static void boot_screen(void)
{
    struct od_boot_bufs bufs;
    bool ok;

    bufs.row = s_boot_row;
    bufs.row_len = sizeof(s_boot_row);
    bufs.qr = s_boot_qr;
    bufs.qr_len = sizeof(s_boot_qr);
    s_dynamic[4] = 0xB1u;
    od_tlsr_publish_msd();            /* a large panel renders and refreshes for a minute or more */
    od_tlsr_boot_fail = 0u;
    ok = od_boot_screen_render(&s_cfg, od_session_app_security(), &bufs);
    s_dynamic[4] = ok ? 0xB2u : (od_tlsr_boot_fail != 0u ? od_tlsr_boot_fail : 0xEFu);
    od_tlsr_publish_msd();
}

void od_tlsr_on_connect(void)
{
    s_link_tag++;
    s_subscribed = false;
    s_auth_abuse = 0u;
}

void od_tlsr_on_disconnect(void)
{
    s_subscribed = false;
    s_link_tag++;                      /* strands anything the departed central left queued */
    s_close_pending = true;            /* the loop owns shared state; reset it there */
}

void od_tlsr_on_notify_enabled(bool enabled)
{
    s_subscribed = enabled;
}

/* ATT write callback context: queue only. Dispatch runs from od_tlsr_poll(), because it may have
 * to defer a frame, and a deferred frame needs somewhere to wait that the stack does not own. */
void od_tlsr_on_write(const uint8_t *data, uint16_t len)
{
    (void)od_rxq_push(data, len, s_link_tag);
    tlsr_port_crumb(2);
}

void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
    od_frame_policy_t p = od_frame_policy(outcome);

    if (rp == NULL || rp->origin != OD_ORIGIN_BLE || rp->tag != s_link_tag) {
        return;
    }
    if (p.stamp_activity) {
        od_session_touch(&s_session, od_hal_uptime_ms());
    }
    if (p.reset_abuse) {
        s_auth_abuse = 0u;
    }
    if (p.increment_abuse && s_auth_abuse < 0xFFu && ++s_auth_abuse >= OD_AUTH_ABUSE_LIMIT) {
        tlsr_port_disconnect();
    }
}

/* The main-loop pump, in the order targets/nordic-zephyr/src/opendisplay_pipe.c documents:
 * cleanup, TX, config-read producer, stale discard, dispatch, consume-unless-deferred, TX. TX
 * goes before dispatch because dispatch reserves reply capacity before it decrypts; a frame
 * deferred after decrypt would be refused as a replay when re-offered. */
void od_tlsr_poll(void)
{
    uint8_t drained;

    (void)od_hal_uptime_ms();          /* keep the tick extension ahead of its 268 s wrap */

    battery_poll();
    od_led_tlsr_poll();

    if (s_boot_pending && !tlsr_port_connected()) {
        s_boot_pending = false;
        boot_screen();
    }

    if (s_close_pending) {
        s_close_pending = false;
        od_core_reset();
        od_config_asm_reset(&s_work.assembler);
        od_session_init(&s_session, 0u);
    }

    for (drained = 0u; drained < OD_RXQ_SLOTS; ++drained) {
        od_rxq_item_t *item;
        od_reply_t rp;
        od_frame_outcome_t outcome;

        (void)od_txq_process();
        (void)od_config_read_pump();
        (void)od_rxq_discard_stale(rx_tag_is_live, NULL);
        item = od_rxq_peek();
        if (item == NULL) {
            break;
        }
        if (!rx_tag_is_live(item->tag, NULL)) {
            od_rxq_consume();
            continue;
        }
        rp.origin = OD_ORIGIN_BLE;
        rp.tag = item->tag;
        tlsr_port_crumb(4);
        outcome = od_dispatch_frame(&rp, od_span_make(item->data, item->len));
        tlsr_port_crumb(5);
        od_core_frame_done(&rp, outcome);
        if (!od_frame_policy(outcome).consume_rx) {
            break;
        }
        od_rxq_consume();
        tlsr_port_crumb(6);
        s_reboot_flag = false;
    }
    (void)od_txq_process();
}

/* ------------------------------------------------------------------------ session app --- */

struct od_session *od_session_app_state(void)
{
    return &s_session;
}

const struct SecurityConfig *od_session_app_security(void)
{
    return s_cfg.security_loaded ? &s_cfg.security : NULL;
}

uint32_t od_session_app_now_ms(void)
{
    return od_hal_uptime_ms();
}

void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
    uint8_t mac[6];

    tlsr_port_mac(mac);
    out[0] = mac[3];
    out[1] = mac[2];
    out[2] = mac[1];
    out[3] = mac[0];
}

void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{
    (void)op;
    (void)result;
    (void)cmd;
    (void)report;
}

/* ---------------------------------------------------------------------------- rxq app --- */

bool od_rxq_app_encryption_enabled(void)
{
    return od_session_security_enabled(od_session_app_security());
}

bool od_rxq_app_quiet(uint16_t cmd)
{
    (void)cmd;
    return true;
}
