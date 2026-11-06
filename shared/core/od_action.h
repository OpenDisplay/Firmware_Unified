/* od_action -- trigger -> sequence of effects, from the device config.
 *
 * WHAT THIS REPLACES. Local reactions have accreted as single-purpose flag bits:
 * OD_LED_FLAG_BUTTON_PRESS, OD_BUZZER_FLAG_BUTTON_PRESS, and in Firmware also an
 * NFC flash and an NFC "fancy" flash whose only reason to exist is that the first
 * one could not express a different colour. Between them they spent three of
 * LedFlags' four spare bits, and the pair needed a documented override rule
 * because flag bits cannot express precedence.
 *
 * Those bits are DEAD on this target -- defined in the contract, acted on nowhere --
 * so this does not change any shipped behaviour. It is the mechanism they should
 * have been, and the reason the next such request does not cost another bit.
 *
 * NOT A VM. A table of triggers, each with an ordered list of effects and a delay
 * after each. No jumps, no loops, no addressing, no conditionals. Worst-case
 * duration is step_count * 2.55 s by inspection, which is what lets the loader
 * reject a table that could hold a battery device awake.
 *
 * NO RESIDENT TABLE. Entries are read from the config through od_cfg_index when a
 * trigger fires, the same way the drive packets are: a config this part holds in
 * RAM is a config it cannot afford.
 *
 * EVERY STEP YIELDS, and the enable bit is read LIVE rather than latched at
 * trigger time -- both inherited from od_led.c, and both load-bearing. A sequence
 * whose delays are all zero must still return, or a superloop with no watchdog
 * spins forever on a table any client can write; and an action that cannot be
 * stopped by rewriting the config is an action that can brick a tag in the field.
 */
#ifndef OD_ACTION_H
#define OD_ACTION_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The packet id is OD_PKT_ACTION_TABLE in od_config_tlv.h, with the other packet
 * ids: od_cfg_index has to know to index it, and should not have to include this
 * header to find that out. Length-prefixed, so firmware predating it steps over
 * the packet rather than abandoning the rest of the blob. */

/* Returned by od_action_service() when nothing is pending. */
#define OD_ACTION_IDLE 0xFFFFFFFFu

enum od_action_trigger {
    OD_TRIG_NONE = 0,
    OD_TRIG_BOOT = 1,         /* once, after the stack is up */
    OD_TRIG_BUTTON_PRESS = 2, /* arg = BinaryInputs pin bitmask, 0 = any */
    OD_TRIG_BUTTON_LONG = 3,  /* arg = hold time in centiseconds */
    OD_TRIG_NFC_FIELD = 4,    /* NFC field-detect rising edge */
    OD_TRIG_HOST = 5,         /* fired by command; arg is the host's own id */
    /* arg packs the window as two SIGNED bytes: low = min C, high = max C.
     * ENTER fires on outside->inside, EXIT on inside->outside -- EDGES, not
     * levels. A level would re-fire every poll and turn a warm room into a
     * refresh loop that flattens the battery. */
    OD_TRIG_TEMP_ENTER = 6,
    OD_TRIG_TEMP_EXIT = 7
};

enum od_action_effect {
    OD_EFF_NONE = 0,          /* delay only */
    OD_EFF_LED = 1,           /* arg0 = colour byte, arg1 = brightness 1..16 */
    OD_EFF_LED_STOP = 2,
    OD_EFF_DISPLAY_SLOT = 3,  /* arg0 = image-store slot */
    OD_EFF_BUZZER = 4         /* arg0 = melody id */
};

/* Entry layout on the wire, little-endian:
 *
 *   +0 entry_len   total bytes including this one
 *   +1 trigger
 *   +2 trigger_arg (2)
 *   +4 flags       bit0 = enabled
 *   +5 step_count
 *   +6 steps[step_count], 4 bytes each: effect, arg0, arg1, delay_cs
 *
 * delay_cs is CENTISECONDS, so a step can wait at most 2.55 s and a sequence's
 * worst case is bounded by inspection. That bound is why there is no millisecond
 * resolution: a table has to be judgeable without executing it. */
#define OD_ACTION_ENTRY_HDR 6u
#define OD_ACTION_STEP_LEN  4u
#define OD_ACTION_MAX_STEPS 8u

typedef struct {
    uint8_t entries;        /* entries found */
    uint8_t active;         /* entries this build can run */
    uint8_t inactive;       /* valid shape, trigger or effect not known here */
    uint8_t rejected;       /* malformed: bad length, step count, or argument */
    /* Observability, so a trigger can be confirmed without waiting on whatever
     * it drives -- a display effect takes 20 s to become visible, which is far
     * too slow to characterise an edge against a temperature ramp. */
    uint8_t last_trigger;   /* the trigger that last started a sequence */
    uint8_t fire_count;     /* sequences started since boot, wraps */
    int8_t  last_temp_c;    /* most recent sample, for diagnosing a window */
} od_action_stats_t;

/* Re-read the table. Call after any config write, and once at boot.
 *
 * STRUCTURAL FAULTS ARE REJECTED, UNKNOWN TYPES ARE NOT. An entry running past the
 * packet, a step count that disagrees with entry_len, or a step argument naming a
 * slot or LED that does not exist is a bug and is dropped. An entry whose trigger
 * or effect this firmware simply does not know is kept and marked inactive -- it
 * is newer, not broken, and a firmware update should activate it without the host
 * rewriting anything. */
void od_action_reload(void);

const od_action_stats_t *od_action_stats(void);

/* Fire any entry matching this trigger. Cheap to call from an interrupt-adjacent
 * path: it only records what to start; nothing is driven here. */
void od_action_trigger(uint8_t trigger, uint16_t arg);

/* Advance at most one step. Returns ms until it wants calling again, or
 * OD_ACTION_IDLE. The caller arms whatever timer it already has. */
uint32_t od_action_service(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* OD_ACTION_H */
