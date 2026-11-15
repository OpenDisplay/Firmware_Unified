/* od_smp_lock.c -- see od_smp_lock.h. */
#include "od_smp_lock.h"

void od_smp_lock_unlock(struct od_smp_lock *l, uint32_t opens, uint32_t now_ms)
{
    l->unlocked = true;
    l->opens_at_unlock = opens;
    l->unlock_ms = now_ms;
}

bool od_smp_lock_step(struct od_smp_lock *l, uint32_t opens, uint32_t closes, uint32_t now_ms)
{
    bool session_opened;
    bool session_closed;

    if (!l->unlocked) {
        return false;
    }
    session_opened = (int32_t)(opens - l->opens_at_unlock) > 0;
    session_closed = (int32_t)(closes - l->opens_at_unlock) > 0;
    if (session_closed ||
        (!session_opened && (uint32_t)(now_ms - l->unlock_ms) >= OD_SMP_UNLOCK_CONNECT_MS)) {
        l->unlocked = false;
        return true;
    }
    return false;
}
