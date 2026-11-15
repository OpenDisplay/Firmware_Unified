/* BG22 od_claim_app_storage_purge() (shared/core/od_claim_app.h, SECURITY_V2 10.4a).
 *
 * NVM3 is a ring of pages: a write or delete only appends, and a superseded object stays
 * readable until a repack copies the live objects off its page and erases it. So the purge
 * appends a throw-away object until the ring's bottom page (fifoFirstIdx) has moved once per
 * page: every page has then been erased, and each live object (config, OS2S, the Bluetooth
 * stack's bonds) exists only as its current copy. Live data is never dropped -- this is NVM3's
 * own power-safe repack, so a power cut leaves a valid store and the next boot retries.
 *
 * The appends alone turn the ring: every nvm3_writeData() first repacks until the hard threshold
 * holds (repackUntilGood, bounded by the SDK). Do not add nvm3_repack() calls. On this layout
 * (3 x 8 KB pages, NVM3_DEFAULT_MAX_OBJECT_SIZE 3136, headroom 0) the soft threshold is
 * 22,632 B against 24,516 B of data area, so nvm3_repackNeeded() stays true whenever more than
 * 1,884 B are live -- i.e. with any real config -- and nvm3_repack() returns SL_STATUS_OK
 * regardless. A loop on either one never ends and erases a page per pass.
 *
 * Counting the turn. fifoFirstIdx alone cannot tell "no page erased" from "a whole number of
 * turns": once live data exceeds 24,516 - thrHard(3136) = 5,028 B, repackUntilGood() never
 * reaches its threshold and runs its full 2 x page-count steps, so a single append can erase
 * every page and leave the index where it was. So an append that leaves the index unchanged is
 * also checked against nvm3_getEraseCount(), the erase count of the bottom page: if that page
 * was erased, the ring turned at least once and the purge is complete. An append that moves the
 * index counts the forward distance, a lower bound.
 *
 * Giving up. Every append succeeding without a confirmed turn within `max` means a ring this
 * code does not understand. Retrying each boot would repeat the same bounded churn forever, so
 * that case reports done (the caller clears OD_SEC2_OS2S_PURGE) and leaves an RTT line. A failed
 * append still reports not done, so a store that could not be written is retried next boot.
 *
 * RAM: one 64-byte buffer. Flash wear: about one erase of each page; at most `max` appends. */

#include "od_claim_app.h"

#include <string.h>

#include "nvm3_default.h"
#include "od_diag.h"

#define OD_NVM3_PURGE_KEY 0x0F441Fu

bool od_claim_app_storage_purge(void)
{
  nvm3_Handle_t *h = nvm3_defaultHandle;
  uint8_t fill[64];
  size_t last, moved = 0u;
  uint32_t i, max, erases, erases_now;
  bool wrote_all = true;

  if (!h->hasBeenOpened && nvm3_initDefault() != SL_STATUS_OK) {
    return false;
  }
  if (h->totalNvmPageCnt == 0u || nvm3_getEraseCount(h, &erases) != SL_STATUS_OK) {
    return false;
  }
  last = h->fifoFirstIdx;
  /* One turn needs at most the region's worth of appends; a quarter on top for page tails. */
  max = (uint32_t)((h->nvmSize + h->nvmSize / 4u) / sizeof fill) + 32u;
  for (i = 0u; i < max && moved < h->totalNvmPageCnt; ++i) {
    memset(fill, (int)(i & 0xFFu), sizeof fill);
    if (nvm3_writeData(h, OD_NVM3_PURGE_KEY, fill, sizeof fill) != SL_STATUS_OK) {
      wrote_all = false;
      break;
    }
    if (h->fifoFirstIdx != last) {
      moved += (h->fifoFirstIdx + h->totalNvmPageCnt - last) % h->totalNvmPageCnt;
      last = h->fifoFirstIdx;
      if (nvm3_getEraseCount(h, &erases) != SL_STATUS_OK) {
        wrote_all = false;
        break;
      }
    } else if (nvm3_getEraseCount(h, &erases_now) != SL_STATUS_OK) {
      wrote_all = false;
      break;
    } else if (erases_now != erases) {
      moved = h->totalNvmPageCnt;   /* the bottom page itself was erased: a whole turn */
    }
  }
  (void)nvm3_deleteObject(h, OD_NVM3_PURGE_KEY);
  if (moved >= h->totalNvmPageCnt) {
    return true;
  }
  if (wrote_all) {
    OD_DIAG_BANNER("[OD] purge: no full turn within the bound; giving up\r\n");
    return true;
  }
  return false;
}
