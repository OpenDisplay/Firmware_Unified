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
 * RAM: one 64-byte buffer. Flash wear: about one erase of each page; at most `max` appends. */

#include "od_claim_app.h"

#include <string.h>

#include "nvm3_default.h"

#define OD_NVM3_PURGE_KEY 0x0F441Fu

bool od_claim_app_storage_purge(void)
{
  nvm3_Handle_t *h = nvm3_defaultHandle;
  uint8_t fill[64];
  size_t last, moved = 0u;
  uint32_t i, max;

  if (!h->hasBeenOpened && nvm3_initDefault() != SL_STATUS_OK) {
    return false;
  }
  if (h->totalNvmPageCnt == 0u) {
    return false;
  }
  last = h->fifoFirstIdx;
  /* Enough appends for twice the region: the ring turns once with room to spare. */
  max = (uint32_t)((2u * h->nvmSize) / sizeof fill) + 32u;
  for (i = 0u; i < max && moved < h->totalNvmPageCnt; ++i) {
    memset(fill, (int)(i & 0xFFu), sizeof fill);
    if (nvm3_writeData(h, OD_NVM3_PURGE_KEY, fill, sizeof fill) != SL_STATUS_OK) {
      break;
    }
    if (h->fifoFirstIdx != last) {
      moved += (h->fifoFirstIdx + h->totalNvmPageCnt - last) % h->totalNvmPageCnt;
      last = h->fifoFirstIdx;
    }
  }
  (void)nvm3_deleteObject(h, OD_NVM3_PURGE_KEY);
  return moved >= h->totalNvmPageCnt;
}
