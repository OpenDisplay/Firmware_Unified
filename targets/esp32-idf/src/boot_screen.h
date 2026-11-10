#ifndef BOOT_SCREEN_H
#define BOOT_SCREEN_H

struct od_boot_bufs;

/* The row and QR scratch the boot renderer draws through. */
const struct od_boot_bufs *bootScreenBufs(void);
/* Render into the built-in driver's framebuffer (IT8951 / FastEPD parallel panels). */
bool writeBootScreenWithQr();

#endif
