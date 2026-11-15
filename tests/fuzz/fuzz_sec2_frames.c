/* fuzz_sec2_frames.c -- a sequence of BLE frames into od_dispatch_frame() on a SEC2 device.
 *
 * The input is [ctl][len][frame]...: ctl picks the device (bit 0 claimed, bit 1 a live session,
 * bit 2 claim mode on, bit 3 fresh), and every frame is [len][bytes]. A len byte with bit 7 set
 * marks a frame to SEAL with the live session's real key before delivery (its first two bytes are
 * the opcode), so the post-decrypt path -- the gate's sealed branch, the record parser's length
 * arithmetic, SEC_KEY -- is reachable at all; random bytes never forge a CCM tag. A len byte of
 * 0x7F advances the clock by the next byte * 256 ms, for the fragment gap and handshake timeouts.
 *
 * This reaches the SEC2 fragment reassembler, both handshakes, SEC_STATUS, the record opener, the
 * replay window, the gate table and the claim-mode allow-list. The invariant checked beyond
 * ASan/UBSan: on a claimed device, no gated command runs from a frame the harness did not seal.
 *
 * The harness is tests/host/sec2_test.c's (its main() compiled out), so a finding reproduces
 * there as a numbered case.
 */
#define SEC2_CTR_SUITE
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../host/sec2_test.c"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static bool plaintext_set(uint16_t cmd)
{
    return cmd == CMD_FIRMWARE_VERSION || cmd == CMD_READ_MSD || cmd == 0x0046u ||
           cmd == 0x0050u || cmd == CMD_SEC_STATUS || cmd == CMD_SEC_CLAIM || cmd == CMD_SEC_AUTH;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t f[256], g[256];
    size_t i = 1u;
    uint8_t ctl;
    bool claimed;

    if (size < 1u) {
        return 0;
    }
    ctl = data[0];
    claimed = (ctl & 1u) != 0u;
    setup(claimed);
    g_cm.active = (ctl & 4u) != 0u;
    g_cm.fresh = (ctl & 8u) != 0u;
    g_cm.renders_left = 3u;
    if (claimed && (ctl & 2u) != 0u && sec2c_auth(&C, &BLE) != 0) {
        abort();                          /* the harness's own handshake must work */
    }

    while (i < size) {
        const uint8_t len = data[i++];
        const uint8_t n = (uint8_t)(len & 0x7Fu);
        bool sealed = false;
        uint16_t fn;

        if (n == 0x7Fu) {
            if (i < size) { g_now_ms += (uint32_t)data[i++] * 256u; }
            continue;
        }
        if (n > size - i) {
            break;
        }
        memcpy(f, &data[i], n);
        i += n;
        fn = n;
        if ((len & 0x80u) != 0u && C.live && n >= 2u) {
            const uint16_t cmd = (uint16_t)(((uint16_t)f[0] << 8) | f[1]);
            fn = sec2c_seal(&C, cmd, &f[2], (uint16_t)(n - 2u), g);
            memcpy(f, g, fn);
            sealed = true;
        }
        (void)send(&BLE, f, fn);
        if (claimed && !sealed && g_hook_calls != 0u && od_sec2_state() == OD_SEC2_CLAIMED &&
            !plaintext_set(g_hook_cmd)) {
            abort();                      /* a gated command ran from an unsealed frame */
        }
        if (!od_sec2_session_live(NULL)) {
            C.live = false;
        }
    }
    return 0;
}
