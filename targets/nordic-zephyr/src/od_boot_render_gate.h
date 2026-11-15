/* od_boot_render_gate.h -- "the boot render is on the panel" (pure C11, host-tested).
 *
 * The boot screen renders on its own work queue, and advertising opens after at most 30 s even if
 * the render (worst case ~125 s with a stuck BUSY) is still running. od_display, od_epd_cfg and the
 * SPIM backend have no lock, so while this gate is held the loop thread must not start, abort or
 * tear down anything on the panel: uploads and SPI_RAW open answer PANEL_BUSY, the drive path finds
 * no display, aborts leave the panel alone, and the display service waits. A gate and not a mutex:
 * a mutex the render holds would block the loop thread -- BLE, the watchdog feed -- for the whole
 * render. */
#ifndef OD_BOOT_RENDER_GATE_H
#define OD_BOOT_RENDER_GATE_H

#include <stdbool.h>

void od_boot_render_gate_enter(void);   /* work queue, before the render touches the panel */
void od_boot_render_gate_leave(void);   /* work queue, after the panel is released */
bool od_boot_render_gate_busy(void);    /* any thread */

#endif /* OD_BOOT_RENDER_GATE_H */
