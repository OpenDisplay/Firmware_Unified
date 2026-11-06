/* The capped profile, compiled separately from the ordinary config_asm executable: struct
 * od_config_asm changes ABI with OD_CONFIG_MAX_SIZE, so this translation unit and the shared
 * implementation must agree on it.
 *
 * ASSERTS THE RELATIONSHIP, NOT A NUMBER. The bound under test is "a declared total above the
 * cap is refused at the start frame", which is what stops a two-byte length field from
 * overflowing a buffer smaller than 4000 bytes (CLAUDE.md decision 12). Pinning the literal cap
 * here made the test restate the profile instead of checking it, so every cap change failed as
 * an edit rather than as a result.
 *
 * The binding limit is whichever of the cap and the transferable ceiling is lower, so the
 * accept/reject pair is taken from that minimum. Above the ceiling the cap stops being
 * reachable through a chunked start at all.
 */

#include "od_config_asm.h"

#include <stdio.h>
#include <string.h>

static struct od_config_asm s;
static uint8_t frame[CONFIG_CHUNK_SIZE_WITH_PREFIX];

static enum od_config_asm_result start(uint16_t total)
{
    frame[0] = (uint8_t)(total & 0xffu);
    frame[1] = (uint8_t)(total >> 8);
    memset(frame + 2, 0xa5, CONFIG_CHUNK_SIZE);
    return od_config_asm_start(&s, od_span_make(frame, sizeof frame));
}

int main(void)
{
    const uint32_t cap = (uint32_t)OD_CONFIG_MAX_SIZE;
    const uint32_t limit = cap < OD_CONFIG_ASM_MAX_TRANSFERABLE
                               ? cap : OD_CONFIG_ASM_MAX_TRANSFERABLE;

    /* The ABI check that justifies a separate executable. */
    if (sizeof s.buffer != cap) {
        fprintf(stderr, "buffer %zu != cap %u: mixed profiles\n", sizeof s.buffer, (unsigned)cap);
        return 1;
    }
    /* A cap at or below one chunk would make `limit + 1` a non-chunked start and test nothing. */
    if (limit <= (uint32_t)CONFIG_CHUNK_SIZE || limit >= 0xffffu) {
        fprintf(stderr, "limit %u is not a chunked start\n", (unsigned)limit);
        return 1;
    }

    od_config_asm_reset(&s);
    if (start((uint16_t)(limit + 1u)) != OD_CONFIG_ASM_REJECTED || s.active) {
        fprintf(stderr, "limit+1 (%u) was accepted\n", (unsigned)(limit + 1u));
        return 1;
    }
    od_config_asm_reset(&s);
    if (start((uint16_t)limit) != OD_CONFIG_ASM_ACCEPTED || !s.active) {
        fprintf(stderr, "limit (%u) was rejected\n", (unsigned)limit);
        return 1;
    }
    return 0;
}
