/* Target seam for the shared CMD_READ_IDENT handler (od_cmd_ident.c). */
#ifndef OD_IDENT_APP_H
#define OD_IDENT_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_config;

/* The live parsed config; NULL or !loaded answers display_count = 0. */
const struct od_config *od_ident_app_config(void);

/* Response byte 3, OD_IDENT_TARGET_*. Set per target build. */
#ifndef OD_IDENT_TARGET
#error "OD_IDENT_TARGET must name this build's OD_IDENT_TARGET_* value"
#endif

#ifdef __cplusplus
}
#endif

#endif /* OD_IDENT_APP_H */
