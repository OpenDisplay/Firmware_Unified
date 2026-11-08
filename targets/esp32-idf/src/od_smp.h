/* od_smp: mcumgr SMP over BLE, so ESP32 updates with the same host tooling as nRF. */
#ifndef OD_SMP_H
#define OD_SMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NimBLE host task. Reassembles fragments and hands a complete frame to the SMP task. */
void od_smp_rx(uint16_t conn_handle, const uint8_t *data, size_t len);
/* A link dropped: discard partial frames and any unfinished upload. */
void od_smp_link_lost(void);
/* With encryption on, SMP is refused until an authenticated CMD_ENTER_DFU unlocks it. */
void od_smp_unlock(void);
/* Keep the running image. An image that dies before this call rolls back at the next boot. */
void od_smp_confirm_running(void);

/* Implemented by src/communication.cpp, which owns the config. */
bool od_smp_encryption_on(void);
/* An SMP frame counts as link activity, or the idle timeout drops a long upload. */
void od_smp_note_activity(void);

/* Implemented by ble/od_ble_nimble.cpp. */
int od_ble_smp_notify(uint16_t conn_handle, const uint8_t *data, size_t len);
void od_ble_smp_fast_link(uint16_t conn_handle);

#ifdef __cplusplus
}
#endif

#endif /* OD_SMP_H */
