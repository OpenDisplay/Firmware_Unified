/* od_core.c -- see od_core.h.
 *
 * Five calls, written in dependency order: the staged-byte producers -- NFC assembly, transfer,
 * config-read -- then the queue their reservations are against, then the session those frames
 * would have been sealed with.
 *
 * THE ORDER IS NOT LOAD-BEARING, and saying so is worth more than a story about why it is. This
 * runs on the consumer context with nothing interleaved, and od_txq_reset() invalidates
 * outstanding tokens by generation anyway, so a producer cancelled second could not have committed
 * in between. What IS load-bearing is that all five happen -- which is the whole reason this
 * exists rather than a reset list copied into each target's teardown.
 */

#include "od_core.h"

#include "od_config_read.h"
#include "od_nfc.h"
#include "od_sec2.h"
#include "od_txq.h"
#include "od_xfer.h"

void od_core_reset(void)
{
    od_nfc_reset();
    od_xfer_reset();
    od_config_read_cancel();
    od_txq_reset();
    od_sec2_disconnect();
}
