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
#else
#define OD_SEC2_TRACE(fmt, ...) do { } while (0)
#define OD_SEC2_TRACE_BANNER()  do { } while (0)
#endif

#endif /* OD_SEC2_TRACE_H */
