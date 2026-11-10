/* od_xfer_app.h -- target hardware seam for the shared direct/partial transfer machine.
 *
 * Shared code owns wire parsing, ownership, byte accounting, compression and replies. Targets
 * own panel setup, controller-plane side effects, refresh and transport recovery. Every offered
 * write is non-empty and carries its pre-write logical stream offset; returning fewer bytes than
 * offered refuses the transfer.
 */
#ifndef OD_XFER_APP_H
#define OD_XFER_APP_H

#include "od_caps.h"
#include "od_color.h"
#include "od_span.h"
#include "od_txq.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    od_color_geometry_t geometry;
    uint16_t width;
    uint16_t height;
    bool partial_enabled;
    /* The config has a display but no drive packets for it (od_display_needs_migration()).
     * Every upload start refuses with OD_ERR_NEEDS_MIGRATION before the panel is touched. */
    bool needs_migration;
} od_xfer_panel_info_t;

/* NACK data byte for "this config cannot drive the panel; write a config with drive packets".
 * Shared by the 0x70 START ([FF][70][08][00]), the 0x76 START (OD_ERR_PARTIAL_* namespace) and
 * the PIPE 0x80 START (OD_ERR_PIPE_START_* namespace): 0x08 is free in both. Provisional until
 * opendisplay-protocol assigns it. */
#ifndef OD_ERR_NEEDS_MIGRATION
#define OD_ERR_NEEDS_MIGRATION 0x08u
#endif

typedef enum {
    OD_XFER_BARRIER_PROCEED = 0,
    OD_XFER_BARRIER_ABORT,
} od_xfer_barrier_t;

/* The target owns panel power policy. In particular, ESP32 distinguishes failures that must
 * force the panel off from replacement/incomplete paths that may release it warm. */
typedef enum {
    OD_XFER_ABORT_REPLACED = 0,
    OD_XFER_ABORT_START_FAILED,
    OD_XFER_ABORT_STREAM_FAILED,
    OD_XFER_ABORT_INCOMPLETE,
    OD_XFER_ABORT_PIPE_INCOMPLETE,
    OD_XFER_ABORT_REPLY_FAILED,
    OD_XFER_ABORT_REFRESH_FAILED,
    OD_XFER_ABORT_RESET,
} od_xfer_abort_reason_t;

void od_xfer_app_prepare_start(void);
bool od_xfer_app_panel_info(od_xfer_panel_info_t *out);
bool od_xfer_app_begin_full(const od_color_geometry_t *geometry);
#if OD_CAP_PARTIAL
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes);
#endif
uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data);
od_mut_span_t od_xfer_app_inflate_scratch(void);
void od_xfer_app_abort(od_xfer_abort_reason_t reason);
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner);
void od_xfer_app_barrier_abort(const od_reply_t *owner);
bool od_xfer_app_refresh(uint8_t mode, bool *completed);
#if OD_CAP_PARTIAL
uint32_t od_xfer_app_displayed_etag(void);
void od_xfer_app_set_displayed_etag(uint32_t etag);
#endif
uint32_t od_xfer_app_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_XFER_APP_H */
