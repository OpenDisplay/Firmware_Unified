#ifndef ENCRYPTION_STATE_H
#define ENCRYPTION_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "structs.h"

/* The parsed 0x27 this firmware runs on. The SEC2 state is file-static in shared/core/od_sec2.c. */
extern struct SecurityConfig &securityConfig;
extern bool encryptionInitialized;

#endif
