# Per-target compile-time profile for shared/ — ONE definition, every consumer.
#
# These macros change the ABI of shared structures: OD_CONFIG_WITH_* #if-gate members of
# struct od_config, OD_CONFIG_MAX_SIZE sizes struct od_config_asm, and OD_TXQ_SLOTS and the
# OD_CAP_* switches size or remove state. A host archive built with a different set is a
# different layout, so a suite compiled that way validates something the firmware does not run —
# it passes, and proves nothing.
#
# So the firmware build and every host archive that claims to represent a target read the list
# from here. A consumer adds only definitions that are genuinely its own (a test-only knob, a
# board flag); it never restates one of these.
#
# Included by shared/sources.cmake, so anything already consuming the source list has these.

# EFR32BG22: 32 KB RAM, no kernel, no PIPE, no partial transfer, no RX ring, single chip select,
# no logging, and no touch/buzzer/Wi-Fi/extended-data hardware.
set(OD_PROFILE_SILABS
    # 3120 rather than 4096: a 16-colour 2.9in config measures 2112 B and each additional
    # display sharing the global waveform slots adds ~493 B, so this covers two displays with
    # margin. The ceiling is NVM3's, not RAM's -- see nvm3_default_config.h. Cost is 1088 B of
    # .memory_manager_heap, which the linker sizes as whatever RAM is left; no flash.
    OD_CONFIG_MAX_SIZE=3120u
    OD_TXQ_SLOTS=3u
    OD_CAP_PIPE=0
    OD_CAP_PARTIAL=0
    OD_CAP_RXQ=0
    OD_CAP_DUAL_CS=0
    OD_CAP_LOG=0
    OD_CONFIG_WITH_TOUCH=0
    # Product decision 2026-08-24: this target will never carry a buzzer.
    # DIVERGENCE_MATRIX 16; ratcheted by check.sh "silabs: BG22 has no buzzer runner".
    OD_CONFIG_WITH_BUZZER=0
    OD_CONFIG_WITH_WIFI=0
    OD_CONFIG_WITH_DATA_EXTENDED=0
    OD_CAP_IMGSTORE=1
    # CMD_DRIVE_WRITE_START: canonical at protocol 2.3, Silabs-first. Routed only by targets with
    # a display path driven from config drive packets; Nordic sets it in its own CMakeLists.
    OD_CAP_DRIVE_WRITE=1
    # CMD_SPI_RAW: Silabs-only by specification, and the opcode every panel in this fleet was
    # brought up through. ESP32 and Nordic set it in their own build files too.
    OD_CAP_SPI_RAW=1
    # CMD_READ_IDENT: plaintext identity from DisplayConfig 0x20, pre-gate. Silabs-first.
    OD_CAP_READ_IDENT=1)

# Compile-time log level, one selector for every target.
#
# The two targets reached the same level by different routes: ESP32's debug board fragments put
# OD_LOG_LEVEL=OD_LOG_DEBUG straight into OD_BOARD_DEFINES and ordinary boards fell through to
# od_log.h's INFO default, while Nordic turned PROFILE=debug into OD_DEBUG_BUILD and then named
# OD_LOG_LEVEL itself. Two directives that agree today drift independently, so the mapping from
# a profile name to the definition lives here and the front doors only supply the name.
#
#   profile  exactly "info" or "debug"; anything else, including empty, is an error.
#   out_var  name of a definition list. Exactly one OD_LOG_LEVEL=... is appended, so an empty
#            destination comes back holding exactly that definition.
#
# A destination that already carries an OD_LOG_LEVEL entry is rejected rather than appended to:
# two of them is a silent precedence question at the compiler, which is the drift this exists to
# stop, and the last one winning is not a contract anybody wrote down. The match covers every
# spelling the compiler would honour, including one nested in a generator expression, plus a
# leading -D and a bare valueless OD_LOG_LEVEL. The latter becomes 1 (OD_LOG_WARN), so it builds
# clean and silently drops every info and debug record.
function(od_select_log_profile profile out_var)
  if("${profile}" STREQUAL "")
    message(FATAL_ERROR "od_select_log_profile: no log profile selected; expected info or debug")
  elseif("${profile}" STREQUAL "info")
    set(_definition OD_LOG_LEVEL=OD_LOG_INFO)
  elseif("${profile}" STREQUAL "debug")
    set(_definition OD_LOG_LEVEL=OD_LOG_DEBUG)
  else()
    message(FATAL_ERROR
            "od_select_log_profile: unknown log profile '${profile}'; expected info or debug")
  endif()

  foreach(_existing IN LISTS ${out_var})
    if("${_existing}" MATCHES "(^|[^A-Za-z0-9_])(-D)?OD_LOG_LEVEL($|[^A-Za-z0-9_])")
      message(FATAL_ERROR
              "od_select_log_profile: ${out_var} already carries ${_existing}")
    endif()
  endforeach()

  set(${out_var} ${${out_var}} ${_definition} PARENT_SCOPE)
endfunction()
