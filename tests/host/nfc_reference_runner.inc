/* nfc_reference_runner.inc -- builds each REF_CASES frame and checks it.
 *
 * Included after nfc_reference_cases.inc by a driver that has already defined:
 *   REF_TARGET_MASK      REF_NORDIC or REF_BG22
 *   ref_setup()          restore the target to a fresh connection with default knobs
 *   ref_submit(b, n)     run one frame through the production hook, drain, return the verdict
 *   ref_reply_count()    frames emitted by the last submit
 *   ref_reply(i)         { len, data } of emitted frame i
 *   ref_sink_*()         the tag fake's call count, forwarded length and forwarded bytes
 *   ref_knob_read/write  apply a driver knob
 *   ref_reset_transport()
 *   ref_authenticate()   claim the device and open a SEC2 session for the next frame
 *   ref_open_reply(f, n, out, out_n)  open a sealed D->H frame with that session's client
 *   CHECK(), CASE()
 *
 * The drivers differ only in those seams; the checking is here so the two targets cannot drift
 * into asserting different things about the same frozen row.
 */

static uint16_t ref_build(const ref_frame_t *fr, uint8_t *out, uint16_t cap)
{
    uint16_t n = 0u;

    if (fr->omit_sub) {
        return 0u;
    }
    out[n++] = fr->sub;
    if (fr->has_header) {
        out[n++] = fr->rec_type;
        out[n++] = (uint8_t)(fr->declared >> 8);
        out[n++] = (uint8_t)fr->declared;
    }
    for (uint16_t i = 0; i < fr->fill_len && n < cap; ++i) {
        out[n++] = REF_FILL(fr->fill_seed, i);
    }
    return n;
}

static void ref_run_table(void)
{
    static uint8_t body[600];

    for (unsigned c = 0; c < REF_CASE_COUNT; ++c) {
        const ref_case_t *rc = &REF_CASES[c];

        if ((rc->targets & REF_TARGET_MASK) == 0u) {
            continue;
        }
        CASE(rc->name);
        ref_setup();
        for (unsigned f = 0; f < rc->n_frames; ++f) {
            const ref_frame_t *fr = &rc->frames[f];
            uint16_t n;
            od_cmd_result_t verdict;

            if (fr->read_ok != REF_KEEP)  { ref_knob_read_ok(fr->read_ok == REF_ON); }
            /* Keyed on the sub-command, not on a nonzero length: a read of zero tag bytes is a
             * real row, so 0 cannot double as "leave the knob alone" here. */
            if (fr->sub == NFC_SUB_READ)  { ref_knob_read_len((uint16_t)fr->read_len); }
            if (fr->write_ok != REF_KEEP) { ref_knob_write_ok(fr->write_ok == REF_ON); }
            if (fr->reset_first)          { ref_reset_transport(); }
            if (fr->sealed)               { ref_authenticate(); }

            n = ref_build(fr, body, (uint16_t)sizeof body);
            verdict = ref_submit(body, n, fr->foreign_owner, fr->foreign_origin,
                                 fr->reply_fails);

            CHECK(verdict == (fr->expect_ok ? OD_CMD_OK : OD_CMD_NACK));
            if (!fr->skip_reply) {
                /* A sealed frame is compared for MEANING: open it and check the application
                 * frame inside, so the assertion is the same one the plaintext row makes. The
                 * wire length is checked too, but on its own it would pass on any correctly
                 * sized ciphertext. */
                uint8_t plain[256];
                uint16_t plain_len = 0u;
                const uint8_t *app = NULL;
                uint16_t app_len = 0u;

                CHECK(ref_reply_count() == (fr->reply_len == 0u ? 0u : 1u));
                if (fr->reply_len != 0u && ref_reply_count() == 1u) {
                    CHECK(ref_reply_len(0) == fr->reply_len);
                    if (fr->unseal_reply) {
                        CHECK(ref_open_reply(ref_reply_data(0), ref_reply_len(0),
                                             plain, &plain_len));
                        app = plain;
                        app_len = plain_len;
                    } else {
                        app = ref_reply_data(0);
                        app_len = ref_reply_len(0);
                    }
                    if (!fr->skip_reply_head && app_len >= fr->reply_head_len) {
                        CHECK(memcmp(app, fr->reply_head, fr->reply_head_len) == 0);
                    }
                    /* Past the head the frame is tag payload, and it is checked rather than
                     * assumed: a response of the right length carrying the wrong bytes -- or the
                     * wrong record type or encoded length in the head above -- would otherwise
                     * pass as frozen behaviour. */
                    if (fr->reply_check_fill && app != NULL) {
                        unsigned bad = 0u;

                        for (uint16_t i = fr->reply_head_len; i < app_len; ++i) {
                            if (app[i] != fr->reply_fill) {
                                ++bad;
                            }
                        }
                        CHECK(bad == 0u);
                        /* Sealed or not, the application frame is the same 224 bytes. */
                        CHECK(app_len == (fr->unseal_reply
                                          ? (uint16_t)(fr->reply_len - OD_SEC2_D2H_OVERHEAD)
                                          : fr->reply_len));
                    }
                }
            }
            CHECK(ref_sink_calls() == fr->sink_calls);
            /* The record type is carried to the tag unchanged. Without this every valid type could
             * be normalised to TEXT and all five rows would still pass. */
            if (fr->sink_check_type && fr->sink_calls != 0u) {
                CHECK(ref_sink_rec_type() == fr->sink_rec_type);
            }
            /* 0 means "not asserted", not "expect zero": the sink is never reached with an empty
             * record, and a row that only counts calls must not accidentally assert a length the
             * previous frame left behind. */
            if (fr->sink_len != 0u && fr->sink_len != REF_ANY_LEN) {
                CHECK(ref_sink_len() == fr->sink_len);
            }
            /* ORDER, not just length: every forwarded byte must carry the offset it was sent at,
             * so chunks assembled backwards or at the wrong offset fail here and nowhere else. */
            if (fr->sink_check_data && fr->sink_calls != 0u
                && ref_sink_len() == fr->sink_len) {
                const uint8_t *got = ref_sink_data();
                unsigned bad = 0u;

                for (uint16_t i = 0; i < fr->sink_len; ++i) {
                    if (got[i] != REF_FILL(fr->sink_seed, i)) {
                        ++bad;
                    }
                }
                CHECK(bad == 0u);
            }
        }
    }
}
