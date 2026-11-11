# shared/sources.cmake — the single source list for shared/.
#
# Four consumers include this file:
#
#   targets/esp32-idf/       idf_component_register(SRCS ${OD_SHARED_SOURCES} ...)
#   targets/nordic-zephyr/   target_sources(app PRIVATE ${OD_SHARED_SOURCES})
#   targets/efr32bg22-slc/   cmake_gcc/opendisplay-bg22.cmake's source set (NOT the .slcp: shared/
#                            sits outside the SLC project dir, same as third_party/, so it is
#                            hand-maintained there like every other out-of-tree source)
#   tests/host/              the host unit-test build
#
# It also brings in shared/profiles.cmake, which owns the per-target compile-time profile. The
# source list and the macro set that decides those sources' ABI travel together on purpose.
#
# One list, four consumers, so a file added to shared/ that is not tested is a build
# error in three places rather than an omission nobody notices. That is the whole point
# of the file existing instead of each build globbing — do not replace this with a GLOB.
#
# Paths are relative to this file's directory. Consumers prefix with ${OD_SHARED_DIR}.
#
# HEADER-ONLY FILES HAVE NO ENTRY HERE and that is not the omission this file exists to prevent.
# core/od_span.h and core/od_nonce_window.h are the two today: both are all static inline, so
# there is nothing to compile separately and nothing to link. They still reach every consumer,
# because every one of them takes OD_SHARED_INCLUDE_DIRS and the sources below include them. Do
# not "fix" their absence by inventing an od_span.c -- what the one-list rule guards against is a
# source no target builds, and a header with no source cannot be that.
#
# The list is no longer empty. core/od_adv_control.c is the first entry, landed with its host
# tests (tests/host/adv_control_test.c), which were written against the header before the
# implementation existed.
#
# WHY THE ADVERTISING CONTROLLER AND NOT THE CONFIG PARSER. Decided 2026-08-05
# (docs/NEXT_STEPS_2026-08-05.md D1, docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md): it has no wire
# surface and no vendor coupling, so it establishes the shared-source build and test pattern
# without moving protocol state. It is a narrow infrastructure exception, NOT a reordering of
# the subsystem sequence below — od_config.c is still the first subsystem that parses, stores,
# or alters wire behaviour, and its pre-auth-attack-surface rationale is untouched.
#
# Order of arrival, per docs/MIGRATION.md § "Per-target procedure" step 4:
#   core/od_adv_control.c advertising/lifecycle policy          (LANDED — no wire surface)
#   core/od_config_asm.c  chunked CONFIG_WRITE reassembly        (LANDED — closes F3)
#   core/od_config_tlv.c  config blob walk + CRC                 (LANDED — pre-auth surface)
#   core/od_config.c      the parsed aggregate: instance caps,   (LANDED — the storage half of
#                         zero-key rule, string termination       the parser, downstream of the
#                                                                 walk. Firmware is the authority
#                                                                 per CLAUDE.md; its zero-key
#                                                                 normalisation is promoted and
#                                                                 the other two targets' lack of
#                                                                 it is the divergence settled.)
#   core/od_watchdog.c    strike/safe-mode/breadcrumb policy     (LANDED — called on
#                         esp32-idf and nordic-zephyr, which implement hal/od_hal_wdt.h over the
#                         Task Watchdog and the devicetree watchdog0/gpregret2 nodes
#                         respectively. efr32bg22-slc does not implement it and does not take
#                         this tier. Policy landed before either HAL because it must behave
#                         identically on four chips and is cheap to get wrong in a way only a
#                         field boot-loop reveals. Like od_adv_control.c this has no wire
#                         surface, so it is not a reordering of the protocol subsystem
#                         sequence below.)
#   core/od_advert.c      16-byte MSD build                   (LANDED — moved AHEAD of the
#                         protocol subsystems below, deliberately. It was the only remaining
#                         item needing no HAL and no state machine, so every target can take it
#                         the day it lands, and it changes wire ENCODING only — the same bytes
#                         three targets already broadcast, now pinned by a differential host
#                         test instead of by a comment citing another target's line numbers.
#                         Not a reordering of anything that parses or alters wire BEHAVIOUR.)
#   core/od_session.c     auth, KDF, nonce/replay              (LANDED — moved AHEAD of
#                         od_dispatch.c, deliberately. The dispatcher's own encryption gate is
#                         written in terms of this module: it asks od_session_security_enabled()
#                         and od_session_alive() before routing, and calls od_session_open() /
#                         od_session_seal() around every handler. Landing dispatch first would
#                         mean writing that gate against three targets' private session state and
#                         then rewriting it, so the ordering is a dependency, not a preference.)
#   core/od_txq.c         response queue, reservation, drain   (LANDED — egress before dispatch,
#                         because the dispatcher reserves capacity BEFORE it calls a handler and
#                         cannot be written against a queue that does not exist yet. No wire
#                         surface of its own: it moves finished frames, it does not build them.)
#   core/od_dispatch.c    opcode dispatch, encryption gate      (LANDED — owns the opcode map and
#                         the ordering: reservation, then gate, then decrypt. A promoted opcode's
#                         row names its shared machine directly and leaves the od_cmd_app surface.)
#   core/od_xfer_direct.c 0x70/0x71/0x72                        (LANDED — Phase 2.)
#   core/od_xfer_partial.c 0x76                                 (LANDED — Phase 2.)
#   core/od_pipe.c        0x80-0x82          (LANDED — Phase 3; capability-gated OD_CAP_PIPE, and
#                         BG22 links the three entry points with no state behind them.)
#   core/od_nfc.c         0x0083             (LANDED — Phase 4; capability-gated OD_CAP_NFC, tag
#                         I/O behind od_nfc_app.h. NOT OPTIONAL for a consumer that takes
#                         od_core.c: od_core_reset() names od_nfc_reset() whatever the capability.)
#   core/od_zlib_inflate.c portable streaming zlib inflate     (LANDED — one bounded static
#                         window, shared by all three target families; ESP32 may remap the API
#                         to its ROM-backed tinfl adapter without changing the wire contract.)
#   core/od_config_store.c stored config record framing + CRC-32 (LANDED — the storage half of
#                         the config subsystem, over shared/hal/od_hal_nvs.h. Dormant on
#                         arrival: it changes no target's behaviour until each target's
#                         cutover replaces its own copy of the same framing.)
#   core/od_sensor_sht40.c SHT40 device policy                  (LANDED — config walk, address
#                         candidates, soft reset, both CRC-8 words, conversion, TTL and MSD
#                         packing. Multi-instance, because both ports were.)
#   core/od_color.c       direct-stream color geometry          (LANDED — stateless checked
#                         geometry shared by all three target families; GRAY8 value 9 remains an
#                         explicitly unsupported local placeholder with no wire representation.)

get_filename_component(OD_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

# ----------------------------------------------------------------------------- source tiers ---
#
# The tiers answer ONE question: what must a consumer already have in order to link this source?
# They are NOT a second dimension of policy and NOT a way to opt out of shared code.
#
#   PURE     needs the C standard library and shared/protocol only. Any consumer can take it.
#   HAL_ADV  needs shared/hal/od_hal_adv.h implemented (three link-time C functions).
#   HAL_CRYPTO needs shared/hal/od_hal_crypto.h implemented (prepared AES key + primitives).
#   HAL_WDT  needs shared/hal/od_hal_wdt.h implemented (reset reason, retained byte, arm, feed).
#   HAL_RADIO needs shared/hal/od_hal_radio.h implemented (send one frame, is this tag live).
#   HAL_NVS   needs shared/hal/od_hal_nvs.h implemented (size/read-at-offset/write/erase).
#
# WHY THE SPLIT EXISTS. A target part-way through migration can consume the sources whose HAL it
# has and add the rest as each HAL lands, instead of waiting to take all of shared/ at once. That
# is a real state — targets/nordic-zephyr implements HAL_WDT and HAL_ADV but not every tier, and
# targets/efr32bg22-slc implements neither — and the alternative was
# a target that consumes nothing from shared/ for as long as one unimplemented HAL exists.
#
# THIS DOES NOT WEAKEN THE ONE-LIST RULE. OD_SHARED_SOURCES is COMPOSED from the tiers below, not
# hand-maintained beside them, so a file added to any tier still reaches the host tests and the
# ESP32 build automatically. Adding a source to a tier and forgetting the aggregate is not a
# possible mistake. Do not turn these into GLOBs and do not add a source to OD_SHARED_SOURCES
# directly — every source belongs to exactly one tier.
#
# A tier is a statement about LINKAGE, not about quality or readiness: od_config_tlv.c is PURE and
# also the most wire-sensitive file here.

set(OD_SHARED_SOURCES_PURE
    "${CMAKE_CURRENT_LIST_DIR}/core/od_boot_payload.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_color.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_asm.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_tlv.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_advert.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_log.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_zlib_inflate.c"
)

set(OD_SHARED_SOURCES_HAL_ADV
    "${CMAKE_CURRENT_LIST_DIR}/core/od_adv_control.c"
)

set(OD_SHARED_SOURCES_HAL_CRYPTO
    "${CMAKE_CURRENT_LIST_DIR}/core/od_session.c"
)

set(OD_SHARED_SOURCES_HAL_RADIO
    "${CMAKE_CURRENT_LIST_DIR}/core/od_txq.c"
)

# HAL_NVS is the stored config record's framing, CRC and bounds. It needs no seam beyond the
# medium itself, so it is a HAL tier rather than an APP one.
set(OD_SHARED_SOURCES_HAL_NVS
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_store.c"
)

# APP_SESSION needs MORE than a HAL: the target must supply the od_session_app seam and pair this
# tier with APP_XFER. Dispatch routes the mandatory direct/partial opcode family straight to
# od_xfer, so a consumer taking APP_SESSION must also implement od_xfer_app. APP_XFER in turn
# requires APP_INFLATE. These are source-list dependencies, not optional target policy.
set(OD_SHARED_SOURCES_APP_SESSION
    "${CMAKE_CURRENT_LIST_DIR}/core/od_reply.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_gate.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_config_read.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_dispatch.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_cmd.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_core.c"
)

# APP_RXQ, like APP_SESSION, is named for a SEAM rather than a HAL: the ring needs no driver at
# all. It logs arrivals and drops itself, once for both targets instead of once per transport
# callback; what it cannot answer alone is od_rxq_app.h's two predicates, which a target taking
# this tier must implement. The quiet predicate delegates to APP_XFER when a target composes both,
# but keeping the call behind this seam means APP_RXQ itself still has no hidden source-tier
# dependency. They are referenced only in a build compiled at OD_LOG_DEBUG, so an INFO build links
# without them -- tests/host's rxq seam fixture is built at DEBUG for that reason. This tier
# deliberately does not depend on APP_SESSION.
set(OD_SHARED_SOURCES_APP_RXQ
    "${CMAKE_CURRENT_LIST_DIR}/core/od_rxq.c"
)

# APP_BOOT needs the target's od_boot_app seam. It is kept separate so a target can consume the
# protocol core without linking the optional boot renderer.
set(OD_SHARED_SOURCES_APP_BOOT
    "${CMAKE_CURRENT_LIST_DIR}/core/od_boot_screen.c"
)

# APP_INFLATE binds the target-selected inflater engine to the shared output pump. Targets keep
# that choice because ESP32 Wi-Fi builds use ROM tinfl while the other builds use the portable
# engine. The pump itself owns no storage; callers supply the bounded scratch buffer and sink.
set(OD_SHARED_SOURCES_APP_INFLATE
    "${CMAKE_CURRENT_LIST_DIR}/core/od_zlib_pump.c"
)

# APP_XFER is the portable legacy direct/partial command machine. It calls the target's
# od_xfer_app seam for panel writes, refresh and recovery, and requires APP_INFLATE for compressed
# streams. Every APP_SESSION consumer takes this tier; capability-off behaviour remains inside the
# shared machine rather than removing its linkage.
set(OD_SHARED_SOURCES_APP_XFER
    "${CMAKE_CURRENT_LIST_DIR}/core/od_xfer.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_xfer_direct.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_xfer_partial.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_pipe.c"
)

# APP_NFC is the portable 0x0083 machine. It calls the target's od_nfc_app seam for tag I/O and
# keeps capability-off behaviour inside itself, so a target that declines NFC still links both
# entry points. NOT OPTIONAL FOR A CONSUMER THAT TAKES od_core.c: od_core_reset() names
# od_nfc_reset(), whatever the capability setting.
set(OD_SHARED_SOURCES_APP_NFC
    "${CMAKE_CURRENT_LIST_DIR}/core/od_nfc.c")

# APP_LED is the 0x0073 pattern runner. It calls the target's od_led_app seam for pin writes and
# for the LIVE mode nibble, and it never sleeps -- od_led_service() returns a delay so each target
# arms the scheduler it already has. Every target has LEDs, so there is no capability-off arm.
set(OD_SHARED_SOURCES_APP_LED
    "${CMAKE_CURRENT_LIST_DIR}/core/od_led.c"
)

# APP_BUZZER is the 0x0077 melody runner. Like APP_LED it returns a relative delay and leaves
# scheduling to the target; tone generation stays behind od_buzzer_app because ESP32 uses LEDC
# while Nordic uses a software square-wave timer. BG22 has no buzzer capability and declines it.
set(OD_SHARED_SOURCES_APP_BUZZER
    "${CMAKE_CURRENT_LIST_DIR}/core/od_buzzer.c"
)

# APP_SENSOR is the shared device drivers. Named for a SEAM rather than a HAL because it needs
# od_sensor_app.h -- a yielding delay, the target's MSD block, and bus recovery on a target that
# caches one -- as well as shared/hal/od_hal_i2c.h. A target with no sensors takes neither and
# grows no dummy functions: BG22 declines this tier entirely.
set(OD_SHARED_SOURCES_APP_SENSOR
    "${CMAKE_CURRENT_LIST_DIR}/core/od_sensor_sht40.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_sensor_bq27220.c"
)

# APP_TOUCH is the shared GT911 driver. Named for a seam, like APP_SENSOR: od_touch_app.h carries
# GPIO and the reset delays, because there is no shared GPIO HAL and inventing one to serve a
# single driver would be a larger change than the promotion. A consumer must also provide
# od_hal_i2c and od_adv_app_boost. BG22 takes neither this nor APP_SENSOR.
set(OD_SHARED_SOURCES_APP_TOUCH
    "${CMAKE_CURRENT_LIST_DIR}/core/od_touch_gt911.c"
)

# HAL_LOG owns portable record formatting. Enabled targets must implement od_hal_log and
# od_hal_time; OD_CAP_LOG=0 compiles explicit no-op entry points with no HAL references or state.
set(OD_SHARED_SOURCES_HAL_LOG
    "${CMAKE_CURRENT_LIST_DIR}/core/od_log.c"
)

set(OD_SHARED_SOURCES_HAL_WDT
    "${CMAKE_CURRENT_LIST_DIR}/core/od_watchdog.c"
)

# APP_EPD is the config-driven panel driver. od_epd holds no panel constants: every command
# byte, waveform, pass count and colour code map arrives in the device config, so a new panel is
# a config change rather than a firmware release. The target fills the seam (SPI, D/C, RST,
# BUSY) and supplies the od_epd vtable.
#
# od_cfg_index comes with it and is not optional: od_epd reads packet bodies on demand instead
# of holding them, because one waveform is 539 bytes against 32 KB of RAM on BG22. It needs
# HAL_NVS for the medium.
#
# A config without drive packets drives nothing: there is no fallback panel table, and such a
# device reports "needs migration" (docs/CONFIG_DRIVEN_DISPLAY.md).
set(OD_SHARED_SOURCES_APP_EPD
    "${CMAKE_CURRENT_LIST_DIR}/core/od_cfg_index.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_epd.c"
)

# APP_NET_A is the finding-network advertising policy, network A (od_net_a) and network G /
# FMDN (od_net_g): what makes each network
# active, how the advertised address derives from the key, and the 31-byte record. It needs
# shared/hal/od_hal_adv2.h -- a SECOND advertising set, declinable, because the finding record
# is 31 network-defined bytes on a key-derived address and od_hal_adv installs 16 bytes of MSD
# on the device identity. Only BG22 takes this tier today; the others answer false from
# od_hal_adv2_supported() and link none of it.
# APP_DRIVE binds od_epd to the stored config (od_epd_cfg) and carries the CMD_DRIVE_WRITE_START
# handler over it. Needs APP_EPD and the od_drive_app.h seam. BG22 has its own adapter and
# handler and does not take this tier.
set(OD_SHARED_SOURCES_APP_DRIVE
    "${CMAKE_CURRENT_LIST_DIR}/core/od_cmd_drive.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_epd_cfg.c"
)

# APP_DISPLAY is the config-driven display path every target shares (od_display.h): ordinary and
# partial uploads, multi-pass drive transfers, the boot screen and the image store, all from the
# drive packets. It implements the panel half of od_boot_app.h, so a target taking it supplies
# only the identity half. Needs APP_EPD, APP_DRIVE, APP_BOOT and the od_display_app.h seam.
set(OD_SHARED_SOURCES_APP_DISPLAY
    "${CMAKE_CURRENT_LIST_DIR}/core/od_display.c"
)

# APP_IDENT is the CMD_READ_IDENT (0x0046) handler. Needs the od_ident_app.h seam and an
# OD_IDENT_TARGET definition. Compiles to nothing unless OD_CAP_READ_IDENT.
set(OD_SHARED_SOURCES_APP_IDENT
    "${CMAKE_CURRENT_LIST_DIR}/core/od_cmd_ident.c"
)

# APP_SPI_RAW is the CMD_SPI_RAW wire handler. Needs the od_spi_raw_app.h seam and
# od_epd_desc_diag() (BG22's panel adapter, or od_epd_cfg.c from APP_DRIVE). Compiles to nothing
# unless OD_CAP_SPI_RAW.
set(OD_SHARED_SOURCES_APP_SPI_RAW
    "${CMAKE_CURRENT_LIST_DIR}/core/od_cmd_spi_raw.c"
)

# APP_IMGSTORE is the external SPI NOR image store: a real NOR driver and fixed-slot storage
# over shared/hal/od_hal_spi.h. Needs a flash chip on config-named pins and a target
# implementation of the SPI seam, so it is gated by OD_CAP_IMGSTORE and only BG22 takes it.
set(OD_SHARED_SOURCES_APP_IMGSTORE
    "${CMAKE_CURRENT_LIST_DIR}/core/od_flash_nor.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_imgstore.c"
)

# APP_ACTION is the trigger -> effect table: the generalisation of the
# OD_LED_FLAG_BUTTON_PRESS / NFC flag bits, which spent three of LedFlags' four
# spare bits on single-purpose reactions. Needs od_action_app.h from the target
# and od_cfg_index to read the table on demand; od_cmd_action.c answers
# CMD_ACTION_INFO / CMD_ACTION_FIRE and compiles to nothing unless OD_CAP_ACTION.
# Taken by efr32bg22-slc and esp32-idf.
set(OD_SHARED_SOURCES_APP_ACTION
    "${CMAKE_CURRENT_LIST_DIR}/core/od_action.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_cmd_action.c"
)

set(OD_SHARED_SOURCES_APP_NET_A
    "${CMAKE_CURRENT_LIST_DIR}/core/od_net_a.c"
    "${CMAKE_CURRENT_LIST_DIR}/core/od_net_g.c"
)

# The aggregate: every tier, for consumers that have every HAL (today: the host tests, which fake
# them, and targets/esp32-idf).
set(OD_SHARED_SOURCES
    ${OD_SHARED_SOURCES_PURE}
    ${OD_SHARED_SOURCES_HAL_ADV}
    ${OD_SHARED_SOURCES_HAL_CRYPTO}
    ${OD_SHARED_SOURCES_HAL_RADIO}
    ${OD_SHARED_SOURCES_HAL_NVS}
    ${OD_SHARED_SOURCES_APP_SESSION}
    ${OD_SHARED_SOURCES_APP_RXQ}
    ${OD_SHARED_SOURCES_APP_BOOT}
    ${OD_SHARED_SOURCES_APP_INFLATE}
    ${OD_SHARED_SOURCES_APP_XFER}
    ${OD_SHARED_SOURCES_APP_NFC}
    ${OD_SHARED_SOURCES_APP_LED}
    ${OD_SHARED_SOURCES_APP_BUZZER}
    ${OD_SHARED_SOURCES_APP_SENSOR}
    ${OD_SHARED_SOURCES_APP_TOUCH}
    ${OD_SHARED_SOURCES_HAL_LOG}
    ${OD_SHARED_SOURCES_HAL_WDT}
)

# Public headers live alongside their sources; shared/protocol is the wire contract and is
# a byte-for-byte synced copy (never hand-edited — see CLAUDE.md § "Protocol header").
set(OD_SHARED_INCLUDE_DIRS
    "${OD_SHARED_DIR}"
    "${OD_SHARED_DIR}/protocol"
    "${OD_SHARED_DIR}/core"
    "${OD_SHARED_DIR}/hal"
    "${OD_SHARED_DIR}/../third_party"
)
# core/ and hal/ were missing until the first source landed and exposed it: with an empty
# source list nothing ever included a shared header, so the gap could not present. Left as a
# note rather than silently fixed, because it is the first evidence that this file's "green
# against an empty list" property also means "unexercised against an empty list".
#
# ORDER IS LOAD-BEARING IF A TARGET EVER CARRIES ITS OWN PROTOCOL HEADERS AGAIN. Two shared
# headers pull in the wire contract by its bare name — od_config_asm.h includes
# "opendisplay_protocol.h", od_config_tlv.h includes "opendisplay_structs.h" — and neither
# quoted include resolves relative to shared/core, so whichever -I comes first wins. No target
# tree holds either name today; targets/nordic-zephyr/src/ did, as hand-written subsets of 77
# and 319 lines against 991 and 1242 here, which is how this rule was learnt.
#
# Getting that backwards is not a build error in the general case: od_config_tlv.c derives packet
# body sizes with sizeof on the canonical structs, so a subset that merely differs in layout
# yields a silently WRONG size table — a wire divergence introduced by an include path. A
# consumer whose own include dirs contain either name MUST put OD_SHARED_INCLUDE_DIRS ahead of
# them for the shared sources, per-source if the target's own translation units still need their
# copies. targets/nordic-zephyr/zephyr/CMakeLists.txt does exactly that and says so.

include(${CMAKE_CURRENT_LIST_DIR}/profiles.cmake)
