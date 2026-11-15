/* od_boot_render_gate.c -- see od_boot_render_gate.h. */
#include "od_boot_render_gate.h"

#include <stdatomic.h>

static atomic_bool s_busy;

void od_boot_render_gate_enter(void)
{
    atomic_store_explicit(&s_busy, true, memory_order_release);
}

void od_boot_render_gate_leave(void)
{
    atomic_store_explicit(&s_busy, false, memory_order_release);
}

bool od_boot_render_gate_busy(void)
{
    return atomic_load_explicit(&s_busy, memory_order_acquire);
}
