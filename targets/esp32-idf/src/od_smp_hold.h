/* od_smp_hold: whether an image upload outlives a dropped link.
 *
 * A link drop keeps the upload (OTA handle and offset) so a host on a new link can continue
 * with ImageUploadWrite(off = last confirmed offset); handle_upload answers a wrong offset with
 * the current one. The upload is aborted once OD_SMP_HOLD_US pass with no link sending SMP
 * frames. Plain C, so tools/test_smp_hold.c checks it on the host.
 */
#ifndef OD_SMP_HOLD_H
#define OD_SMP_HOLD_H

#include <stdbool.h>
#include <stdint.h>

#ifndef OD_SMP_HOLD_US
#define OD_SMP_HOLD_US (120ull * 1000000ull)
#endif

typedef struct {
    bool detached;      /* the link that sent SMP frames dropped, no frame since */
    uint64_t lost_us;   /* when it dropped */
} od_smp_hold_t;

/* NimBLE host task, at a disconnect. Restarts the hold on every drop. */
static inline void od_smp_hold_lost(od_smp_hold_t *h, uint64_t now_us)
{
    h->detached = true;
    h->lost_us = now_us;
}

/* NimBLE host task, at every SMP fragment: some link is sending again. */
static inline void od_smp_hold_attach(od_smp_hold_t *h)
{
    h->detached = false;
}

/* True while an unfinished upload waits for a link: the device must stay awake for it. */
static inline bool od_smp_hold_waiting(const od_smp_hold_t *h, bool upload_active, uint64_t now_us)
{
    return upload_active && h->detached && now_us - h->lost_us < OD_SMP_HOLD_US;
}

/* True when an unfinished upload has waited out the hold and must be aborted. */
static inline bool od_smp_hold_expired(const od_smp_hold_t *h, bool upload_active, uint64_t now_us)
{
    return upload_active && h->detached && now_us - h->lost_us >= OD_SMP_HOLD_US;
}

#endif /* OD_SMP_HOLD_H */
