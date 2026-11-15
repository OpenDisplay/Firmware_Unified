/* The seven experimental image-store and action hooks, as link-time stubs.
 *
 * od_dispatch_ops.h expands these rows whenever OD_CAP_IMGSTORE is set, which the silabs profile
 * does, so every executable linking od_shared_silabs owes definitions even when the test has
 * nothing to do with flash. The real hooks live in targets/efr32bg22-slc/od_cmd_flash.c and reach
 * an SPI NOR through a bit-banged HAL built on em_gpio -- faking that to answer an opcode no
 * fixture here exercises would buy nothing.
 *
 * OD_CMD_UNKNOWN is the honest answer: it is exactly what a build without the subsystem returns,
 * so a corpus or fault test that unexpectedly routes one of these opcodes sees the unknown-opcode
 * NACK rather than a silently plausible reply.
 */
#include "od_caps.h"
#include "od_cmd_app.h"

#if OD_CAP_IMGSTORE

#define STUB(fn)                                                       \
    od_cmd_result_t fn(const od_cmd_ctx_t *ctx, od_span_t body)        \
    { (void)ctx; (void)body; return OD_CMD_UNKNOWN; }

STUB(od_cmd_app_flash_info)
STUB(od_cmd_app_flash_list)
STUB(od_cmd_app_flash_erase)
STUB(od_cmd_app_flash_preload)
STUB(od_cmd_app_flash_display)
STUB(od_cmd_app_action_info)
STUB(od_cmd_app_action_fire)

#endif
