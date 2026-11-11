/* od_sec2_trace.h -- TEST-ONLY security trace (claim code, screen kinds, claim-mode and
 * factory-reset steps) to the log console.
 *
 * OD_SEC2_TEST_TRACE is never set by a board fragment or a release script. A target build sets
 * it only behind an explicit acknowledgement (ESP32: -DOD_SEC2_TEST_TRACE=I_ACCEPT_CLAIM_CODE_ON_SERIAL,
 * targets/esp32-idf/main/CMakeLists.txt). The claim code is the one secret claim mode keeps
 * off the radio; a build that prints it to a serial port is for a bench with the port attached
 * and must never reach a user. Off, every macro compiles to nothing. */
#ifndef OD_SEC2_TRACE_H
#define OD_SEC2_TRACE_H

#include "od_log.h"
#include "od_hal_time.h"

#ifndef OD_SEC2_TEST_TRACE
#define OD_SEC2_TEST_TRACE 0
#endif

#if OD_SEC2_TEST_TRACE
#define OD_SEC2_TRACE(fmt, ...) _od_log(OD_LOG_WARN, "[SEC2-TEST] " fmt, ##__VA_ARGS__)
#define OD_SEC2_TRACE_BANNER()                                                                \
    do {                                                                                     \
        od_log_raw("\r\n");                                                                  \
        od_log_raw("################################################################\r\n"); \
        od_log_raw("##  SEC2 TEST-TRACE BUILD -- NOT FOR RELEASE                    ##\r\n"); \
        od_log_raw("##  The claim code and security state are printed on this port. ##\r\n"); \
        od_log_raw("################################################################\r\n"); \
    } while (0)
/* A fixed stall right after a traced storage step, so a bench reset (EN pulse) can land between
 * two steps on purpose: the power-cut tests of SECURITY_V2 § 11 and FACTORY_RESET.md R1-R8. */
#define OD_SEC2_TRACE_PAUSE(what)                                                            \
    do {                                                                                     \
        const uint32_t od_t0_ = od_hal_uptime_ms();                                          \
        _od_log(OD_LOG_WARN, "[SEC2-TEST] PAUSE %s: 3000 ms window for a reset", (what));    \
        while ((uint32_t)(od_hal_uptime_ms() - od_t0_) < 3000u) {                            \
        }                                                                                    \
        _od_log(OD_LOG_WARN, "[SEC2-TEST] PAUSE %s: over", (what));                          \
    } while (0)
/* FNV-1a 64 of a buffer: the trace names what a screen rendered (a key, a URL) by a hash the
 * bench recomputes from its own copy, never by the bytes themselves. */
#include <stddef.h>
#include <stdint.h>
static inline uint64_t od_sec2_trace_fnv64(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 0xcbf29ce484222325ull;
    while (n-- > 0u) {
        h ^= *b++;
        h *= 0x100000001b3ull;
    }
    return h;
}
/* Bench fault hooks (panic, interrupt / task watchdog) read from the console; target code. */
#define OD_SEC2_TRACE_BUILD 1
#else
#define OD_SEC2_TRACE_BUILD 0
#define OD_SEC2_TRACE_PAUSE(what) do { } while (0)
#define OD_SEC2_TRACE(fmt, ...) do { } while (0)
#define OD_SEC2_TRACE_BANNER()  do { } while (0)
#endif

#endif /* OD_SEC2_TRACE_H */
