/* od_health.h -- is this image able to serve GATT? If it demonstrably is not, hand the device to
 * the bootloader's own updater so it stays updatable (docs/BG22_RAM_BUDGET.md section 4).
 *
 * THE FAILURE IT GUARDS: an image that boots, advertises and accepts a link, but whose GATT server
 * never answers -- on BG22 the Bluetooth stack failing a heap allocation for the connection's ATT
 * bearer, with nothing failing at boot. Such a unit cannot be sent ENTER_DFU and needs a wired
 * debugger. The updater (the Gecko AppLoader on BG22) is a separate image with its own RAM, so
 * entering it restores updatability whatever is wrong with the application.
 *
 * EVIDENCE ONLY, NEVER PREDICTION. A link "carried ATT" when any GATT event reached the
 * application during it (the stack raises gatt_mtu_exchanged on every link whose ATT bearer works,
 * because this firmware requests an MTU exchange at connect). At each link close:
 *   carried ATT                                   healthy: the dead-link count returns to 0
 *   no ATT, the stack reported a heap failure
 *     during the link, nothing served this boot   ENTER_UPDATE now (the stack itself said so)
 *   no ATT, link lasted >= OD_HEALTH_DEAD_LINK_MS one more dead link; ENTER_UPDATE at
 *                                                 OD_HEALTH_DEAD_LINKS consecutive ones
 *   no ATT, shorter                               ignored (scanners connect and drop)
 * A healthy peer answers the MTU request within one connection interval, and ATT itself times a
 * request out at 30 s, so OD_HEALTH_DEAD_LINKS consecutive links of 8 s with no ATT at all are not
 * something a busy radio environment produces. Overlapping links are not judged (this firmware
 * stops connectable advertising while a link is up, so they do not occur in normal operation).
 *
 * BOUNDED: at most OD_HEALTH_FORCED_MAX forced entries per power-on; the count lives in the
 * target's retained RAM (a power cycle clears it). Each entry also needs fresh evidence, and the
 * updater leaves only by an update or a reset, so there is no unattended loop.
 *
 * Pure logic, no HAL: the target feeds it events and acts on the returned verdict.
 * CONTEXT: single loop/main task. */
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
#define OD_HEALTH_DEAD_LINKS     3u      /* consecutive dead links that force the updater */
#endif
#ifndef OD_HEALTH_FORCED_MAX
#define OD_HEALTH_FORCED_MAX     2u      /* forced updater entries per power-on */
#endif
/* Opt-in power-cycle escape (section 4.4 of the doc): k quick power cycles' worth of short boots
 * (the od_claim_mode count), the boot then left on past T_CLEAR, on an UNCLAIMED device only.
 * 0 = off. Must sit strictly between the claim gesture and the factory-reset pattern. */
#ifndef OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS
#define OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS 0u
#endif

enum od_health_action {
    OD_HEALTH_NONE = 0,
    OD_HEALTH_ENTER_UPDATE
};

enum od_health_reason {
    OD_HEALTH_REASON_NONE = 0,
    OD_HEALTH_REASON_STACK_HEAP,   /* stack heap failure on a link that carried no ATT */
    OD_HEALTH_REASON_DEAD_LINKS,   /* OD_HEALTH_DEAD_LINKS consecutive dead links */
    OD_HEALTH_REASON_PATTERN       /* the opt-in power-cycle escape */
};

/* Plain counters so a debugger can read them from the target's instance. */
struct od_health {
    uint32_t link_open_ms;
    uint32_t heap_failures;    /* total stack heap-allocation failures reported this boot */
    uint16_t links;            /* links judged this boot */
    uint8_t  open_links;
    uint8_t  dead_links;       /* consecutive */
    uint8_t  forced;           /* forced entries this power-on, including the one returned */
    uint8_t  reason;           /* enum od_health_reason of the last ENTER_UPDATE, or NONE */
    bool     overlapped;       /* the current link overlapped another: not judged */
    bool     link_att;         /* the current link carried ATT */
    bool     link_heap_fail;   /* the stack failed a heap allocation during the current link */
    bool     gatt_proven;      /* some link this boot carried ATT */
};

/* forced_so_far: the target's retained count for this power-on (0 after a power cycle). */
void od_health_init(struct od_health *h, uint8_t forced_so_far);
void od_health_link_opened(struct od_health *h, uint32_t now_ms);
void od_health_att_seen(struct od_health *h);
/* n: the num_heap_allocation_failures field of one stack report. */
void od_health_stack_heap_failure(struct od_health *h, uint32_t n);
/* The verdict for the link that just closed. On ENTER_UPDATE h->forced has been incremented:
 * the target stores it in retained RAM, then enters the updater. */
enum od_health_action od_health_link_closed(struct od_health *h, uint32_t now_ms);

/* The opt-in escape: true when this boot ends the update pattern. short_boots is
 * od_security_boot_report.short_boots; `unclaimed` is state == OD_SEC2_UNCLAIMED. The target asks
 * once the boot has lived past OD_CM_T_CLEAR_MS (the pattern can no longer continue). Counts as a
 * forced entry. Always false when OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS is 0. */
bool od_health_update_pattern(struct od_health *h, uint8_t short_boots, bool unclaimed);

#ifdef __cplusplus
}
#endif

#endif /* OD_HEALTH_H */
