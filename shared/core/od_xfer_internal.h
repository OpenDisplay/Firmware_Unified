/* Private state shared only by the transfer implementation translation units. */
#ifndef OD_XFER_INTERNAL_H
#define OD_XFER_INTERNAL_H

#include "od_caps.h"
#include "od_color.h"
#include "od_log.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_pump.h"

#include <stddef.h>
#include <stdint.h>

/* The error od_xfer_app_note_start_error() recorded since the last call; 0 if none. */
uint8_t od_xfer_take_start_error(void);

typedef enum {
    OD_XFER_IDLE = 0,
    OD_XFER_DIRECT_FULL,
    OD_XFER_DIRECT_PARTIAL,
    OD_XFER_PIPE_FULL,
    OD_XFER_PIPE_PARTIAL,
    OD_XFER_FATAL,
} od_xfer_mode_t;

#if OD_CAP_PARTIAL
typedef struct {
    uint32_t new_etag;
    uint32_t plane_bytes;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} od_xfer_partial_state_t;
#endif

typedef struct {
    od_xfer_mode_t mode;
    od_reply_t owner;
    uint32_t started_ms;
    uint32_t finalized_ms;       /* when the stream finished, if it did */
    uint32_t expected_bytes;
    uint32_t received_bytes;
    uint32_t written_bytes;
    bool compressed;
    bool finalized;              /* separate because zero is a valid uptime */
    od_color_geometry_t geometry;
#if OD_CAP_PARTIAL
    od_xfer_partial_state_t partial;
#endif
} od_xfer_state_t;

typedef enum {
    OD_XFER_START_OK = 0,
    OD_XFER_START_MALFORMED,
    OD_XFER_START_UNSUPPORTED_FLAGS,
    OD_XFER_START_ETAG_MISMATCH,
    OD_XFER_START_PARTIAL_UNSUPPORTED,
    OD_XFER_START_RECT_BOUNDS,
    OD_XFER_START_RECT_ALIGNMENT,
    OD_XFER_START_PANEL_GEOMETRY,
    OD_XFER_START_SPLIT_LAYOUT,
    OD_XFER_START_ZERO_SIZE,
    OD_XFER_START_SIZE_MISMATCH,
    OD_XFER_START_NEEDS_MIGRATION,
} od_xfer_start_cause_t;

/* Whether a SPLIT_HALVES geometry may start: only where the target said it streams one. */
static inline bool split_halves_ok(const od_xfer_panel_info_t *panel)
{
#if OD_XFER_SPLIT_HALVES
    return panel->split_halves_ok;
#else
    (void)panel;
    return false;
#endif
}

typedef enum {
    OD_XFER_STREAM_OK = 0,
    OD_XFER_STREAM_INVALID,
    OD_XFER_STREAM_INFLATE_FAILED,
    OD_XFER_STREAM_WRITE_FAILED,
    OD_XFER_STREAM_SIZE_EXCEEDED,
} od_xfer_stream_result_t;

typedef enum {
    OD_XFER_TERM_PANEL_PREPARATION = 0,
    OD_XFER_TERM_MALFORMED_STREAM,
    OD_XFER_TERM_SIZE_EXCEEDED,
    OD_XFER_TERM_INVALID_BUFFER,
    OD_XFER_TERM_PANEL_WRITE,
    OD_XFER_TERM_INCOMPLETE,
    OD_XFER_TERM_REPLY_DELIVERY,
    OD_XFER_TERM_BARRIER_ABORTED,
    OD_XFER_TERM_REFRESH_FAILED,
    OD_XFER_TERM_REFRESH_INCOMPLETE,
    OD_XFER_TERM_TIMEOUT,
    OD_XFER_TERM_FRAME_SIZE,
    OD_XFER_TERM_REORDER_FULL,
    OD_XFER_TERM_SEQUENCE_WINDOW,
} od_xfer_terminal_cause_t;

typedef struct od_xfer_terminal_snapshot od_xfer_terminal_snapshot_t;

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
struct od_xfer_terminal_snapshot {
    od_xfer_mode_t mode;
    uint32_t received_bytes;
    uint32_t written_bytes;
    uint32_t expected_bytes;
    uint32_t chunks;
    uint32_t elapsed_ms;
    uint32_t rate_tenths;
    uint32_t ratio_hundredths;
    bool compressed;
    char pipe_suffix[64];
};
#endif

od_xfer_state_t *od_xfer_state(void);
bool od_xfer_owner_matches(const od_cmd_ctx_t *ctx);
void od_xfer_clear_state(void);
void od_xfer_abort_active(od_xfer_abort_reason_t reason, bool clear_etag);
bool od_xfer_peer_warning_allowed(void);
void od_xfer_log_start_refused(const char *kind, od_xfer_start_cause_t cause,
                               int error, bool local_failure);
void od_xfer_log_owner_mismatch(uint16_t opcode);

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
void od_xfer_terminal_capture(od_xfer_terminal_snapshot_t *out);
void od_xfer_terminal_complete(const od_xfer_terminal_snapshot_t *snapshot);
void od_xfer_terminal_failure(const od_xfer_terminal_snapshot_t *snapshot,
                              od_xfer_terminal_cause_t cause, const char *phase,
                              int error, uint32_t offset, uint32_t offered);
#else
#define od_xfer_terminal_failure(snapshot_, cause_, phase_, error_, offset_, offered_) \
    ((void)(snapshot_), (void)(cause_), (void)(phase_), (void)(error_), \
     (void)(offset_), (void)(offered_))
#endif
void od_xfer_fail_active(od_xfer_terminal_cause_t cause, const char *phase, int error,
                         uint32_t offset, uint32_t offered,
                         od_xfer_abort_reason_t reason, bool clear_etag);
od_xfer_terminal_cause_t od_xfer_stream_cause(od_xfer_stream_result_t result);

od_txq_status_t od_xfer_reply_app(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);
void od_xfer_reply_error(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);
void od_xfer_reply_simple_error(const od_cmd_ctx_t *ctx, uint8_t opcode);
#if OD_CAP_PARTIAL
void od_xfer_reply_partial_error(const od_cmd_ctx_t *ctx, uint8_t opcode, uint8_t error);
#endif

void od_xfer_stream_reset(uint32_t expected_bytes);
od_xfer_stream_result_t od_xfer_stream_push(od_span_t input, bool final);
void od_xfer_log_start(void);
void od_xfer_log_chunk(od_span_t payload);
void od_xfer_log_progress(void);

/* Reply-free PIPE operations. od_pipe owns every wire byte; this module owns the transfer,
 * accounting, inflater, panel lifecycle and etag. */
od_xfer_start_cause_t od_xfer_pipe_arm_full(const od_cmd_ctx_t *ctx, uint32_t total,
                                             bool compressed);
#if OD_CAP_PARTIAL
od_xfer_start_cause_t od_xfer_pipe_arm_partial(const od_cmd_ctx_t *ctx, uint32_t total,
                                                bool compressed, uint32_t old_etag,
                                                uint16_t x, uint16_t y, uint16_t width,
                                                uint16_t height, uint8_t *err_out);
#endif
bool od_xfer_pipe_activate(void);
od_xfer_stream_result_t od_xfer_pipe_consume(od_span_t payload);
od_xfer_stream_result_t od_xfer_pipe_finalize(void);
void od_xfer_pipe_mark_finalized(void);
bool od_xfer_pipe_complete(void);
void od_xfer_pipe_enter_fatal(void);
od_xfer_barrier_t od_xfer_pipe_before_refresh(void);
void od_xfer_pipe_barrier_abort(void);
bool od_xfer_pipe_refresh(uint8_t mode, bool has_new_etag, uint32_t new_etag,
                          bool *completed, od_xfer_terminal_snapshot_t *snapshot);
void od_pipe_reset_state(void);
size_t od_pipe_log_suffix(char *buf, size_t size);

od_cmd_result_t od_xfer_direct_data_impl(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_xfer_direct_end_impl(const od_cmd_ctx_t *ctx, od_span_t body);
#if OD_CAP_PARTIAL
od_cmd_result_t od_xfer_partial_data_impl(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_xfer_partial_end_impl(const od_cmd_ctx_t *ctx, od_span_t body);
#endif

#endif /* OD_XFER_INTERNAL_H */
