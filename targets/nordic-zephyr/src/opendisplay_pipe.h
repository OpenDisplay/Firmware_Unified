#ifndef OPENDISPLAY_PIPE_H
#define OPENDISPLAY_PIPE_H

#include <stdbool.h>
#include <stdint.h>

void opendisplay_pipe_on_write(const uint8_t *data, uint16_t len, bool write_cmd);
void opendisplay_pipe_on_connection_closed(void);
void opendisplay_pipe_on_notify_changed(bool enabled);
void opendisplay_pipe_process(void);

/* The live connection generation -- this target's frame identity, incremented on every close. A
 * queued frame carries the generation that produced it, so od_txq and od_rxq can both discard work
 * belonging to a connection that has gone.
 *
 * Public rather than behind a private header because it is a fact about the TRANSPORT, which is
 * what this file is for. */
uint32_t od_pipe_conn_gen(void);

#endif
