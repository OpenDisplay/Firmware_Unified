/* The reset-cause byte kept in DEEP_ANA_REG1 (od_tlsr_hal.c), decoded and encoded here so the
 * host test (tests/host/tlsr_reset_state_test.c) covers it. Only a power cycle clears the
 * register. Layout:
 *   [7:4] a mark, written on every boot, so any other value means power was applied (a
 *         brownout included: this part cannot tell the two apart):
 *           0xA  no boot-screen render in progress
 *           0xB  a boot-screen render in progress
 *           0xC  a render was in progress at an unplanned reset since power was applied;
 *                kept until the next power cycle
 *   [3]   set just before a planned reboot
 *   [2:0] unplanned resets (watchdog, reset pin) since power was applied, saturating at 7
 * Only 0xC stops the boot screen, so an unrelated reset (the reset pin, a watchdog outside a
 * render) does not hide it or the key-state redraw until a power cycle. */
#ifndef OD_TLSR_RESET_STATE_H
#define OD_TLSR_RESET_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define OD_TLSR_RS_IDLE      0xA0u
#define OD_TLSR_RS_RENDERING 0xB0u
#define OD_TLSR_RS_DIED      0xC0u
#define OD_TLSR_RS_MARK_MASK 0xF0u
#define OD_TLSR_RS_PLANNED   0x08u
#define OD_TLSR_RS_COUNT     0x07u

enum od_tlsr_rs_cause { OD_TLSR_RS_POWER_ON, OD_TLSR_RS_PLANNED_BOOT, OD_TLSR_RS_UNPLANNED };

struct od_tlsr_rs {
    enum od_tlsr_rs_cause cause;
    uint8_t unplanned;
    bool render_died;
};

/* The register's value at boot into the state it describes. */
static inline void od_tlsr_rs_decode(uint8_t reg, struct od_tlsr_rs *s)
{
    const uint8_t mark = (uint8_t)(reg & OD_TLSR_RS_MARK_MASK);
    const bool planned = (reg & OD_TLSR_RS_PLANNED) != 0u;

    s->cause = OD_TLSR_RS_POWER_ON;
    s->unplanned = 0u;
    s->render_died = false;
    if (mark != OD_TLSR_RS_IDLE && mark != OD_TLSR_RS_RENDERING && mark != OD_TLSR_RS_DIED) {
        return;
    }
    s->unplanned = (uint8_t)(reg & OD_TLSR_RS_COUNT);
    if (planned) {
        s->cause = OD_TLSR_RS_PLANNED_BOOT;
    } else {
        s->cause = OD_TLSR_RS_UNPLANNED;
        if (s->unplanned < OD_TLSR_RS_COUNT) {
            s->unplanned++;
        }
    }
    s->render_died = mark == OD_TLSR_RS_DIED || (mark == OD_TLSR_RS_RENDERING && !planned);
}

/* The value to write for that state: rendering while a boot-screen render runs, planned just
 * before a deliberate reboot. */
static inline uint8_t od_tlsr_rs_encode(const struct od_tlsr_rs *s, bool rendering, bool planned)
{
    const uint8_t mark = s->render_died ? OD_TLSR_RS_DIED
                       : rendering      ? OD_TLSR_RS_RENDERING
                                        : OD_TLSR_RS_IDLE;

    return (uint8_t)(mark | (planned ? OD_TLSR_RS_PLANNED : 0u) | (s->unplanned & OD_TLSR_RS_COUNT));
}

#endif
