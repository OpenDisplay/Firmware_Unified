/* od_display_esp -- the ESP32 side of shared od_display: panel rail and keep-alive, the
 * od_drive_app / od_display_app / od_ident_app seams, CMD_SPI_RAW and the config-driven boot
 * screen. display_service.cpp keeps the od_xfer_app glue, which also routes the panels driven by
 * a built-in controller driver (IT8951, FastEPD parallel). */
#ifndef OD_DISPLAY_ESP_H
#define OD_DISPLAY_ESP_H

#include <stdbool.h>

struct DisplayConfig;

/* Panel rail for the built-in drivers, which have no od_display session: acquire brings the rail
 * up (or takes a warm one back); release keeps it warm for the keep-alive window on success and
 * forces everything off otherwise. */
void displayRailAcquire(void);
void displayRailRelease(bool success);

/* True when the panel is driven by a built-in controller driver rather than drive packets. */
bool displayConfigUsesNativeDriver(const struct DisplayConfig *d);
bool displayUsesNativeDriver(void);

/* Clear any upload in progress (both paths) and resume touch; forceOff also drops the rail. */
void displayXferClear(bool forceOff);

/* The boot screen from drive packets. Skipped, and not latched, while the config has none. */
bool displayBootConfigDriven(void);
/* After a config write: show the boot screen from the loop if it has not been shown yet. */
void displayRequestBootAfterConfigWrite(void);
void displayServiceDeferredBoot(void);

#endif /* OD_DISPLAY_ESP_H */
