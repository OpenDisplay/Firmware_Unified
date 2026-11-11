/* sec2_client_dispatch.c -- sec2c_deliver() through the real dispatcher (see sec2_client.h). */

#include "sec2_client.h"

#include "od_dispatch.h"

od_frame_outcome_t sec2c_deliver(const od_reply_t *rp, od_span_t frame)
{
    return od_dispatch_frame(rp, frame);
}
