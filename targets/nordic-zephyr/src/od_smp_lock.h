/* od_smp_lock.h -- when an ENTER_DFU unlock of SMP ends (pure C, host-tested).
 *
 * On a claimed unit SMP is hidden until an authenticated CMD_ENTER_DFU. That handler drops the
 * link that sent it, so the unlock is for the NEXT connection: the session link. SMP re-locks
 *   - when the session link closes, or
 *   - after OD_SMP_UNLOCK_CONNECT_MS with no session link opened.
 * A running upload is never cut: nothing re-locks while the session link is open. A completed
 * upload ends in an SMP reset, which clears the unlock anyway.
 *
 * Links are counted, not tracked by handle: the BT callbacks bump `opens` and `closes`, and with
 * one connection at a time (CONFIG_BT_MAX_CONN=1) the k-th close ends the k-th open. The session
 * link is open number opens_at_unlock + 1, which holds whether or not the ENTER_DFU link has
 * closed yet when the unlock is recorded. Counter differences are taken as int32_t, so wrap is
 * harmless. */
#ifndef OD_SMP_LOCK_H
#define OD_SMP_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifndef OD_SMP_UNLOCK_CONNECT_MS
#define OD_SMP_UNLOCK_CONNECT_MS 120000u
#endif

struct od_smp_lock {
    bool     unlocked;
    uint32_t opens_at_unlock;  /* `opens` when the unlock was granted */
    uint32_t unlock_ms;
};

/* Records an unlock granted when `opens` links had been opened so far. */
void od_smp_lock_unlock(struct od_smp_lock *l, uint32_t opens, uint32_t now_ms);

/* True exactly once, on the step that ends the unlock; the caller then hides SMP. */
bool od_smp_lock_step(struct od_smp_lock *l, uint32_t opens, uint32_t closes, uint32_t now_ms);

#endif /* OD_SMP_LOCK_H */
