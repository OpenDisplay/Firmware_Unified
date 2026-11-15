/* nfc_reference_cases.inc -- what each shipping target answers on CMD_NFC_ENDPOINT (0x0083) today.
 *
 * THIS TABLE IS A FREEZE, NOT A SPECIFICATION. Every row records behaviour already in the tree,
 * captured before the 0x83 machine moves to shared/ so the promotion can be shown to have
 * preserved it byte for byte. Where a row's expectation looks wrong, it is still what ships: the
 * place to change it is the promotion, with a DIVERGENCE_MATRIX row, never this file.
 *
 * Rows carry a `targets` mask because the two ports genuinely disagree, in two places: the
 * inline-write check order, and what a tag read above the 218-byte cap does -- BG22 refuses where
 * Nordic truncates. Those disagreements are why a shared machine needs decisions written down
 * rather than a file copied, and why neither can be captured as a single row.
 *
 * Bodies are BUILT rather than written out, so a row can carry 120, 218 or 512 payload bytes
 * without a literal of that size, and so the filler is a position-dependent pattern -- which is
 * what lets an assembled record be compared for ORDER and not merely for length. A fake that
 * recorded only a length would stay green on chunks concatenated backwards.
 *
 * Included by nfc_reference_nordic (in nordic_nfc_test.c) and the BG22 suite, which supply the
 * drivers. The table names no target function and links nothing.
 *
 * ONE ROW CLASS IS DELIBERATELY ABSENT AND IS NAMED HERE RATHER THAN LEFT TO BE NOTICED: ESP32's
 * capability-off silence. Its hook answers OD_CMD_UNKNOWN to every input and sends nothing, but
 * od_cmd_app.cpp reaches buzzer, communication, device-control and display-service headers that
 * have no host fakes, so freezing an eight-line stub would mean standing up that whole surface.
 * The same observable is proven against the machine that will actually ship by step 7's
 * nfc_off_test.c and its map/symbol check, which is where the plan already assigns it.
 */

#define REF_NORDIC 0x1u
#define REF_BG22   0x2u
#define REF_BOTH   (REF_NORDIC | REF_BG22)

#define REF_ANY_LEN   0xFFFFu   /* do not assert the forwarded length */
/* Knob tri-state. REF_KEEP must be 0: a designated initializer zero-fills every field a row does
 * not name, so anything else would silently force a knob on every unrelated frame in the table. */
#define REF_KEEP      0
#define REF_OFF       1
#define REF_ON        2

/* Filler is seed+index so content carries its own offset: a chunk written at the wrong place, or
 * assembled out of order, changes bytes rather than only counts. */
#define REF_FILL(seed, i) ((uint8_t)((unsigned)(seed) + (unsigned)(i)))

typedef struct {
    /* --- the frame ------------------------------------------------------------------------ */
    uint8_t  sub;             /* sub-command byte */
    bool     has_header;      /* sub-commands 0x01 and 0x10 carry rec_type + a 16-bit length */
    uint8_t  rec_type;
    uint16_t declared;        /* the 16-bit header field, sent verbatim */
    uint16_t fill_len;        /* payload bytes appended after the header */
    uint8_t  fill_seed;
    bool     omit_sub;        /* send a zero-length body instead */

    /* --- driver knobs applied before the frame -------------------------------------------- */
    int      read_ok;         /* REF_KEEP / REF_OFF / REF_ON */
    int      read_len;        /* applied on every READ frame, including a length of 0 */
    int      write_ok;        /* REF_KEEP / REF_OFF / REF_ON */
    bool     foreign_owner;   /* submit under a different reply tag on the same origin */
    bool     foreign_origin;  /* submit under a different origin entirely */
    bool     reset_first;     /* run the target's transport reset before this frame */
    bool     reply_fails;     /* submit with a spent reservation, so the reply cannot be queued */
    bool     sealed;          /* claim + open a SEC2 session first, so replies are sealed */
    bool     unseal_reply;    /* open the reply and compare the APPLICATION frame it carries */

    /* --- what is expected ----------------------------------------------------------------- */
    uint8_t  reply_head[6];
    uint8_t  reply_head_len;
    uint16_t reply_len;       /* 0 = no frame emitted at all */
    uint8_t  sink_calls;      /* cumulative tag writes after this frame */
    uint16_t sink_len;        /* forwarded length; 0 = not asserted */
    uint8_t  sink_seed;       /* expected filler seed of the forwarded bytes */
    bool     sink_check_data; /* compare every forwarded byte against sink_seed */
    uint8_t  sink_rec_type;   /* the record type the tag should have been handed */
    bool     sink_check_type; /* assert it -- without this, normalising every type to TEXT passes */
    bool     expect_ok;       /* OD_CMD_OK rather than OD_CMD_NACK */
    bool     skip_reply;      /* assert the mutation only -- see the ownership rows */
    bool     skip_reply_head; /* assert the frame length only -- sealed frames are ciphertext */
    uint8_t  reply_fill;      /* expected byte for every payload byte past reply_head_len */
    bool     reply_check_fill;/* compare that payload rather than only the head and length */
} ref_frame_t;

typedef struct {
    const char  *name;
    unsigned     targets;
    ref_frame_t  frames[6];
    uint8_t      n_frames;
} ref_case_t;

#define NACKF(err) .reply_head = { 0xFFu, RESP_NFC_ENDPOINT, 0xFFu, (err) }, \
                   .reply_head_len = 4u, .reply_len = 4u
#define ACKF(st)   .reply_head = { 0x00u, RESP_NFC_ENDPOINT, (st) }, \
                   .reply_head_len = 3u, .reply_len = 3u, .expect_ok = true

/* rec_type 0 is OD_NFC_REC_TEXT and 9 is not a record type at all -- rows meaning to exercise a
 * length check use the former so the type gate cannot be what refused them. */
static const ref_case_t REF_CASES[] = {

/* ---------------------------------------------------------------- sub-command dispatch --- */
{ "empty body is malformed", REF_BOTH, {
    { .omit_sub = true, NACKF(NFC_ERR_MALFORMED) } }, 1u },
{ "unknown sub-command 0x02", REF_BOTH, {
    { .sub = 0x02u, NACKF(NFC_ERR_UNKNOWN_SUBCMD) } }, 1u },
{ "unknown sub-command 0x13", REF_BOTH, {
    { .sub = 0x13u, NACKF(NFC_ERR_UNKNOWN_SUBCMD) } }, 1u },
{ "unknown sub-command 0xFF", REF_BOTH, {
    { .sub = 0xFFu, NACKF(NFC_ERR_UNKNOWN_SUBCMD) } }, 1u },

/* --------------------------------------------------------------------------- READ 0x00 --- */
/* The whole frame is asserted, not its first three bytes: the header carries the record type and
 * a 16-bit length that a malformed response of the right size would get wrong silently, and the
 * tag payload is the fake's 0x5A fill. Both fakes report record type 1. */
#define READ_OK(taglen) \
    .sub = NFC_SUB_READ, .read_len = (taglen), \
    .reply_head = { 0x00u, RESP_NFC_ENDPOINT, 0x80u, 0x01u, \
                    (uint8_t)((taglen) >> 8), (uint8_t)(taglen) }, \
    .reply_head_len = 6u, .reply_len = (uint16_t)(6u + (taglen)), \
    .reply_fill = 0x5Au, .reply_check_fill = true, .expect_ok = true

{ "a read returning no tag bytes at all", REF_BOTH, {
    { READ_OK(0) } }, 1u },
{ "a read of 1 tag byte", REF_BOTH, {
    { READ_OK(1) } }, 1u },
{ "a read of 4 tag bytes", REF_BOTH, {
    { READ_OK(4) } }, 1u },
{ "a read of 120 tag bytes", REF_BOTH, {
    { READ_OK(120) } }, 1u },
{ "a read of 121 tag bytes", REF_BOTH, {
    { READ_OK(121) } }, 1u },
/* 218 is the cap both ports request, so the frame is the 224-byte plaintext maximum. */
{ "a read of exactly the 218-byte cap fills the maximum frame", REF_BOTH, {
    { READ_OK(218) } }, 1u },
/* DIVERGENCE (plan N2b). Above the 218-byte cap the two controller adapters answer differently and
 * the shared machine is explicitly NOT going to normalise them: BG22 refuses, because its staging
 * buffer is smaller than the cap, and Nordic truncates to the cap and answers a full frame. Frozen
 * as two rows so the promotion cannot quietly pick one. */
{ "a read of 219 is refused rather than truncated", REF_BG22, {
    { .sub = NFC_SUB_READ, .read_len = 219, NACKF(NFC_ERR_READ_FAILED) } }, 1u },
{ "a read of 219 is truncated to the 218-byte cap", REF_NORDIC, {
    { .sub = NFC_SUB_READ, .read_len = 219,
      .reply_head = { 0x00u, RESP_NFC_ENDPOINT, 0x80u, 0x01u, 0x00u, 218u },
      .reply_head_len = 6u, .reply_len = 224u,
      .reply_fill = 0x5Au, .reply_check_fill = true, .expect_ok = true } }, 1u },
{ "a failing tag read", REF_BOTH, {
    { .sub = NFC_SUB_READ, .read_ok = REF_OFF, NACKF(NFC_ERR_READ_FAILED) } }, 1u },

/* ------------------------------------------------------------------- inline write 0x01 --- */
{ "inline write shorter than its own header", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .fill_len = 1u, .fill_seed = 0x00u,
      NACKF(NFC_ERR_MALFORMED) } }, 1u },
{ "inline write whose length exactly fills the body", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = OD_NFC_REC_TEXT,
      .declared = 4u, .fill_len = 4u, .fill_seed = 0x31u,
      ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u, .sink_len = 4u,
      .sink_seed = 0x31u, .sink_check_data = true } }, 1u },
{ "inline write of 120 bytes, the client's inline ceiling", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 120u, .fill_len = 120u, .fill_seed = 0x40u,
      ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u, .sink_len = 120u,
      .sink_seed = 0x40u, .sink_check_data = true } }, 1u },
{ "inline write with trailing bytes past the declared length is accepted", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = OD_NFC_REC_TEXT,
      .declared = 2u, .fill_len = 4u, .fill_seed = 0x51u,
      ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u, .sink_len = 2u,
      .sink_seed = 0x51u, .sink_check_data = true } }, 1u },
{ "inline write with an invalid record type and an exactly fitting length", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = 9u,
      .declared = 4u, .fill_len = 4u, NACKF(NFC_ERR_INVALID_REC_TYPE) } }, 1u },
{ "a failing tag write on the inline path", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = OD_NFC_REC_TEXT,
      .declared = 4u, .fill_len = 4u, .write_ok = REF_OFF,
      NACKF(NFC_ERR_TAG_WRITE_FAILED), .sink_calls = 1u, .sink_len = 4u } }, 1u },

/* DIVERGENCE (plan section 3.4 item 4, decided by N4). Nordic tests the length first and answers
 * MALFORMED; BG22 tests the record type first and folds its length test into the sink call, so a
 * valid type with a bad length wears the tag-write error code and an invalid one wears the
 * record-type code. Neither reaches the tag. */
{ "inline write, valid type, length beyond the body", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = OD_NFC_REC_TEXT,
      .declared = 5u, .fill_len = 4u, NACKF(NFC_ERR_MALFORMED) } }, 1u },
{ "inline write, invalid type, length beyond the body", REF_BOTH, {
    { .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = 9u,
      .declared = 5u, .fill_len = 4u, NACKF(NFC_ERR_MALFORMED) } }, 1u },

/* -------------------------------------------------------------------------- START 0x10 --- */
{ "START with an invalid record type", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = 9u, .declared = 8u,
      NACKF(NFC_ERR_INVALID_REC_TYPE) } }, 1u },
{ "START declaring zero bytes", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 0u, NACKF(NFC_ERR_BAD_TOTAL_LEN) } }, 1u },
{ "START declaring more than the 512-byte wire limit", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 513u, NACKF(NFC_ERR_BAD_TOTAL_LEN) } }, 1u },
{ "START declaring exactly the 512-byte wire limit", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 512u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) } }, 1u },

/* ---------------------------------------------------------------- DATA and END 0x11/0x12 --- */
{ "DATA with no active START", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, NACKF(NFC_ERR_CHUNK_NO_START) } }, 1u },
{ "END with no active START", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_END, NACKF(NFC_ERR_CHUNK_NO_START) } }, 1u },
{ "a zero-length DATA is malformed and leaves the assembly intact", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 4u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, NACKF(NFC_ERR_MALFORMED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 4u, .fill_seed = 0x61u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_END, ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u,
      .sink_len = 4u, .sink_seed = 0x61u, .sink_check_data = true } }, 4u },
{ "a chunk past total_len is refused and clears the assembly", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 4u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 5u, NACKF(NFC_ERR_CHUNK_OVERFLOW) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 1u, NACKF(NFC_ERR_CHUNK_NO_START) } }, 3u },
{ "a short END is retryable: the assembly survives and a later END commits in order", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 4u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, .fill_seed = 0x70u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_END, NACKF(NFC_ERR_END_LEN_MISMATCH) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, .fill_seed = 0x72u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    /* seed 0x70 across all four bytes: the second chunk must land AFTER the first. */
    { .sub = NFC_SUB_WRITE_END, ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u,
      .sink_len = 4u, .sink_seed = 0x70u, .sink_check_data = true } }, 5u },
{ "single-byte chunks assemble in order", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 3u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 1u, .fill_seed = 0x80u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 1u, .fill_seed = 0x81u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 1u, .fill_seed = 0x82u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_END, ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u,
      .sink_len = 3u, .sink_seed = 0x80u, .sink_check_data = true } }, 5u },
/* The deployed-scale case: 512 bytes across four 128-byte chunks, compared byte for byte. */
{ "a full 512-byte assembly commits in order", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 512u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 128u, .fill_seed = 0x00u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 128u, .fill_seed = 0x80u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 128u, .fill_seed = 0x00u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 128u, .fill_seed = 0x80u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_END, ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u,
      .sink_len = 512u, .sink_seed = 0x00u, .sink_check_data = true } }, 6u },
{ "a failing tag write on END clears the assembly", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_END, .write_ok = REF_OFF, NACKF(NFC_ERR_TAG_WRITE_FAILED),
      .sink_calls = 1u, .sink_len = 2u },
    { .sub = NFC_SUB_WRITE_END, NACKF(NFC_ERR_CHUNK_NO_START), .sink_calls = 1u } }, 4u },
{ "a replacement START discards a partial assembly and re-arms", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 4u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    /* Had the old two bytes survived, this END would complete the new total instead. */
    { .sub = NFC_SUB_WRITE_END, NACKF(NFC_ERR_END_LEN_MISMATCH) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, .fill_seed = 0x90u,
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_END, ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u,
      .sink_len = 2u, .sink_seed = 0x90u, .sink_check_data = true } }, 6u },

/* --------------------------------------------------------------------- record types --- */
/* All five valid values reach the tag, and the type is forwarded verbatim rather than normalised. */
#define TYPE_OK(t) \
    .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = (t), \
    .declared = 4u, .fill_len = 4u, .fill_seed = 0xB0u, \
    ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u, .sink_len = 4u, \
    .sink_seed = 0xB0u, .sink_check_data = true, \
    .sink_rec_type = (t), .sink_check_type = true

{ "record type TEXT",           REF_BOTH, { { TYPE_OK(OD_NFC_REC_TEXT) } },           1u },
{ "record type URI",            REF_BOTH, { { TYPE_OK(OD_NFC_REC_URI) } },            1u },
{ "record type WELL_KNOWN_RAW", REF_BOTH, { { TYPE_OK(OD_NFC_REC_WELL_KNOWN_RAW) } }, 1u },
{ "record type MIME",           REF_BOTH, { { TYPE_OK(OD_NFC_REC_MIME) } },           1u },
{ "record type RAW_NDEF",       REF_BOTH, { { TYPE_OK(OD_NFC_REC_RAW_NDEF) } },       1u },

/* 5 is one past the last valid value, 6 is beyond it, and 0x80/0xFF probe the high bits -- a
 * range check written as a mask rather than a set would let one of these through. */
#define TYPE_BAD(t) \
    .sub = NFC_SUB_WRITE, .has_header = true, .rec_type = (t), \
    .declared = 4u, .fill_len = 4u, NACKF(NFC_ERR_INVALID_REC_TYPE)

{ "record type 5 is refused",    REF_BOTH, { { TYPE_BAD(5u) } },    1u },
{ "record type 6 is refused",    REF_BOTH, { { TYPE_BAD(6u) } },    1u },
{ "record type 0x80 is refused", REF_BOTH, { { TYPE_BAD(0x80u) } }, 1u },
{ "record type 0xFF is refused", REF_BOTH, { { TYPE_BAD(0xFFu) } }, 1u },
{ "START with record type 0xFF is refused", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = 0xFFu, .declared = 8u,
      NACKF(NFC_ERR_INVALID_REC_TYPE) } }, 1u },

/* --------------------------------------------------------- assembled write lengths --- */
/* One chunk carrying the whole declared total, at each length the client can produce. 218 and 219
 * are here because they are the READ cap: the write path has no such cap, and these rows are what
 * says so. */
#define ASSEMBLE(total, seed) \
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF, \
      .declared = (total), ACKF(NFC_STATUS_CHUNK_ACCEPTED) }, \
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = (total), .fill_seed = (seed), \
      ACKF(NFC_STATUS_CHUNK_ACCEPTED) }, \
    { .sub = NFC_SUB_WRITE_END, ACKF(NFC_STATUS_WRITE_COMMITTED), .sink_calls = 1u, \
      .sink_len = (total), .sink_seed = (seed), .sink_check_data = true }

{ "an assembled write of 1 byte",   REF_BOTH, { ASSEMBLE(1u,   0xC0u) }, 3u },
{ "an assembled write of 120 bytes", REF_BOTH, { ASSEMBLE(120u, 0xC1u) }, 3u },
{ "an assembled write of 121 bytes", REF_BOTH, { ASSEMBLE(121u, 0xC2u) }, 3u },
{ "an assembled write of 218 bytes", REF_BOTH, { ASSEMBLE(218u, 0xC3u) }, 3u },
{ "an assembled write of 219 bytes", REF_BOTH, { ASSEMBLE(219u, 0xC4u) }, 3u },

/* ------------------------------------------------------------------------- ownership --- */
/* NEITHER PORT BINDS AN OWNER TODAY, and that is the baseline this row freezes: a foreign
 * connection can extend and commit somebody else's assembly. Both donors reject it with
 * CHUNK_NO_START. N1 restores the binding, so this row is expected to change at the promotion --
 * which is exactly why it is captured now rather than argued about later. */
/* PROMOTED (N1). Both ports now bind the assembler to the full reply identity, restoring what the
 * donors did and both imports dropped. The rows recording the old permissive behaviour went with
 * the handlers that had it. */
{ "a foreign owner is refused and mutates nothing", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, .fill_seed = 0xA0u, .foreign_owner = true,
      .skip_reply = true },
    { .sub = NFC_SUB_WRITE_END, .foreign_owner = true, .skip_reply = true, .sink_calls = 0u } }, 3u },
{ "a foreign ORIGIN is refused and mutates nothing", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, .fill_seed = 0xA8u, .foreign_origin = true,
      .skip_reply = true },
    { .sub = NFC_SUB_WRITE_END, .foreign_origin = true, .skip_reply = true, .sink_calls = 0u } }, 3u },


/* ---------------------------------------------------------------------- reply failure --- */
/* A spent reservation, so od_txq refuses the frame the handler tried to queue. TODAY EVERY ARM
 * IGNORES THAT: the reply return value is discarded, the verdict is still OK, and on START and
 * DATA the assembler keeps the bytes it staged for a frame the client never saw answered. N6
 * changes all of this, so it is frozen here first. */

/* ------------------------------------------------------------------ protected session --- */
/* The 218-byte cap is applied in BOTH modes on purpose, so the answer's size does not depend on
 * whether the session happens to be encrypted. Sealed, the 224-byte plain frame becomes 241: the
 * D->H record adds [E5][ctr:4] and a 12-byte tag. */
/* The sealed frame is opened by the SEC2 client and the application frame inside it is compared to
 * the plaintext one byte for byte. Asserting 241 alone would pass on any correctly sized
 * ciphertext, including one carrying the wrong record. The 219-byte refusal is a plaintext NACK
 * (od_nfc sends error frames through od_reply_plain), so it stays 4 bytes inside a session. */
{ "the 218-byte cap holds in a protected session", REF_BOTH, {
    { .sub = NFC_SUB_READ, .read_len = 218, .sealed = true, .unseal_reply = true,
      .reply_head = { 0x00u, RESP_NFC_ENDPOINT, 0x80u, 0x01u, 0x00u, 218u },
      .reply_head_len = 6u, .reply_len = 224u + OD_SEC2_D2H_OVERHEAD,
      .reply_fill = 0x5Au, .reply_check_fill = true, .expect_ok = true } }, 1u },
{ "a 219-byte read is refused in a protected session too", REF_BG22, {
    { .sub = NFC_SUB_READ, .read_len = 219, .sealed = true,
      NACKF(NFC_ERR_READ_FAILED) } }, 1u },

/* ----------------------------------------------------------------------------- reset --- */
{ "a transport reset mid-assembly discards it", REF_BOTH, {
    { .sub = NFC_SUB_WRITE_START, .has_header = true, .rec_type = OD_NFC_REC_RAW_NDEF,
      .declared = 4u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, ACKF(NFC_STATUS_CHUNK_ACCEPTED) },
    { .sub = NFC_SUB_WRITE_DATA, .fill_len = 2u, .reset_first = true,
      NACKF(NFC_ERR_CHUNK_NO_START) },
    { .sub = NFC_SUB_WRITE_END, NACKF(NFC_ERR_CHUNK_NO_START) } }, 4u },
};

#define REF_CASE_COUNT (sizeof REF_CASES / sizeof REF_CASES[0])
