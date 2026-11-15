/* Target seam for the shared CMD_READ_IDENT handler (od_cmd_ident.c). */
#ifndef OD_IDENT_APP_H
#define OD_IDENT_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_config;

/* The live parsed config; NULL or !loaded answers display_count = 0. */
const struct od_config *od_ident_app_config(void);

/* 1 on a target that drives some panels from a built-in controller driver described by the
 * config rather than from 0xAE/0xB1 drive sequences (ESP32: IT8951, FastEPD parallel). Such a
 * display does not need migration. 0 (default) leaves the handler exactly as before. */
#ifndef OD_IDENT_NATIVE_PANELS
#define OD_IDENT_NATIVE_PANELS 0
#endif
#if OD_IDENT_NATIVE_PANELS
struct DisplayConfig;
/* True when this firmware drives `d` without drive packets. */
bool od_ident_app_native_panel(const struct DisplayConfig *d);
#endif

/* Response byte 3, OD_IDENT_TARGET_*. Set per target build. */
#ifndef OD_IDENT_TARGET
#error "OD_IDENT_TARGET must name this build's OD_IDENT_TARGET_* value"
#endif

#ifdef __cplusplus
}
#endif

#endif /* OD_IDENT_APP_H */
