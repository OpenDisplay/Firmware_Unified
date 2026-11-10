#include "od_xfer_internal.h"

#include "od_log.h"
#include "od_log_budget.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static od_xfer_state_t s_xfer;
static volatile uint8_t s_log_quiet;
static bool s_sink_failed;

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
/* The last sink write, read only by the terminal failure record. */
static uint32_t s_sink_offset;
static uint32_t s_sink_offered;
#define SINK_NOTE(off, n) (s_sink_offset = (off), s_sink_offered = (n))
#else
#define SINK_NOTE(off, n) ((void)(off), (void)(n))
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
static struct {
    uint32_t chunks;
    bool terminal_emitted;
} s_xfer_diag;
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static struct {
    uint16_t last_len;
    uint8_t last_step;
    uint8_t last_head_len;
    uint8_t last_head[16];
} s_xfer_debug;
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN
static od_log_budget_t s_peer_warning_budget;
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static void log_hex(char *buf, size_t size, const uint8_t *data, uint8_t len)
{
    size_t pos = 0u;
    uint8_t i;

    if (size == 0u) {
        return;
    }
    buf[0] = '\0';
    for (i = 0u; i < len && pos < size; ++i) {
        const int written = snprintf(buf + pos, size - pos, i == 0u ? "%02X" : " %02X",
                                     data[i]);
        if (written < 0 || (size_t)written >= size - pos) {
            break;
        }
        pos += (size_t)written;
    }
}
#endif

od_xfer_state_t *od_xfer_state(void)
{
    return &s_xfer;
}

bool od_xfer_active(void)
{
    return s_xfer.mode != OD_XFER_IDLE;
}

bool od_xfer_owns_hardware(void)
{
    return s_xfer.mode != OD_XFER_IDLE && s_xfer.mode != OD_XFER_FATAL;
}

bool od_xfer_frames_may_arrive(void)
{
    return s_xfer.mode != OD_XFER_IDLE;
}

bool od_xfer_owner(od_reply_t *out)
{
    if (!od_xfer_active() || out == NULL) {
        return false;
    }
    *out = s_xfer.owner;
    return true;
}

bool od_xfer_log_quiet(uint16_t opcode)
{
    if (opcode != CMD_DIRECT_WRITE_DATA && opcode != CMD_PIPE_WRITE_DATA) {
        return false;
    }
    return __atomic_load_n(&s_log_quiet, __ATOMIC_ACQUIRE) != 0u;
}

bool od_xfer_owner_matches(const od_cmd_ctx_t *ctx)
{
    return ctx != NULL && od_reply_same(&ctx->rp, &s_xfer.owner);
}

void od_xfer_clear_state(void)
{
    memset(&s_xfer, 0, sizeof s_xfer);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    memset(&s_xfer_diag, 0, sizeof s_xfer_diag);
#endif
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    memset(&s_xfer_debug, 0, sizeof s_xfer_debug);
#endif
    s_xfer.mode = OD_XFER_IDLE;
    __atomic_store_n(&s_log_quiet, 0u, __ATOMIC_RELEASE);
    od_pipe_reset_state();
}

void od_xfer_log_start(void)
{
    /* INFO, not DEBUG: this is the one line that says a transfer was admitted and on what
     * terms. The sender decides compression per transfer via the START header flag, so the
     * boot-time transmission_modes dump cannot answer it -- without the mode here a slow push
     * is ambiguous between "sent raw" and "compressed, link-limited". */
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    od_log_info("DW start: %u bytes expected, %s", (unsigned)s_xfer.expected_bytes,
                s_xfer.compressed ? "zlib streaming" : "raw (uncompressed)");
#endif
}

void od_xfer_log_chunk(od_span_t payload)
{
    if (!od_span_valid(payload) || payload.n == 0u) {
        return;
    }
    __atomic_store_n(&s_log_quiet, 1u, __ATOMIC_RELEASE);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    if (s_xfer_diag.chunks < UINT32_MAX) {
        s_xfer_diag.chunks++;
    }
#endif
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    s_xfer_debug.last_len = payload.n > UINT16_MAX ? UINT16_MAX : (uint16_t)payload.n;
    s_xfer_debug.last_head_len = payload.n < sizeof s_xfer_debug.last_head
        ? (uint8_t)payload.n : (uint8_t)sizeof s_xfer_debug.last_head;
    memcpy(s_xfer_debug.last_head, payload.p, s_xfer_debug.last_head_len);
    if (s_xfer_diag.chunks == 1u) {
        char hex[64];
        uint32_t estimate;

        log_hex(hex, sizeof hex, s_xfer_debug.last_head, s_xfer_debug.last_head_len);
        od_log_debug("DW frame 1: %u bytes: %s", (unsigned)payload.n, hex);
        if (s_xfer.expected_bytes != 0u) {
            estimate = ((s_xfer.expected_bytes - 1u) / (uint32_t)payload.n) + 1u;
            od_log_debug("DW expecting ~%u chunks", (unsigned)estimate);
        }
    }
#else
    (void)payload;
#endif
}

void od_xfer_log_progress(void)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    uint32_t percent;
    uint8_t step;

    if (s_xfer.expected_bytes == 0u) {
        return;
    }
    percent = (uint32_t)(((uint64_t)s_xfer.written_bytes * 100u) / s_xfer.expected_bytes);
    if (percent >= 100u) {
        return;
    }
    step = (uint8_t)(percent / 5u);
    if (step <= s_xfer_debug.last_step) {
        return;
    }
    s_xfer_debug.last_step = step;
    od_log_debug("DW %u%% (%u chunks, %u/%u bytes)", (unsigned)percent,
                 (unsigned)s_xfer_diag.chunks, (unsigned)s_xfer.written_bytes,
                 (unsigned)s_xfer.expected_bytes);
#endif
}

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static void log_final_frame(void)
{
    char hex[64];

    log_hex(hex, sizeof hex, s_xfer_debug.last_head, s_xfer_debug.last_head_len);
    od_log_debug("DW final frame %u: %u bytes: %s", (unsigned)s_xfer_diag.chunks,
                 (unsigned)s_xfer_debug.last_len, hex);
}
#else
#define log_final_frame() ((void)0)
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN
bool od_xfer_peer_warning_allowed(void)
{
    return od_log_budget_allows(&s_peer_warning_budget, od_xfer_app_now_ms(), 5000u);
}
#else
bool od_xfer_peer_warning_allowed(void) { return false; }
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
static const char *start_cause_text(od_xfer_start_cause_t cause)
{
    switch (cause) {
    case OD_XFER_START_MALFORMED: return "malformed request";
    case OD_XFER_START_UNSUPPORTED_FLAGS: return "unsupported flags";
    case OD_XFER_START_ETAG_MISMATCH: return "etag mismatch";
    case OD_XFER_START_PARTIAL_UNSUPPORTED: return "partial update unsupported";
    case OD_XFER_START_RECT_BOUNDS: return "rectangle out of bounds";
    case OD_XFER_START_RECT_ALIGNMENT: return "rectangle not byte-aligned";
    case OD_XFER_START_PANEL_GEOMETRY: return "panel geometry unavailable";
    case OD_XFER_START_SPLIT_LAYOUT: return "split-panel layout unsupported";
    case OD_XFER_START_ZERO_SIZE: return "zero declared size";
    case OD_XFER_START_SIZE_MISMATCH: return "declared size mismatch";
    case OD_XFER_START_OK: return "accepted";
    }
    return "";
}
#endif

void od_xfer_log_start_refused(const char *kind, od_xfer_start_cause_t cause,
                               int error, bool local_failure)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    const char *text = start_cause_text(cause);

    if (local_failure) {
        od_log_error("%s START failed: %s", kind, text);
        return;
    }
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN
    if (!od_xfer_peer_warning_allowed()) {
        return;
    }
    if (error >= 0) {
        od_log_warn("%s START refused: %s (error=0x%02X)", kind, text,
                    (unsigned)error);
    } else {
        od_log_warn("%s START refused: %s", kind, text);
    }
#else
    (void)error;
#endif
#else
    (void)kind;
    (void)cause;
    (void)error;
    (void)local_failure;
#endif
}

void od_xfer_log_owner_mismatch(uint16_t opcode)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN
    if (od_xfer_peer_warning_allowed()) {
        od_log_warn("Transfer frame refused: owner mismatch (opcode=0x%04X)",
                    (unsigned)opcode);
    }
#else
    (void)opcode;
#endif
}

od_xfer_terminal_cause_t od_xfer_stream_cause(od_xfer_stream_result_t result)
{
    switch (result) {
    case OD_XFER_STREAM_INFLATE_FAILED: return OD_XFER_TERM_MALFORMED_STREAM;
    case OD_XFER_STREAM_WRITE_FAILED: return OD_XFER_TERM_PANEL_WRITE;
    case OD_XFER_STREAM_SIZE_EXCEEDED: return OD_XFER_TERM_SIZE_EXCEEDED;
    case OD_XFER_STREAM_INVALID:
    case OD_XFER_STREAM_OK:
        return OD_XFER_TERM_INVALID_BUFFER;
    }
    return OD_XFER_TERM_INVALID_BUFFER;
}

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
static const char *mode_text(od_xfer_mode_t mode)
{
    switch (mode) {
    case OD_XFER_DIRECT_FULL: return "direct full";
    case OD_XFER_DIRECT_PARTIAL: return "direct partial";
    case OD_XFER_PIPE_FULL: return "PIPE full";
    case OD_XFER_PIPE_PARTIAL: return "PIPE partial";
    case OD_XFER_FATAL: return "fatal";
    case OD_XFER_IDLE: return "idle";
    }
    return "fatal";
}

static const char *terminal_cause_text(od_xfer_terminal_cause_t cause)
{
    switch (cause) {
    case OD_XFER_TERM_PANEL_PREPARATION: return "panel preparation failed";
    case OD_XFER_TERM_MALFORMED_STREAM: return "malformed compressed stream";
    case OD_XFER_TERM_SIZE_EXCEEDED: return "size limit exceeded";
    case OD_XFER_TERM_INVALID_BUFFER: return "invalid internal buffer";
    case OD_XFER_TERM_PANEL_WRITE: return "panel write failed";
    case OD_XFER_TERM_INCOMPLETE: return "incomplete stream";
    case OD_XFER_TERM_REPLY_DELIVERY: return "response delivery failed";
    case OD_XFER_TERM_BARRIER_ABORTED: return "refresh barrier aborted";
    case OD_XFER_TERM_REFRESH_FAILED: return "refresh invocation failed";
    case OD_XFER_TERM_REFRESH_INCOMPLETE: return "refresh did not complete";
    case OD_XFER_TERM_TIMEOUT: return "timeout";
    case OD_XFER_TERM_FRAME_SIZE: return "frame exceeds negotiated size";
    case OD_XFER_TERM_REORDER_FULL: return "reorder queue full";
    case OD_XFER_TERM_SEQUENCE_WINDOW: return "sequence outside negotiated window";
    }
    return "invalid internal buffer";
}

static bool terminal_is_error(od_xfer_terminal_cause_t cause)
{
    return cause == OD_XFER_TERM_PANEL_PREPARATION
        || cause == OD_XFER_TERM_INVALID_BUFFER
        || cause == OD_XFER_TERM_PANEL_WRITE
        || cause == OD_XFER_TERM_REPLY_DELIVERY
        || cause == OD_XFER_TERM_REFRESH_FAILED
        || cause == OD_XFER_TERM_TIMEOUT;
}

static uint32_t saturate_u64(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
static uint32_t bytes_kb_tenths(uint32_t bytes)
{
    return (uint32_t)(((uint64_t)bytes * 10u + 512u) / 1024u);
}
#endif

static void appendf(char *buf, size_t size, const char *fmt, ...)
{
    size_t used;
    va_list ap;

    if (buf == NULL || size == 0u) {
        return;
    }
    used = strlen(buf);
    if (used >= size - 1u) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);
}

static void terminal_capture_at(od_xfer_terminal_snapshot_t *out, uint32_t now_ms)
{
    uint64_t scaled;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->mode = s_xfer.mode;
    out->received_bytes = s_xfer.received_bytes;
    out->written_bytes = s_xfer.written_bytes;
    out->expected_bytes = s_xfer.expected_bytes;
    out->chunks = s_xfer_diag.chunks;
    out->elapsed_ms = (s_xfer.finalized ? s_xfer.finalized_ms : now_ms)
        - s_xfer.started_ms;
    out->compressed = s_xfer.compressed;
    if (out->elapsed_ms != 0u) {
        scaled = ((uint64_t)out->written_bytes * 10000u
                  + (uint64_t)512u * out->elapsed_ms)
            / ((uint64_t)1024u * out->elapsed_ms);
        out->rate_tenths = saturate_u64(scaled);
    }
    if (out->compressed && out->received_bytes != 0u) {
        scaled = ((uint64_t)out->written_bytes * 100u + out->received_bytes / 2u)
            / out->received_bytes;
        out->ratio_hundredths = saturate_u64(scaled);
    }
    (void)od_pipe_log_suffix(out->pipe_suffix, sizeof out->pipe_suffix);
    log_final_frame();
}

void od_xfer_terminal_capture(od_xfer_terminal_snapshot_t *out)
{
    terminal_capture_at(out, od_xfer_app_now_ms());
}

static void emit_message(int level, const char *message)
{
    if (level == OD_LOG_ERROR) {
        od_log_error("%s", message);
    } else if (level == OD_LOG_WARN) {
        od_log_warn("%s", message);
    } else {
        od_log_info("%s", message);
    }
}

void od_xfer_terminal_complete(const od_xfer_terminal_snapshot_t *snapshot)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    char message[256];
    uint32_t received_kb;
    uint32_t written_kb;
    uint32_t expected_kb;

    if (snapshot == NULL || s_xfer_diag.terminal_emitted) {
        return;
    }
    received_kb = bytes_kb_tenths(snapshot->received_bytes);
    written_kb = bytes_kb_tenths(snapshot->written_bytes);
    expected_kb = bytes_kb_tenths(snapshot->expected_bytes);
    if (snapshot->compressed) {
        (void)snprintf(message, sizeof message,
                       "DW complete: %s rx=%u.%01uKB wr=%u.%01u/%u.%01uKB n=%u t=%u.%01us "
                       "r=%u.%01uKB/s z=%u.%02ux%s",
                       mode_text(snapshot->mode), (unsigned)(received_kb / 10u),
                       (unsigned)(received_kb % 10u), (unsigned)(written_kb / 10u),
                       (unsigned)(written_kb % 10u), (unsigned)(expected_kb / 10u),
                       (unsigned)(expected_kb % 10u),
                       (unsigned)snapshot->chunks,
                       (unsigned)(snapshot->elapsed_ms / 1000u),
                       (unsigned)((snapshot->elapsed_ms % 1000u) / 100u),
                       (unsigned)(snapshot->rate_tenths / 10u),
                       (unsigned)(snapshot->rate_tenths % 10u),
                       (unsigned)(snapshot->ratio_hundredths / 100u),
                       (unsigned)(snapshot->ratio_hundredths % 100u), snapshot->pipe_suffix);
    } else {
        (void)snprintf(message, sizeof message,
                       "DW complete: %s rx=%u.%01uKB wr=%u.%01u/%u.%01uKB n=%u "
                       "t=%u.%01us r=%u.%01uKB/s%s",
                       mode_text(snapshot->mode), (unsigned)(received_kb / 10u),
                       (unsigned)(received_kb % 10u), (unsigned)(written_kb / 10u),
                       (unsigned)(written_kb % 10u), (unsigned)(expected_kb / 10u),
                       (unsigned)(expected_kb % 10u),
                       (unsigned)snapshot->chunks,
                       (unsigned)(snapshot->elapsed_ms / 1000u),
                       (unsigned)((snapshot->elapsed_ms % 1000u) / 100u),
                       (unsigned)(snapshot->rate_tenths / 10u),
                       (unsigned)(snapshot->rate_tenths % 10u), snapshot->pipe_suffix);
    }
    if (od_xfer_active()) {
        s_xfer_diag.terminal_emitted = true;
    }
    emit_message(OD_LOG_INFO, message);
#else
    (void)snapshot;
#endif
}

void od_xfer_terminal_failure(const od_xfer_terminal_snapshot_t *snapshot,
                              od_xfer_terminal_cause_t cause, const char *phase,
                              int error, uint32_t offset, uint32_t offered)
{
    char message[256];
    char details[80] = "";
    const bool pipe_failure = snapshot != NULL && snapshot->pipe_suffix[0] != '\0';

    if (snapshot == NULL || s_xfer_diag.terminal_emitted) {
        return;
    }
    /* The PIPE state suffix localizes its DATA failures within the fixed record budget. */
    if (phase != NULL && !pipe_failure) {
        appendf(details, sizeof details, " phase=%s", phase);
    }
    if (error >= 0) {
        appendf(details, sizeof details, " error=0x%02X", (unsigned)error);
    }
    if (cause == OD_XFER_TERM_PANEL_WRITE && !pipe_failure) {
        if (offered == 0u) {
            offset = s_sink_offset;
            offered = s_sink_offered;
        }
        appendf(details, sizeof details, " offset=%u offered=%u",
                (unsigned)offset, (unsigned)offered);
    }
    if (snapshot->compressed) {
        appendf(details, sizeof details, " zlib");
    }
    (void)snprintf(message, sizeof message,
                   "DW failed: cause=%s mode=%s rx=%u written=%u/%u chunks=%u elapsed=%u ms%s%s",
                   terminal_cause_text(cause), mode_text(snapshot->mode),
                   (unsigned)snapshot->received_bytes, (unsigned)snapshot->written_bytes,
                   (unsigned)snapshot->expected_bytes, (unsigned)snapshot->chunks,
                   (unsigned)snapshot->elapsed_ms, details, snapshot->pipe_suffix);
    if (od_xfer_active()) {
        s_xfer_diag.terminal_emitted = true;
    }
    emit_message(terminal_is_error(cause) ? OD_LOG_ERROR : OD_LOG_WARN, message);
}

static void terminal_ended(const char *outcome, const char *cause)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    od_xfer_terminal_snapshot_t snapshot;
    char message[256];

    if (s_xfer_diag.terminal_emitted) {
        return;
    }
    od_xfer_terminal_capture(&snapshot);
    if (cause != NULL) {
        (void)snprintf(message, sizeof message,
                       "DW ended: outcome=%s cause=%s mode=%s rx=%u written=%u/%u chunks=%u "
                       "elapsed=%u ms%s", outcome, cause, mode_text(snapshot.mode),
                       (unsigned)snapshot.received_bytes, (unsigned)snapshot.written_bytes,
                       (unsigned)snapshot.expected_bytes, (unsigned)snapshot.chunks,
                       (unsigned)snapshot.elapsed_ms, snapshot.pipe_suffix);
    } else {
        (void)snprintf(message, sizeof message,
                       "DW ended: outcome=%s mode=%s rx=%u written=%u/%u chunks=%u elapsed=%u ms%s",
                       outcome, mode_text(snapshot.mode), (unsigned)snapshot.received_bytes,
                       (unsigned)snapshot.written_bytes, (unsigned)snapshot.expected_bytes,
                       (unsigned)snapshot.chunks, (unsigned)snapshot.elapsed_ms,
                       snapshot.pipe_suffix);
    }
    if (od_xfer_active()) {
        s_xfer_diag.terminal_emitted = true;
    }
    emit_message(OD_LOG_INFO, message);
#else
    (void)outcome;
    (void)cause;
#endif
}
#endif

void od_xfer_fail_active(od_xfer_terminal_cause_t cause, const char *phase, int error,
                         uint32_t offset, uint32_t offered,
                         od_xfer_abort_reason_t reason, bool clear_etag)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    od_xfer_terminal_snapshot_t snapshot;

    od_xfer_terminal_capture(&snapshot);
    od_xfer_terminal_failure(&snapshot, cause, phase, error, offset, offered);
#else
    (void)cause;
    (void)phase;
    (void)error;
    (void)offset;
    (void)offered;
#endif
    od_xfer_abort_active(reason, clear_etag);
}

void od_xfer_replace_active(void)
{
    if (od_xfer_active()) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
        terminal_ended("replaced", NULL);
#endif
        if (od_xfer_owns_hardware()) {
            od_xfer_app_abort(OD_XFER_ABORT_REPLACED);
        }
        od_xfer_clear_state();
    }
}

void od_xfer_abort_active(od_xfer_abort_reason_t reason, bool clear_etag)
{
    if (od_xfer_owns_hardware()) {
        od_xfer_app_abort(reason);
    }
#if OD_CAP_PARTIAL
    if (clear_etag) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)clear_etag;
#endif
    od_xfer_clear_state();
}

void od_xfer_reset(void)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    if (od_xfer_active()) {
        terminal_ended("aborted", "reset");
    }
#endif
    od_xfer_abort_active(OD_XFER_ABORT_RESET, false);
}

bool od_xfer_report_timeout(uint32_t now_ms, uint32_t limit_ms)
{
    if (!od_xfer_active() || limit_ms == 0u
        || (uint32_t)(now_ms - s_xfer.started_ms) <= limit_ms) {
        return false;
    }
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    {
        od_xfer_terminal_snapshot_t snapshot;
        terminal_capture_at(&snapshot, now_ms);
        od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_TIMEOUT, NULL, -1, 0u, 0u);
    }
#endif
    return true;
}

od_txq_status_t od_xfer_reply_app(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    if (ctx == NULL) {
        return OD_TXQ_INVARIANT;
    }
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

void od_xfer_reply_error(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    if (ctx != NULL) {
        (void)od_reply_plain(ctx->r, &ctx->rp, frame, len);
    }
}

void od_xfer_reply_simple_error(const od_cmd_ctx_t *ctx, uint8_t opcode)
{
    const uint8_t frame[] = { RESP_NACK, opcode };
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
}

#if OD_CAP_PARTIAL
void od_xfer_reply_partial_error(const od_cmd_ctx_t *ctx, uint8_t opcode, uint8_t error)
{
    const uint8_t frame[] = { RESP_NACK, opcode, error, 0u };

    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
}
#endif

static bool stream_sink(void *ctx, od_mut_span_t bytes)
{
    od_xfer_state_t *state = (od_xfer_state_t *)ctx;
    uint32_t consumed;

    if (state == NULL || bytes.n == 0u || bytes.n > UINT32_MAX) {
        return false;
    }
    if (state->written_bytes > state->expected_bytes
        || bytes.n > (size_t)(state->expected_bytes - state->written_bytes)) {
        return false;
    }
    consumed = od_xfer_app_write(state->written_bytes, od_mut_span_const(bytes));
    SINK_NOTE(state->written_bytes, (uint32_t)bytes.n);
    if (consumed != (uint32_t)bytes.n) {
        s_sink_failed = true;
        return false;
    }
    state->written_bytes += consumed;
    return true;
}

void od_xfer_stream_reset(uint32_t expected_bytes)
{
    od_zlib_pump_reset(expected_bytes);
}

od_xfer_stream_result_t od_xfer_stream_push(od_span_t input, bool final)
{
    od_mut_span_t scratch = od_xfer_app_inflate_scratch();
    od_zlib_pump_status_t status;

    if (!od_span_valid(input) || !od_mut_span_valid(scratch) || scratch.n == 0u) {
        return OD_XFER_STREAM_INVALID;
    }
    s_sink_failed = false;
    SINK_NOTE(0u, 0u);
    status = od_zlib_pump_push(input, final, scratch, stream_sink, &s_xfer);
    if (status == OD_ZLIB_PUMP_ERROR) {
        return s_sink_failed ? OD_XFER_STREAM_WRITE_FAILED : OD_XFER_STREAM_INFLATE_FAILED;
    }
    return (!final || status == OD_ZLIB_PUMP_DONE)
        ? OD_XFER_STREAM_OK : OD_XFER_STREAM_INFLATE_FAILED;
}

od_xfer_start_cause_t od_xfer_pipe_arm_full(const od_cmd_ctx_t *ctx, uint32_t total,
                                             bool compressed)
{
    od_xfer_panel_info_t panel;

    if (ctx == NULL || s_xfer.mode != OD_XFER_IDLE) {
        return OD_XFER_START_MALFORMED;
    }
    if (total == 0u) {
        return OD_XFER_START_ZERO_SIZE;
    }
    od_xfer_app_prepare_start();
    memset(&panel, 0, sizeof panel);
    if (!od_xfer_app_panel_info(&panel) || panel.geometry.total_bytes == 0u) {
        return OD_XFER_START_PANEL_GEOMETRY;
    }
    if (panel.geometry.layout == OD_COLOR_LAYOUT_SPLIT_HALVES) {
        return OD_XFER_START_SPLIT_LAYOUT;
    }
    if (total != panel.geometry.total_bytes) {
        return OD_XFER_START_SIZE_MISMATCH;
    }

    s_xfer.mode = OD_XFER_PIPE_FULL;
    s_xfer.owner = ctx->rp;
    s_xfer.started_ms = od_xfer_app_now_ms();
    s_xfer.expected_bytes = total;
    s_xfer.compressed = compressed;
    s_xfer.geometry = panel.geometry;
    if (compressed) {
        od_xfer_stream_reset(total);
    }
    return OD_XFER_START_OK;
}

#if OD_CAP_PARTIAL
od_xfer_start_cause_t od_xfer_pipe_arm_partial(const od_cmd_ctx_t *ctx, uint32_t total,
                                                bool compressed, uint32_t old_etag,
                                                uint16_t x, uint16_t y, uint16_t width,
                                                uint16_t height, uint8_t *err_out)
{
    od_xfer_panel_info_t panel;
    od_color_geometry_t rect;
    uint32_t plane_bytes;

    if (err_out != NULL) {
        *err_out = OD_ERR_PIPE_START_BAD_HEADER;
    }
    if (ctx == NULL || s_xfer.mode != OD_XFER_IDLE) {
        return OD_XFER_START_MALFORMED;
    }
    if (total == 0u) {
        return OD_XFER_START_ZERO_SIZE;
    }
    od_xfer_app_prepare_start();
    memset(&panel, 0, sizeof panel);
    if (!od_xfer_app_panel_info(&panel)) {
        od_xfer_app_set_displayed_etag(0u);
        return OD_XFER_START_PANEL_GEOMETRY;
    }
    if (!panel.partial_enabled || !panel.geometry.partial_supported) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED;
        }
        return OD_XFER_START_PARTIAL_UNSUPPORTED;
    }
    if (old_etag == 0u || old_etag != od_xfer_app_displayed_etag()) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_ETAG_MISMATCH;
        }
        return OD_XFER_START_ETAG_MISMATCH;
    }
    if (width == 0u || height == 0u
        || (uint32_t)x + width > panel.width
        || (uint32_t)y + height > panel.height) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_RECT_INVALID;
        }
        return OD_XFER_START_RECT_BOUNDS;
    }
    if ((x & 7u) != 0u || (width & 7u) != 0u) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_RECT_INVALID;
        }
        return OD_XFER_START_RECT_ALIGNMENT;
    }
    if (od_color_direct_geometry(OD_COLOR_SCHEME_MONO, width, height, &rect) != OD_COLOR_OK
        || rect.part_bytes[0] == 0u || rect.part_bytes[0] > UINT32_MAX / 2u) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_RECT_INVALID;
        }
        return OD_XFER_START_RECT_BOUNDS;
    }
    plane_bytes = rect.part_bytes[0];
    if (total != plane_bytes * 2u) {
        od_xfer_app_set_displayed_etag(0u);
        if (err_out != NULL) {
            *err_out = OD_ERR_PIPE_START_SIZE_MISMATCH;
        }
        return OD_XFER_START_SIZE_MISMATCH;
    }

    s_xfer.mode = OD_XFER_PIPE_PARTIAL;
    s_xfer.owner = ctx->rp;
    s_xfer.started_ms = od_xfer_app_now_ms();
    s_xfer.expected_bytes = total;
    s_xfer.compressed = compressed;
    s_xfer.geometry = panel.geometry;
    s_xfer.partial.plane_bytes = plane_bytes;
    s_xfer.partial.x = x;
    s_xfer.partial.y = y;
    s_xfer.partial.width = width;
    s_xfer.partial.height = height;
    if (compressed) {
        od_xfer_stream_reset(total);
    }
    return OD_XFER_START_OK;
}
#endif

bool od_xfer_pipe_activate(void)
{
    bool ok = false;

    od_xfer_log_start();
    switch (s_xfer.mode) {
    case OD_XFER_PIPE_FULL:
        ok = od_xfer_app_begin_full(&s_xfer.geometry);
        break;
#if OD_CAP_PARTIAL
    case OD_XFER_PIPE_PARTIAL:
        ok = od_xfer_app_begin_partial(s_xfer.partial.x, s_xfer.partial.y,
                                       s_xfer.partial.width, s_xfer.partial.height,
                                       s_xfer.partial.plane_bytes);
        break;
#else
    case OD_XFER_PIPE_PARTIAL:
        break;
#endif
    case OD_XFER_IDLE:
    case OD_XFER_DIRECT_FULL:
    case OD_XFER_DIRECT_PARTIAL:
    case OD_XFER_FATAL:
        break;
    }
    if (!ok) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
        {
            od_xfer_terminal_snapshot_t snapshot;
            od_xfer_terminal_capture(&snapshot);
            od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_PANEL_PREPARATION,
                                     "START", -1, 0u, 0u);
        }
#endif
        od_xfer_pipe_enter_fatal();
    }
    return ok;
}

od_xfer_stream_result_t od_xfer_pipe_consume(od_span_t payload)
{
    uint32_t remaining;
    uint32_t consumed;

    if (!od_span_valid(payload) || payload.n == 0u || payload.n > UINT32_MAX
        || (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL)) {
        return OD_XFER_STREAM_INVALID;
    }
    if (payload.n > UINT32_MAX - s_xfer.received_bytes) {
        return OD_XFER_STREAM_SIZE_EXCEEDED;
    }
    od_xfer_log_chunk(payload);
    s_xfer.received_bytes += (uint32_t)payload.n;
    if (s_xfer.compressed) {
        {
            const od_xfer_stream_result_t result = od_xfer_stream_push(payload, false);
            if (result != OD_XFER_STREAM_OK) {
                return result;
            }
        }
        od_xfer_log_progress();
        return OD_XFER_STREAM_OK;
    }
    if (s_xfer.written_bytes > s_xfer.expected_bytes) {
        return OD_XFER_STREAM_SIZE_EXCEEDED;
    }
    remaining = s_xfer.expected_bytes - s_xfer.written_bytes;
    if (s_xfer.mode == OD_XFER_PIPE_PARTIAL && payload.n > remaining) {
        return OD_XFER_STREAM_SIZE_EXCEEDED;
    }
    payload = od_span_take(payload, payload.n < remaining ? payload.n : remaining);
    if (payload.n == 0u) {
        return OD_XFER_STREAM_OK;
    }
    SINK_NOTE(s_xfer.written_bytes, (uint32_t)payload.n);
    consumed = od_xfer_app_write(s_xfer.written_bytes, payload);
    if (consumed != (uint32_t)payload.n) {
        return OD_XFER_STREAM_WRITE_FAILED;
    }
    s_xfer.written_bytes += consumed;
    od_xfer_log_progress();
    return OD_XFER_STREAM_OK;
}

od_xfer_stream_result_t od_xfer_pipe_finalize(void)
{
    if (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL) {
        return OD_XFER_STREAM_INVALID;
    }
    if (!s_xfer.compressed) {
        return OD_XFER_STREAM_OK;
    }
    return s_xfer.received_bytes != 0u
        ? od_xfer_stream_push(od_span_none(), true) : OD_XFER_STREAM_INFLATE_FAILED;
}

void od_xfer_pipe_mark_finalized(void)
{
    s_xfer.finalized_ms = od_xfer_app_now_ms();
    s_xfer.finalized = true;
}

bool od_xfer_pipe_complete(void)
{
    return (s_xfer.mode == OD_XFER_PIPE_FULL || s_xfer.mode == OD_XFER_PIPE_PARTIAL)
        && s_xfer.written_bytes == s_xfer.expected_bytes;
}

void od_xfer_pipe_enter_fatal(void)
{
    const bool partial = s_xfer.mode == OD_XFER_PIPE_PARTIAL;

    if (od_xfer_owns_hardware()) {
        od_xfer_app_abort(OD_XFER_ABORT_STREAM_FAILED);
    }
#if OD_CAP_PARTIAL
    if (partial) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)partial;
#endif
    if (s_xfer.mode != OD_XFER_IDLE) {
        s_xfer.mode = OD_XFER_FATAL;
    }
}

od_xfer_barrier_t od_xfer_pipe_before_refresh(void)
{
    if (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL) {
        return OD_XFER_BARRIER_ABORT;
    }
    return od_xfer_app_before_refresh(&s_xfer.owner);
}

void od_xfer_pipe_barrier_abort(void)
{
    const od_reply_t owner = s_xfer.owner;
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    od_xfer_terminal_snapshot_t snapshot;

    od_xfer_terminal_capture(&snapshot);
    od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_BARRIER_ABORTED, "END", -1, 0u, 0u);
#endif
#if OD_CAP_PARTIAL
    if (s_xfer.mode == OD_XFER_PIPE_PARTIAL) {
        od_xfer_app_set_displayed_etag(0u);
    }
#endif
    od_xfer_clear_state();
    od_xfer_app_barrier_abort(&owner);
}

bool od_xfer_pipe_refresh(uint8_t mode, bool has_new_etag, uint32_t new_etag,
                          bool *completed, od_xfer_terminal_snapshot_t *snapshot)
{
    const bool partial = s_xfer.mode == OD_XFER_PIPE_PARTIAL;
    bool refreshed;

    if (completed == NULL
        || (s_xfer.mode != OD_XFER_PIPE_FULL && s_xfer.mode != OD_XFER_PIPE_PARTIAL)) {
        return false;
    }
    refreshed = od_xfer_app_refresh(mode, completed);
    if (!refreshed) {
        od_xfer_fail_active(OD_XFER_TERM_REFRESH_FAILED, "END", -1, 0u, 0u,
                            OD_XFER_ABORT_REFRESH_FAILED, partial || has_new_etag);
        return false;
    }
#if OD_CAP_PARTIAL
    if (partial) {
        od_xfer_app_set_displayed_etag(*completed && has_new_etag && new_etag != 0u
                                      ? new_etag : 0u);
    } else if (*completed) {
        od_xfer_app_set_displayed_etag(has_new_etag && new_etag != 0u ? new_etag : 0u);
    } else if (has_new_etag) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)partial;
    (void)has_new_etag;
    (void)new_etag;
#endif
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    od_xfer_terminal_capture(snapshot);
#else
    (void)snapshot;
#endif
    od_xfer_clear_state();
    return true;
}

od_cmd_result_t od_xfer_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (!od_span_valid(body)) {
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_DATA_ACK);
        return OD_CMD_NACK;
    }
    switch (s_xfer.mode) {
    case OD_XFER_DIRECT_FULL:
        return od_xfer_direct_data_impl(ctx, body);
#if OD_CAP_PARTIAL
    case OD_XFER_DIRECT_PARTIAL:
        return od_xfer_partial_data_impl(ctx, body);
#else
    case OD_XFER_DIRECT_PARTIAL:
        return OD_CMD_OK;
#endif
    case OD_XFER_IDLE:
    case OD_XFER_PIPE_FULL:
    case OD_XFER_PIPE_PARTIAL:
    case OD_XFER_FATAL:
        return OD_CMD_OK;
    }
    return OD_CMD_NACK;
}

od_cmd_result_t od_xfer_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (!od_span_valid(body)) {
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_END_ACK);
        return OD_CMD_NACK;
    }
    switch (s_xfer.mode) {
    case OD_XFER_DIRECT_FULL:
        return od_xfer_direct_end_impl(ctx, body);
#if OD_CAP_PARTIAL
    case OD_XFER_DIRECT_PARTIAL:
        return od_xfer_partial_end_impl(ctx, body);
#else
    case OD_XFER_DIRECT_PARTIAL:
        return OD_CMD_OK;
#endif
    case OD_XFER_IDLE:
    case OD_XFER_PIPE_FULL:
    case OD_XFER_PIPE_PARTIAL:
    case OD_XFER_FATAL:
        return OD_CMD_OK;
    }
    return OD_CMD_NACK;
}
