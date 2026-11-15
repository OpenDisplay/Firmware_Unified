/* The link layer's ACL RX/TX FIFOs. liblt_825x.a only declares blt_rxfifo / blt_txfifo (common
 * symbols); the application owns the storage. Left undefined, the linker allocates them as
 * 1-byte, zero-sized FIFOs and the chip advertises but can hold no connection.
 * tools/check_ll_fifos.py fails the build if that happens again.
 *
 * Sizes are the SDK ble_sample's for a 27-octet link-layer payload (no data length extension):
 * RX entry maxRxOct + 22 rounded to 16, TX entry maxTxOct + 10 rounded to 4; both counts powers
 * of two. The 247-byte ATT MTU rides on L2CAP reassembly, not on these entries. */
#include "od_tlsr_sdk.h"

#define OD_LL_RX_FIFO_SIZE 64
#define OD_LL_RX_FIFO_NUM  8
#define OD_LL_TX_FIFO_SIZE 40
#define OD_LL_TX_FIFO_NUM  16

u8 blt_rxfifo_b[OD_LL_RX_FIFO_SIZE * OD_LL_RX_FIFO_NUM];
my_fifo_t blt_rxfifo = { OD_LL_RX_FIFO_SIZE, OD_LL_RX_FIFO_NUM, 0, 0, blt_rxfifo_b };

u8 blt_txfifo_b[OD_LL_TX_FIFO_SIZE * OD_LL_TX_FIFO_NUM];
my_fifo_t blt_txfifo = { OD_LL_TX_FIFO_SIZE, OD_LL_TX_FIFO_NUM, 0, 0, blt_txfifo_b };
