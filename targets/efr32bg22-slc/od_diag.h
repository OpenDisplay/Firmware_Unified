#ifndef OD_DIAG_H
#define OD_DIAG_H

/* Target diagnostics (printf to RTT channel 0) by level, set by the CMake cache variable
 * OD_BG22_DIAG:
 *
 *   2  every diagnostic line (developer image)
 *   1  the boot banner and the three "entering bootloader" fatals only, written straight to
 *      RTT by od_rtt.c without the libc formatter (no stdio is linked)
 *   0  nothing
 *
 * A disabled call stays inside `if (0)` so its format string and arguments are still type
 * checked, and -Oz drops the call, the string and every argument computation with it. */

#include <stdio.h>

#ifndef OD_BG22_DIAG
#define OD_BG22_DIAG 2
#endif

#if OD_BG22_DIAG >= 2
#define OD_DIAG(...) printf(__VA_ARGS__)
#else
#define OD_DIAG(...)                                                                         \
  do {                                                                                       \
    if (0) {                                                                                 \
      printf(__VA_ARGS__);                                                                   \
    }                                                                                        \
  } while (0)
#endif

/* OD_DIAG_BANNER(s): one constant line. OD_DIAG_FATAL_SC(what, sc): "[OD] FATAL: <what>
 * failed sc=0x<sc>; entering bootloader". */
#if OD_BG22_DIAG >= 2
#define OD_DIAG_BANNER(s) printf("%s", s)
#define OD_DIAG_FATAL_SC(what, sc)                                                           \
  printf("[OD] FATAL: %s failed sc=0x%04lX; entering bootloader\r\n", (what),               \
         (unsigned long)(sc))
#elif OD_BG22_DIAG == 1 && OD_ENABLE_RTT
void od_diag_line(const char *s);
void od_diag_fatal_sc(const char *what, unsigned long sc);
#define OD_DIAG_BANNER(s) od_diag_line(s)
#define OD_DIAG_FATAL_SC(what, sc) od_diag_fatal_sc((what), (unsigned long)(sc))
#else
#define OD_DIAG_BANNER(s) ((void)(s))
#define OD_DIAG_FATAL_SC(what, sc) ((void)(what), (void)(sc))
#endif

#endif /* OD_DIAG_H */
