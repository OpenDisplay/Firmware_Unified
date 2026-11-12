/* od_health.h -- ONE rule, every target: an image is trusted only after a link has carried GATT
 * traffic, and a link that cannot serve is a failure (docs/BG22_RAM_BUDGET.md section 4,
 * docs/UPGRADING.md "Image trust").
 *
 * THE FAILURE IT GUARDS: an image that boots, advertises and accepts a link, but whose GATT server
 * never answers -- on BG22 the Bluetooth stack failing a heap allocation for the connection's ATT
 * bearer, with nothing failing at boot. Such a unit cannot be sent ENTER_DFU and needs a wired
 * debugger. Every target has a way back that does not depend on the application's GATT server;
 * this module decides WHEN to take it, and the target takes it:
 *
 *   recovery kind               target           RECOVER means                       CONFIRM means
 *   OD_HEALTH_RECOVERY_UPDATER  efr32bg22-slc    enter the Gecko AppLoader (its own  (nothing to confirm:
 *                                                image, own RAM, takes a .gbl)        no rollback slot)
 *   OD_HEALTH_RECOVERY_ROLLBACK nordic-zephyr    reset while unconfirmed: MCUboot    boot_write_img_confirmed()
 *                                                swaps the previous image back
 *                               esp32-idf        esp_ota_mark_app_invalid_rollback_  esp_ota_mark_app_valid_
 *                                                and_reboot(): the bootloader boots   cancel_rollback()
 *                                                the previous OTA slot
 *
 * EVIDENCE ONLY, NEVER PREDICTION. A link "carried ATT" when any GATT event reached the
 * application during it (an MTU exchange, a CCC write, a characteristic write). At each link close:
 *   carried ATT                                   healthy: the dead-link count returns to 0
 *   no ATT, the stack reported a heap failure
 *     during the link, nothing served this boot   RECOVER now (the stack itself said so)
 *   no ATT, link lasted >= OD_HEALTH_DEAD_LINK_MS one more dead link; RECOVER at
 *                                                 OD_HEALTH_DEAD_LINKS consecutive ones
 *   no ATT, shorter                               ignored (scanners connect and drop)
 * A healthy peer answers the MTU request within one connection interval, and ATT itself times a
 * request out at 30 s, so OD_HEALTH_DEAD_LINKS consecutive links of 8 s with no ATT at all are not
 * something a busy radio environment produces. Overlapping links are not judged.
 *
 * ROLLBACK TARGETS ONLY: TRUST. A freshly installed image starts UNCONFIRMED.
 *   SERVED     a GATT write from a peer reached the OpenDisplay characteristic and was
 *              accepted                                                  -> CONFIRM
 *   link close the verdict above, while unconfirmed                      -> RECOVER (revert)
 *              any dead link at all also disables the fallback below
 *   FALLBACK   OD_HEALTH_CONFIRM_FALLBACK_MS of uptime with no dead link  -> CONFIRM
 *   SLEEP      (a target that powers down and boots through its bootloader on wake, ESP32 deep
 *              sleep) before sleeping unconfirmed: stay awake until OD_HEALTH_SLEEP_HOLD_MS of
 *              uptime; then CONFIRM if no dead link was seen (the fallback, shortened because
 *              the wake boot would otherwise revert an image nobody objected to), else RECOVER.
 * WHY THE FALLBACK, AND WHY 24 h: an updater client reconnects within seconds to minutes, so a
 * healthy image confirms by SERVED in normal use. A device nobody contacts after its update would
 * otherwise stay unconfirmed indefinitely, and the next unrelated reset (a battery swap months
 * later) would silently undo the update. After a day of uptime with no link that failed to carry
 * ATT, the image has given no evidence against itself. A GATT-dead image that nobody tries to use
 * for a day is confirmed too -- the accepted residual -- but the first three hosts that try within
 * the day revert it. An image that is already confirmed (a normal boot) never recovers: there is
 * nothing to revert to, so the verdict on a confirmed image is reported as NONE.
 *
 * UPDATER TARGET (BG22): there is no confirmation; the verdict itself is the action, at most
 * OD_HEALTH_FORCED_MAX times per power-on (the count lives in the target's retained RAM, a power
 * cycle clears it). Each entry needs fresh evidence, and the updater leaves only by an update or a
 * reset, so there is no unattended loop.
 *
 * Pure logic, no HAL: the target feeds it events and acts on the returned action.
 * CONTEXT: single loop/main task. Host tests: tests/host/health_test.c (both kinds). */
#ifndef OD_HEALTH_H
#define OD_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OD_HEALTH_DEAD_LINK_MS
#define OD_HEALTH_DEAD_LINK_MS   8000u   /* a link at least this long with no ATT is dead */
#endif
#ifndef OD_HEALTH_DEAD_LINKS
#define OD_HEALTH_DEAD_LINKS     3u      /* consecutive dead links that force recovery */
#endif
#ifndef OD_HEALTH_FORCED_MAX
#define OD_HEALTH_FORCED_MAX     2u      /* forced updater entries per power-on */
#endif
#ifndef OD_HEALTH_CONFIRM_FALLBACK_MS
#define OD_HEALTH_CONFIRM_FALLBACK_MS (24u * 3600u * 1000u)
#endif
#ifndef OD_HEALTH_SLEEP_HOLD_MS
#define OD_HEALTH_SLEEP_HOLD_MS  (15u * 60u * 1000u)  /* awake at most this long, unconfirmed */
#endif
/* Opt-in power-cycle escape (section 4.4 of the doc): k quick power cycles' worth of short boots
 * (the od_claim_mode count), the boot then left on past T_CLEAR, on an UNCLAIMED device only.
 * 0 = off. Must sit strictly between the claim gesture and the factory-reset pattern. */
#ifndef OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS
#define OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS 0u
#endif

enum od_health_recovery {
    OD_HEALTH_RECOVERY_UPDATER = 0,  /* BG22: the verdict enters the updater; nothing to confirm */
    OD_HEALTH_RECOVERY_ROLLBACK      /* Nordic, ESP32: revert an unconfirmed image */
};

enum od_health_action {
    OD_HEALTH_NONE = 0,
    OD_HEALTH_RECOVER,   /* take the target's way back (table above) */
    OD_HEALTH_CONFIRM,   /* rollback targets: mark the running image good */
    OD_HEALTH_HOLD       /* od_health_before_sleep(): do not sleep yet */
};

enum od_health_reason {
    OD_HEALTH_REASON_NONE = 0,
    OD_HEALTH_REASON_STACK_HEAP,   /* stack heap failure on a link that carried no ATT */
    OD_HEALTH_REASON_DEAD_LINKS,   /* OD_HEALTH_DEAD_LINKS consecutive dead links */
    OD_HEALTH_REASON_PATTERN,      /* the opt-in power-cycle escape */
    OD_HEALTH_REASON_SLEEP         /* about to sleep unconfirmed after a dead link */
};

enum od_health_confirm_reason {
    OD_HEALTH_CONFIRM_NONE = 0,
    OD_HEALTH_CONFIRM_ALREADY,     /* confirmed before this boot */
    OD_HEALTH_CONFIRM_SERVED,      /* a GATT write was served */
    OD_HEALTH_CONFIRM_FALLBACK,    /* OD_HEALTH_CONFIRM_FALLBACK_MS with no dead link */
    OD_HEALTH_CONFIRM_SLEEP,       /* OD_HEALTH_SLEEP_HOLD_MS with no dead link, before sleep */
    OD_HEALTH_CONFIRM_EXTERNAL     /* confirmed by another path (an SMP image-confirm command) */
};

/* Plain counters so a debugger can read them from the target's instance. */
struct od_health {
    uint32_t link_open_ms;
    uint32_t heap_failures;    /* total stack heap-allocation failures reported this boot */
    uint16_t links;            /* links judged this boot */
    uint8_t  recovery;         /* enum od_health_recovery */
    uint8_t  open_links;
    uint8_t  dead_links;       /* consecutive */
    uint8_t  forced;           /* forced entries this power-on, including the one returned */
    uint8_t  reason;           /* enum od_health_reason of the last RECOVER, or NONE */
    uint8_t  confirm_reason;   /* enum od_health_confirm_reason of the last CONFIRM */
    bool     overlapped;       /* the current link overlapped another: not judged */
    bool     link_att;         /* the current link carried ATT */
    bool     link_heap_fail;   /* the stack failed a heap allocation during the current link */
    bool     gatt_proven;      /* some link this boot carried ATT */
    bool     confirmed;        /* rollback targets: the running image is trusted */
    bool     dead_seen;        /* some link this boot was dead (or a verdict was RECOVER) */
};

/* Updater kind (BG22). forced_so_far: the target's retained count for this power-on (0 after a
 * power cycle). */
void od_health_init(struct od_health *h, uint8_t forced_so_far);
/* Rollback kind (Nordic, ESP32). already_confirmed: the bootloader's view of the running image
 * (a normal boot is confirmed; the first boot of a test/OTA image is not). */
void od_health_init_rollback(struct od_health *h, bool already_confirmed);

void od_health_link_opened(struct od_health *h, uint32_t now_ms);
void od_health_att_seen(struct od_health *h);
/* n: the num_heap_allocation_failures field of one stack report. */
void od_health_stack_heap_failure(struct od_health *h, uint32_t n);
/* The verdict for the link that just closed: NONE or RECOVER. On RECOVER h->forced has been
 * incremented: an updater target stores it in retained RAM, then enters the updater. */
enum od_health_action od_health_link_closed(struct od_health *h, uint32_t now_ms);

/* A GATT write from a peer reached the application and was accepted (also counts as ATT).
 * CONFIRM once on a rollback target whose image is unconfirmed; otherwise NONE. */
enum od_health_action od_health_served(struct od_health *h);
/* From the loop, any rate: CONFIRM by the 24 h fallback, otherwise NONE. */
enum od_health_action od_health_poll(struct od_health *h, uint32_t uptime_ms);
/* Before a power-down that boots through the bootloader on wake: NONE (sleep), HOLD (stay awake),
 * CONFIRM (confirm, then sleep) or RECOVER. Always NONE on an updater target or once confirmed. */
enum od_health_action od_health_before_sleep(struct od_health *h, uint32_t uptime_ms);
/* The target's confirm write failed: try again on the next evidence. */
void od_health_confirm_failed(struct od_health *h);
/* The image was confirmed by another path (an SMP image-confirm command). */
void od_health_confirmed_externally(struct od_health *h);

/* The opt-in escape: true when this boot ends the update pattern. short_boots is
 * od_security_boot_report.short_boots; `unclaimed` is state == OD_SEC2_UNCLAIMED. The target asks
 * once the boot has lived past OD_CM_T_CLEAR_MS (the pattern can no longer continue). Counts as a
 * forced entry. Always false when OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS is 0. */
bool od_health_update_pattern(struct od_health *h, uint8_t short_boots, bool unclaimed);

#ifdef __cplusplus
}
#endif

#endif /* OD_HEALTH_H */
