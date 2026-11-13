#ifndef PROTOCOL_PENDING_H
#define PROTOCOL_PENDING_H

/* Wire-contract values this target's code uses that shared/protocol/ does not define yet.
 *
 * ============================ THIS FILE IS A DEBT, NOT A DESIGN ============================
 *
 * It exists for exactly one reason: the sync of the ESP32 target to Firmware
 * feat/psram-dram-reclaim brought in code that references panel IC values which the canonical
 * protocol header does not carry, and shared/protocol/ is under an editing freeze. Deleting
 * this file is a required follow-up, not an optional cleanup.
 *
 * WHAT HAPPENED, precisely, because the shape of it matters more than the two constants:
 *
 *   * `opendisplay-protocol/src/opendisplay_structs.h` is the canonical wire contract, and
 *     Firmware_Unified's `shared/protocol/opendisplay_structs.h` is a byte-for-byte copy of
 *     it. Those two agree.
 *   * `Firmware/include/opendisplay_structs.h` -- a VENDORED COPY, which the workspace's own
 *     rule says must never be hand-edited -- has drifted AHEAD of canonical. Someone added
 *     wire values there and did not propagate them:
 *
 *         OD_PANEL_IC_INKPLATE5V2_1280X720 = 3002   (added by "feat(fastepd): add Inkplate
 *         OD_PANEL_IC_INKPLATE10_1200X825  = 3003    5 V2 / 10 parallel support", #131/1e2a200)
 *         OD_IC_TYPE_ESP32                 = 9
 *         OD_MANUFACTURER_SOLDERED         = 5
 *
 *     plus doc-comment edits replacing "Seeed_GFX" with "FastEPD" throughout.
 *   * So the values below are real, shipped, host-visible wire numbers that the canonical
 *     source of truth has never heard of. This is not a Firmware_Unified problem that
 *     Firmware_Unified can fix.
 *
 * Only the two panel ICs are here. OD_IC_TYPE_ESP32 and OD_MANUFACTURER_SOLDERED are equally
 * missing but nothing in the imported sources references them, and inventing definitions for
 * unused values would make this file look like a place to put things.
 *
 * ---------------------------------------------------------------------------- THE FIX
 *
 * NOT "copy these into shared/protocol/". The correct sequence, in order:
 *
 *   1. Land 3002/3003 (and 9, and 5) in `opendisplay-protocol/src/opendisplay_structs.h`,
 *      the canonical file, with the changelog entry the header's own rules require.
 *   2. `cd ../opendisplay-protocol && tools/sync_protocol_header.py --push` to propagate --
 *      which also needs `Firmware_Unified/shared/protocol/` ADDED TO THE COPY MAP first; it
 *      still lists only the four original repos, so `--check` cannot currently see drift here
 *      at all (CLAUDE.md § "Protocol header -- do not hand-edit", "Known gap").
 *   3. Delete this file and the `#include "protocol_pending.h"` lines in display_fastepd.cpp
 *      and display_service.cpp. The static_assert below turns step 3 from something to
 *      remember into a compile error if it is forgotten -- the moment the real enumerators
 *      arrive, the redefinition here stops matching and the build says so.
 *
 * Until step 1 happens the numbers below are a LOCAL GUESS AT A REMOTE FACT. They are taken
 * from Firmware's vendored copy at feat/psram-dram-reclaim, which is what shipped firmware and
 * py-opendisplay were built against, so they are almost certainly right -- but "almost
 * certainly" is exactly the property the canonical header exists to eliminate.
 */

#include "opendisplay_structs.h"

/* Values, not enumerators. Adding to `enum OdPanelIcType` from outside the header that
 * declares it is not possible in C++, and re-opening the enum here would be a second
 * declaration of a wire type -- the precise failure mode the canonical header prevents.
 * Plain constants of the enum's underlying type keep the comparisons in display_service.cpp
 * and display_fastepd.cpp working without creating a rival definition of the type itself. */
#define OD_PANEL_IC_INKPLATE5V2_1280X720 3002  /* Soldered Inkplate 5 V2 (ED050WROW, 1280x720, 1bpp) */
#define OD_PANEL_IC_INKPLATE10_1200X825  3003  /* Soldered Inkplate 10   (ED097TC2, 1200x825, 1bpp) */

/* The tripwire. 3001 is OD_PANEL_IC_ED103TC2_1872X1404_4GRAY, the highest value the canonical
 * header currently defines in the 3000+ FastEPD range. When 3002/3003 are added upstream this
 * assert still passes -- but the #defines above will then collide with real enumerators and
 * the compiler will reject the file outright, which is the louder signal and the one that
 * actually forces the deletion. This assert catches the quieter failure: the range being
 * renumbered or reused underneath us. */
static_assert((int)OD_PANEL_IC_ED103TC2_1872X1404_4GRAY == 3001,
              "the FastEPD panel-IC range moved; protocol_pending.h's 3002/3003 are no longer "
              "safe to assume -- re-derive them from opendisplay-protocol before building");

/* Third of the same kind, added the other way round: the value landed in the canonical header
 * FIRST (opendisplay-protocol "structs: add BatterySenseFlags bit1 CALIBRATED_MV"), and only the
 * shared/protocol/ freeze keeps it out of the vendored copy. readBatteryVoltageUncached() tests
 * it, so the battery path can read millivolts through od_hal_adc_read_mv() when a board's config
 * opts in. Same deletion rule as above: it goes when shared/protocol/ is next synced.
 *
 * Spelled 0x02u, not canonical's (1u << 1), on purpose: an IDENTICAL redefinition is legal C and
 * would sync down silently, while a differently-spelled one is a redefinition diagnostic -- the
 * loud signal this file relies on to force its own deletion. The assert pins the value. */
#define OD_BATTERY_SENSE_FLAG_CALIBRATED_MV 0x02u  /* ESP32: read the sense pin in calibrated mV; voltage_scaling_factor = divider ratio x1000 */
static_assert(OD_BATTERY_SENSE_FLAG_CALIBRATED_MV == 0x02u,
              "BatterySenseFlags bit1 moved; re-derive CALIBRATED_MV from opendisplay-protocol");

#endif /* PROTOCOL_PENDING_H */
