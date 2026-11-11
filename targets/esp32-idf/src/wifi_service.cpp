#include "od_claim_mode.h"
#include "wifi_service.h"
#include "od_ble.h"        /* OD: BLE identity address for the mDNS TXT record */

#ifdef OPENDISPLAY_HAS_WIFI

#include "communication.h"
#include "od_cmd.h"
#include "encryption.h"   // getChipIdHex(), getAuthDeviceIdBytes(), OD_CHIP_ID_HEX_LEN
#include "od_sec2.h"
#include "structs.h"
#include "od_log.h"
#include "ble_transport.h"
#include "link_owner.h"
#include "od_hal_time.h"
#include "od_hal_sleep.h"
#include <errno.h>
#include <mdns.h>
#include <esp_err.h>
#include <lwip/sockets.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>       // raw handler for WIFI_EVENT_STA_BSS_RSSI_LOW (no Arduino event id)
#include <esp_heap_caps.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/platform.h"   // mbedtls_platform_set_calloc_free (MBEDTLS_PLATFORM_MEMORY is on via esp_config.h)

#ifndef COMM_MODE_WIFI
#define COMM_MODE_WIFI (1 << 2)
#endif

extern struct od_config globalConfig;
extern char wifiSsid[33];
extern char wifiPassword[33];
extern uint8_t wifiEncryptionType;
extern bool wifiConfigured;
extern bool wifiConnected;
extern bool wifiInitialized;
extern uint16_t wifiServerPort;
extern bool wifiServerConnected;
// tcpReceiveBuffer / tcpReceiveBufferPos are declared in wifi_service.h (included
// above) so the pointer type is checked against its definition in main.h.
extern uint8_t msd_payload[16];

// Station status. Was compat/WiFi.h's wl_status_t and its WL_* macros, which were
// Arduino's names for values this file both produces (odWifiStatus) and consumes --
// nothing outside it ever saw them. The numeric values are Arduino's, kept only so a
// log line comparing against old firmware still reads the same.
typedef int od_wifi_status_t;
#define OD_WIFI_IDLE          0
#define OD_WIFI_NO_SSID       1
#define OD_WIFI_CONNECTED     3
#define OD_WIFI_CONNECT_FAIL  4
#define OD_WIFI_DISCONNECTED  6

// Defined further down with the rest of the WiFi station layer; used before that point.
static int  odWifiStatus(void);
static void odWifiIpStr(char* out, size_t n);

// ------------------------------------------------------------- LAN sockets ---
// Phase C step 9b-iii. This replaces compat/WiFi.h's WiFiServer/WiFiClient, which
// were already lwIP sockets underneath -- what the Arduino classes added was an
// object identity this file never needed. There is exactly ONE listener and AT MOST
// ONE peer by design (R3: a second client is refused, never admitted), so the whole
// of that identity is two file descriptors.
//
// -1 means "not open" everywhere below. The two are independent lifetimes: the
// listener outlives any number of peer sessions and is closed only by
// opendisplay_lan_teardown().
static int s_lanListenFd = -1;
static int s_lanClientFd = -1;

static void lanSockClose(int* fd) {
    if (*fd >= 0) {
        lwip_close(*fd);
        *fd = -1;
    }
}

// Is the PEER still there? Deliberately not `s_lanClientFd >= 0`: on the plain-TCP
// path lanClientAvailable() also returns 0 for a socket the peer has closed, so
// holding an fd is not evidence of a live session and wifiServerConnected would stay
// true until reboot. (The TLS path is unaffected -- it maps recv == 0 to
// OD_LAN_READ_CLOSED itself.) MSG_PEEK|MSG_DONTWAIT answers the question without
// consuming a byte or blocking the loop task.
//
// LIVENESS ONLY -- NEVER GUARD A TEARDOWN WITH THIS. The two questions "do we own a
// descriptor" and "is the peer still live" are separate predicates, and teardown wants
// the first one (`s_lanClientFd >= 0`). Guarding a close with this leaked the socket on
// the single most common path there is: wifiLanReapClosedSession() fires precisely when
// !lanClientConnected(), so an orderly FIN / TLS close_notify reached disconnectWiFi-
// Server(), re-asked the same question, got "not connected", and skipped the close --
// after which the next accept overwrote the only reference to the fd. A few hundred
// normal host reconnects exhausted the lwIP descriptor table and LAN admission failed
// until reboot. lanSockClose() is idempotent and clears the fd, so teardown sites call
// it unconditionally.
static bool lanClientConnected(void) {
    if (s_lanClientFd < 0) {
        return false;
    }
    uint8_t b;
    int r = lwip_recv(s_lanClientFd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r > 0) {
        return true;    /* data waiting */
    }
    if (r == 0) {
        return false;   /* orderly shutdown by the peer */
    }
    // Report DISCONNECTED only on an errno that definitely means it. Everything else
    // -- including EWOULDBLOCK, the common "idle but healthy" case -- stays connected.
    // Erring the other way tears down live sessions on a spurious error, which is
    // worse than the leak this check exists to fix.
    return !(errno == ECONNRESET || errno == ENOTCONN ||
             errno == EPIPE      || errno == EBADF);
}

static int lanClientAvailable(void) {
    if (s_lanClientFd < 0) return 0;
    int n = 0;
    return (lwip_ioctl(s_lanClientFd, FIONREAD, &n) == 0) ? n : 0;
}

static int lanClientRead(uint8_t* buf, size_t n) {
    if (s_lanClientFd < 0) return -1;
    return (int)lwip_recv(s_lanClientFd, buf, n, 0);
}

// Returns bytes accepted by the stack; 0 on failure. Callers compare against the
// length they asked for, so a short write reads as incomplete rather than as success.
static int lanClientWrite(const uint8_t* buf, size_t n) {
    if (s_lanClientFd < 0) return 0;
    int sent = (int)lwip_send(s_lanClientFd, buf, n, 0);
    return (sent > 0) ? sent : 0;
}

static void lanSockSetNoDelay(int fd) {
    int v = 1;
    lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
}

// Bounds a single recv(), not a whole frame read -- the framing loop keeps its own
// budget. Kept because the accepted socket is otherwise blocking-by-default and a
// half-open peer could park the loop task indefinitely.
static void lanSockSetRcvTimeout(int fd, uint32_t ms) {
    struct timeval tv;
    tv.tv_sec  = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

// The send-side counterpart, and it exists for the same reason: the accepted socket is
// blocking-by-default, so without it a peer that stops reading fills the window and parks the loop
// task inside send() with no bound at all. od_hal_radio.h requires the opposite ("MUST NOT BLOCK")
// and every drain deadline above it is sized on that.
//
// Short deliberately. A send blocks only once the peer's receive window is full, and waiting longer
// does not empty it -- it just spends the caller's deadline. 200 ms sits inside the 250 ms
// pre-refresh barrier, so one stalled peer costs a single retry rather than the whole barrier.
#define LAN_SEND_TIMEOUT_MS 200u

static void lanSockSetSndTimeout(int fd, uint32_t ms) {
    struct timeval tv;
    tv.tv_sec  = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

// Dotted-quad of the peer, into a caller buffer. Replaces
// wifiClient.remoteIP().toString().c_str() -- an IPAddress plus an Arduino String
// heap allocation, per connect, to format four bytes.
static void lanSockPeerIpStr(int fd, char* out, size_t n) {
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    if (fd < 0 || lwip_getpeername(fd, (struct sockaddr*)&a, &len) != 0) {
        snprintf(out, n, "?");
        return;
    }
    uint32_t v = a.sin_addr.s_addr;   /* network order == big-endian bytes in memory */
    snprintf(out, n, "%u.%u.%u.%u",
             (unsigned)(v & 0xFF), (unsigned)((v >> 8) & 0xFF),
             (unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 24) & 0xFF));
}

// Open the listening socket, idempotent. Returns false if the port could not be
// bound; the caller logs, and LAN simply does not come up.
static bool lanListenBegin(uint16_t port) {
    if (s_lanListenFd >= 0) return true;

    s_lanListenFd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_lanListenFd < 0) return false;

    int one = 1;
    lwip_setsockopt(s_lanListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a = {};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons(port);

    // Backlog 4, not 1: the accept poll runs from the main loop, which can be parked
    // in a panel refresh for tens of seconds, and a backlog of 1 refuses every
    // connection attempt arriving in that window.
    if (lwip_bind(s_lanListenFd, (struct sockaddr*)&a, sizeof a) != 0 ||
        lwip_listen(s_lanListenFd, 4) != 0) {
        lanSockClose(&s_lanListenFd);
        return false;
    }
    // Non-blocking accept: this is polled from the main loop and must never park it
    // in accept() while a BLE transfer is in flight.
    int flags = lwip_fcntl(s_lanListenFd, F_GETFL, 0);
    lwip_fcntl(s_lanListenFd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

// A new peer's fd, or -1 when none is waiting (the ordinary case every tick).
static int lanAcceptOne(void) {
    if (s_lanListenFd < 0) return -1;
    return lwip_accept(s_lanListenFd, nullptr, nullptr);
}



// Roam gating: never re-associate (or full-channel scan, which steals the shared radio
// from BLE under coex) while a transfer is in flight. transferActive() covers all three
// transfer types and is declared in display_service.h.
bool transferActive(void);

void getChipIdHex(char* out, size_t out_size);
#ifndef OD_CHIP_ID_HEX_LEN
#define OD_CHIP_ID_HEX_LEN 6
#endif
static void lanBeginConnect(void);   // defined with the roaming / RTC AP-cache block below
uint8_t getFirmwareMajor();
uint8_t getFirmwareMinor();

// od_dispatch_app_frame() comes from communication.h.

// ------------------------------------------------------------------ TLS-PSK ---
// One TLS session at a time, driven cooperatively from handleWiFiServer(). The
// PSK identity string "opendisplay" must match the py-opendisplay client; the
// PSK bytes are K_lan = KDF(K_dev, "OD2/k-lan", uid) (SECURITY_V2 2.2, 12).
static const char* kTlsPskIdentity = "opendisplay";
static const int kTlsCiphersuites[] = { MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, 0 };

static bool tlsMode = false;            // true when the ACTIVE channel is TLS-PSK
static bool tlsInited = false;          // mbedTLS config objects built once
static bool tlsSessionActive = false;   // an mbedtls_ssl_context is live for s_lanClientFd
static bool tlsHandshakeDone = false;
static uint8_t tlsPsk[16];

static mbedtls_ssl_context   tlsSsl;
static mbedtls_ssl_config    tlsConf;
static mbedtls_ctr_drbg_context tlsDrbg;
static mbedtls_entropy_context  tlsEntropy;

// lastLanActivityMs is GONE. LAN used to keep its own activity clock, stamped at
// connect, handshake completion, raw bytes read and frame dispatch, and checked
// inline in handleWiFiServer(). R4 requires the SAME definition of activity on
// every transport -- a recognised command from the owner, excluding refresh -- and
// two clocks implementing one rule is how they drift. The shared clock in
// link_owner.cpp is now the only one; serviceIdleTimeout() in main.cpp enforces it
// for both transports, with each keeping its own constant (BLE 120 s local,
// LAN OD_LAN_READ_TIMEOUT_S 30 s from the wire header).
// Epoch of the current LAN session's ownership claim, 0 when this session does not
// own the slot. Kept so the release matches the FULL identity: releasing on
// transport alone would let a stale LAN teardown free a slot a newer session (or a
// BLE client) had since taken.
static uint16_t s_lanEpoch = 0;

static uint16_t lanBasePort(void) {
    return (wifiServerPort != 0) ? wifiServerPort : (uint16_t)OD_LAN_TCP_PORT;
}
// Active LAN port: plaintext -> base; TLS -> base+1 (derived, no config field).
uint16_t lanActivePort(void) {
    uint16_t base = lanBasePort();
    return (od_sec2_state() == OD_SEC2_CLAIMED) ? (uint16_t)(base + 1) : base;
}

bool lanTlsEnabled(void) { return (od_sec2_state() == OD_SEC2_CLAIMED); }

// Is there a LIVE, FULLY HANDSHAKEN TLS-PSK session on the accepted socket?
//
// Distinct from lanTlsEnabled(), which only answers "is the TLS listener the one we
// bound" -- a configuration question with no session in it. Authorization decisions
// need this one: SECTION 9 rule 4 makes the TLS handshake the authentication on this
// transport, so "authenticated" means the peer proved possession of the derived PSK,
// which is true only once mbedtls_ssl_handshake() has returned 0. Deriving it from the
// global encryption-enabled bit instead would authorize a plaintext frame on a device
// that merely has encryption configured.
bool lanTlsSessionEstablished(void) {
    return tlsMode && tlsSessionActive && tlsHandshakeDone &&
           wifiServerConnected && s_lanClientFd >= 0;
}

// mbedTLS BIO shims over the accepted socket (non-blocking cooperative model).
// ctx is unused and deliberately so: these read s_lanClientFd through the same
// accessors as the rest of the file, so they always see the CURRENT descriptor.
// mbedTLS holds the bio ctx pointer for the life of the ssl context, and the
// descriptor changes when one session ends and another is admitted -- passing the fd
// by value would leave the BIO writing to a closed, or recycled, descriptor. The
// pointer is still handed to set_bio (&s_lanClientFd) so a future caller who does
// dereference ctx gets the live value rather than a stale copy.
static int tls_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    (void)ctx;
    if (!lanClientConnected()) return MBEDTLS_ERR_NET_CONN_RESET;
    int w = lanClientWrite(buf, len);
    if (w <= 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return w;
}
static int tls_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    (void)ctx;
    if (!lanClientConnected()) return MBEDTLS_ERR_NET_CONN_RESET;
    if (lanClientAvailable() <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
    int r = lanClientRead(buf, len);
    if (r <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return r;
}

// mbedTLS on this SDK is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y, so every TLS
// allocation comes from internal DRAM only -- PSRAM is never eligible, however much of
// it the board has. ssl_setup alone needs two ~16.4 KB contiguous blocks there
// (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384, asymmetric length not set), which is the
// dominant failure mode once WiFi + BLE coex and the static buffers have taken their cut.
// Append this to any TLS failure so the log says whether it was OOM and by how much.
// Returns `buf` so it can be used inline as a %s argument. mbedTLS error codes are negative
// and conventionally written as -0xNNNN, which is why the sign is handled explicitly rather
// than left to %d -- "-0x7280" is greppable against the mbedTLS headers and "-29312" is not.
static const char* tlsFailNote(int ret, char* buf, size_t n) {
    char code[16];
    if (ret < 0) {
        snprintf(code, sizeof(code), "-0x%X", (unsigned)(-ret));
    } else {
        snprintf(code, sizeof(code), "%d", ret);
    }
    snprintf(buf, n, " (ret=%s, internal free=%u, largest block=%u)", code,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return buf;
}

// -------------------------------------------- TLS record pre-reservation ---
// mbedtls_ssl_setup() allocates TWO record buffers of CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN
// (16384) + record overhead, i.e. ~16.7 KB EACH, and CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y
// forces both into internal DRAM. Both values are baked into the prebuilt SDK libs, so
// neither the size nor the region can be changed from here.
//
// That fails in the field even with plenty of free RAM, because what matters is the
// LARGEST CONTIGUOUS block, not the total. Observed on hardware:
//   ssl_setup failed (ret=-0x7f00, internal free=51144, largest block=31732)
// 51 KB free, yet the first 16.7 KB allocation leaves only ~15 KB in that block and the
// second has nowhere to go -- short by under 2 KB.
//
// Worse, ssl_setup runs on EVERY LAN client connect and ssl_free releases on every
// disconnect, so TLS cycles 33 KB through the heap per connection while String logging
// churns small blocks in between: the odds degrade the longer the device stays up.
//
// Fix: reserve the two slots ONCE at boot -- from setup(), right after full_config_init()
// and BEFORE BleTransport::begin()/initWiFi() take their ~100 KB -- when the heap is still
// contiguous, then serve mbedTLS from them via its allocator hook. This does not raise
// peak usage (the buffers are needed whenever TLS runs); it moves the allocation to the
// one moment where success is guaranteed, and recycling the slots stops TLS fragmenting
// the heap for lwIP/BLE.
//
// Only record-sized requests come from the pool. Small handshake/bignum allocations (and
// encryption.cpp's CCM/CMAC contexts) still go to the normal internal heap, so their
// behavior and placement are unchanged.
//
// Single-task by design: every mbedTLS call here runs on the loop task (handleWiFiServer
// drives handshake + reads), so the busy flags need no locking.
#define OD_TLS_RECORD_SLOT_SIZE  17408u   /* 17 KB: 16384 content + record overhead margin */
#define OD_TLS_RECORD_SLOTS      2        /* ssl_setup takes exactly one in + one out */
#define OD_TLS_POOL_MIN_ALLOC    8192u    /* only record-sized requests use the pool */

static uint8_t* s_tlsSlot[OD_TLS_RECORD_SLOTS];
static bool     s_tlsSlotBusy[OD_TLS_RECORD_SLOTS];
static bool     s_tlsAllocHooked = false;

static void* od_tls_calloc(size_t n, size_t size) {
    const size_t total = n * size;
    if (total >= OD_TLS_POOL_MIN_ALLOC) {
        for (int i = 0; i < OD_TLS_RECORD_SLOTS; i++) {
            if (s_tlsSlot[i] != nullptr && !s_tlsSlotBusy[i] && total <= OD_TLS_RECORD_SLOT_SIZE) {
                s_tlsSlotBusy[i] = true;
                memset(s_tlsSlot[i], 0, total);   /* calloc semantics */
                return s_tlsSlot[i];
            }
        }
        // The pool could not serve a record-sized request: either the slot is too small
        // or both are in use. This is exactly the case that reintroduces the original
        // failure, so name it once -- the size tells us whether the slot needs raising.
        static bool warned = false;
        if (!warned) {
            warned = true;
            od_log_warn("TLS alloc %u B not served from the reserved pool (slot %u B) "
                   "-- falling back to heap",
                   (unsigned)total, (unsigned)OD_TLS_RECORD_SLOT_SIZE);
        }
    }
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void od_tls_free(void* p) {
    if (p == nullptr) return;
    for (int i = 0; i < OD_TLS_RECORD_SLOTS; i++) {
        if (p == s_tlsSlot[i]) {
            s_tlsSlotBusy[i] = false;   /* recycle, never return it to the heap */
            return;
        }
    }
    heap_caps_free(p);
}

void od_tls_reserve_records(void) {
    if (s_tlsAllocHooked) return;
    // Gate on config: a device without encryption must not hold 34 KB it never uses.
    if (!(od_sec2_state() == OD_SEC2_CLAIMED)) {
        od_log_info("TLS: encryption disabled, no record buffers reserved");
        return;
    }
    int got = 0;
    for (int i = 0; i < OD_TLS_RECORD_SLOTS; i++) {
        s_tlsSlot[i] = (uint8_t*)heap_caps_malloc(OD_TLS_RECORD_SLOT_SIZE,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_tlsSlotBusy[i] = false;
        if (s_tlsSlot[i] != nullptr) got++;
    }
    // Install the hook even on a partial reservation: whatever was reserved still helps,
    // and od_tls_calloc falls back to the heap for the rest.
    mbedtls_platform_set_calloc_free(od_tls_calloc, od_tls_free);
    s_tlsAllocHooked = true;
    od_log_info("TLS: reserved %d/%d record slots of %u B, internal free=%u, largest block=%u",
           (int)got, (int)OD_TLS_RECORD_SLOTS, (unsigned)OD_TLS_RECORD_SLOT_SIZE,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (got < OD_TLS_RECORD_SLOTS) {
        od_log_warn("TLS record reservation incomplete -- ssl_setup may still fail");
    }
}

void odLanReserveRxBuffer(void) {
    if (tcpReceiveBuffer != nullptr) return;   // idempotent, per od_tls_reserve_records
    // calloc, not malloc: this was a .bss array and callers may read ahead of the write
    // cursor on a partial frame. PSRAM first -- it is 16 KB of internal DRAM otherwise,
    // on a part where mbedTLS alone needs 34 KB contiguous internal.
    tcpReceiveBuffer = (uint8_t*)heap_caps_calloc(1, OD_LAN_RX_BUFFER_SIZE,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool inPsram = (tcpReceiveBuffer != nullptr);
    if (!inPsram) {
        tcpReceiveBuffer = (uint8_t*)heap_caps_calloc(1, OD_LAN_RX_BUFFER_SIZE,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (tcpReceiveBuffer == nullptr) {
        od_log_error("LAN RX buffer reservation failed -- LAN transport will not start");
        return;
    }
    od_log_info("LAN: reserved RX buffer %u B in %s, internal free=%u, largest block=%u",
           (unsigned)OD_LAN_RX_BUFFER_SIZE, inPsram ? "PSRAM" : "DRAM",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!inPsram) {
        // The only signal that this board's PSRAM is absent or dead:
        // CONFIG_SPIRAM_IGNORE_NOTFOUND=1 lets it boot silently. No reclaim here.
        od_log_warn("LAN RX buffer fell back to internal DRAM -- no PSRAM on this board?");
    }
}

// Build the shared server config once (RNG + PSK + one ECDHE-PSK ciphersuite).
static bool tlsEnsureConfig(void) {
    // Late fallback: encryption can be turned on by a runtime config write, long after
    // the boot-time reservation window has passed. Try anyway -- it may fail on a churned
    // heap, but the log then says so instead of leaving it silently unreserved.
    od_tls_reserve_records();
    if (tlsInited) return true;
    if (!od_sec2_k_lan(tlsPsk)) {
        od_log_error("TLS PSK derivation failed (device not claimed)");
        return false;
    }
    mbedtls_ssl_config_init(&tlsConf);
    mbedtls_ctr_drbg_init(&tlsDrbg);
    mbedtls_entropy_init(&tlsEntropy);
    const char* pers = "opendisplay-tls";
    int rc = mbedtls_ctr_drbg_seed(&tlsDrbg, mbedtls_entropy_func, &tlsEntropy,
                                   reinterpret_cast<const unsigned char*>(pers), strlen(pers));
    if (rc != 0) {
        char note[128];
        od_log_error("TLS RNG seed failed%s", tlsFailNote(rc, note, sizeof(note)));
        return false;
    }
    rc = mbedtls_ssl_config_defaults(&tlsConf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        char note[128];
        od_log_error("TLS config defaults failed%s", tlsFailNote(rc, note, sizeof(note)));
        return false;
    }
    mbedtls_ssl_conf_rng(&tlsConf, mbedtls_ctr_drbg_random, &tlsDrbg);
    mbedtls_ssl_conf_ciphersuites(&tlsConf, kTlsCiphersuites);
    rc = mbedtls_ssl_conf_psk(&tlsConf, tlsPsk, sizeof(tlsPsk),
                              reinterpret_cast<const unsigned char*>(kTlsPskIdentity),
                              strlen(kTlsPskIdentity));
    if (rc != 0) {
        char note[128];
        od_log_error("TLS conf_psk failed%s", tlsFailNote(rc, note, sizeof(note)));
        return false;
    }
    tlsInited = true;
    return true;
}

static void tlsCloseSession(void) {
    if (tlsSessionActive) {
        mbedtls_ssl_close_notify(&tlsSsl);
        mbedtls_ssl_free(&tlsSsl);
    }
    tlsSessionActive = false;
    tlsHandshakeDone = false;
}

static bool tlsBeginSession(void) {
    if (!tlsEnsureConfig()) return false;
    mbedtls_ssl_init(&tlsSsl);
    int rc = mbedtls_ssl_setup(&tlsSsl, &tlsConf);
    if (rc != 0) {
        // -0x7F00 == MBEDTLS_ERR_SSL_ALLOC_FAILED: the record buffers did not fit in
        // internal DRAM. Compare "largest block" against ~16.4 KB in the note above.
        char note[128];
        od_log_error("TLS ssl_setup failed%s", tlsFailNote(rc, note, sizeof(note)));
        mbedtls_ssl_free(&tlsSsl);
        return false;
    }
    mbedtls_ssl_set_bio(&tlsSsl, &s_lanClientFd, tls_bio_send, tls_bio_recv, nullptr);
    tlsSessionActive = true;
    tlsHandshakeDone = false;
    return true;
}

// Staging buffer so a frame leaves as ONE write. Two writes cost an extra packet
// (and, before setNoDelay(), a 40-200 ms Nagle/delayed-ACK stall on every frame);
// on TLS they also cost a second record header + MAC, which on a 2-byte ACK is more
// overhead than payload. Sized past the largest response the device produces
// (encrypted_response[600] in communication.cpp); anything larger falls back to the
// two-write path rather than growing static RAM for a case that does not occur.
static uint8_t lanTxFrame[2 + 640];

// Write one [len:2 LE][payload] frame over the active LAN channel (TLS or plain).
// Called by communication.cpp for LAN-origin responses (send_tls_lan_frame / plain).
// Drop the socket and let the ordinary reap path do the orderly teardown: with the fd cleared,
// lanClientConnected() reports false and wifiLanReapClosedSession() calls disconnectWiFiServer()
// early next pass. Calling that from here instead would re-enter the TLS context this write is
// standing in.
static od_radio_result_t lanAbandonSession(const char* why) {
    od_log_error("LAN: %s -- dropping the session", why);
    lanSockClose(&s_lanClientFd);
    return OD_RADIO_GONE;
}

// One bounded write of one whole buffer over plain TCP.
//
// The return is a framing statement, not an errno translation. RETRY is reserved for the case
// where ZERO bytes reached the wire, because that is the only state in which od_txq re-presenting
// the same frame is safe. A short write has already put part of a length-prefixed frame on the
// stream, so no retry can repair it and the session goes.
static od_radio_result_t lanPlainWriteAll(const uint8_t* buf, size_t n) {
    if (s_lanClientFd < 0) {
        return OD_RADIO_GONE;
    }
    const int sent = (int)lwip_send(s_lanClientFd, buf, n, 0);
    if (sent == (int)n) {
        return OD_RADIO_SENT;
    }
    if (sent > 0) {
        return lanAbandonSession("partial frame write desynchronised the stream");
    }
    // SO_SNDTIMEO expiry lands here as EWOULDBLOCK/EAGAIN with nothing written -- the peer's
    // window is full. A zero return is not a documented success either; both are safe to re-offer.
    if (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        return OD_RADIO_RETRY;
    }
    if (sent == 0) {
        return OD_RADIO_RETRY;
    }
    return lanAbandonSession("socket write failed");
}

// The TLS counterpart, and it is NOT symmetric with the plain one on partial writes.
//
// WANT_WRITE may follow mbedTLS having already pushed part of a record to the socket, but mbedTLS
// owns that resumption: its contract is to re-call with IDENTICAL arguments, which is exactly what
// od_txq does with an unchanged queue head. So WANT_WRITE is retryable here even though the same
// situation is fatal on plain TCP. A short POSITIVE return is not -- that is application data
// mbedTLS considers delivered, and re-presenting the frame would duplicate it.
static od_radio_result_t lanTlsWriteAll(const uint8_t* buf, size_t n) {
    const int ret = mbedtls_ssl_write(&tlsSsl, buf, n);
    if (ret == (int)n) {
        return OD_RADIO_SENT;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
        return OD_RADIO_RETRY;
    }
    if (ret > 0) {
        return lanAbandonSession("partial TLS record desynchronised the stream");
    }
    return lanAbandonSession("TLS write failed");
}

static od_radio_result_t lanWriteAll(const uint8_t* buf, size_t n) {
    return tlsMode ? lanTlsWriteAll(buf, n) : lanPlainWriteAll(buf, n);
}

od_radio_result_t opendisplay_lan_send_frame(const uint8_t* payload, uint16_t len) {
    if (payload == nullptr || len == 0) {
        return OD_RADIO_ERROR;
    }
    // Not GONE: GONE tells od_txq that every entry for this tag is undeliverable, and a LAN link
    // that has not come up yet is a different thing from one that has died. RETRY holds the
    // response until the session is usable.
    if (!wifiServerConnected || !lanClientConnected()) {
        return OD_RADIO_RETRY;
    }
    if (tlsMode && (!tlsSessionActive || !tlsHandshakeDone)) {
        return OD_RADIO_RETRY;
    }

    if ((uint32_t)len + 2u <= sizeof(lanTxFrame)) {
        lanTxFrame[0] = (uint8_t)(len & 0xFF);
        lanTxFrame[1] = (uint8_t)((len >> 8) & 0xFF);
        memcpy(lanTxFrame + 2, payload, len);
        // One write for prefix and payload together, which is why this is the path that can report
        // RETRY at all: there is no half-written frame to strand.
        return lanWriteAll(lanTxFrame, (size_t)len + 2u);
    }

    // Oversized fallback: prefix, then payload. Only the FIRST write may report RETRY -- after it
    // succeeds the peer is committed to a payload, so anything short of SENT on the second is the
    // stranded-prefix case and takes the session with it.
    const uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
    const od_radio_result_t rh = lanWriteAll(hdr, 2);
    if (rh != OD_RADIO_SENT) {
        return rh;
    }
    if (lanWriteAll(payload, len) == OD_RADIO_SENT) {
        return OD_RADIO_SENT;
    }
    return lanAbandonSession("length prefix sent without its payload");
}

bool wifiLanClientConnected(void) {
    return wifiServerConnected && lanClientConnected();
}

static void hex14_lower(const uint8_t* src, char* out29) {
    static const char* h = "0123456789abcdef";
    for (int i = 0; i < 14; i++) {
        out29[i * 2] = h[(src[i] >> 4) & 0x0F];
        out29[i * 2 + 1] = h[src[i] & 0x0F];
    }
    out29[28] = '\0';
}

// --------------------------------------------------------------------- mDNS ---
// Phase C step 9b-iv. This replaces compat/ESPmDNS.h, whose MDNSResponder was
// ENTIRELY NO-OPS -- begin() returned true, addService() and addServiceTxt() did
// nothing at all. So this target has never advertised anything since the import,
// while logging nine TXT records as though it had. The shim said why: "if the mdns
// component is not present in the build, these become no-ops rather than a build
// failure". It was not present, because ESP-IDF dropped mdns from core in v5.0. It
// is now vendored at targets/esp32-idf/components/mdns (see third_party/NOTICE.md).
//
// THIS DEVICE ADVERTISES ONLY. It never resolves a name and never browses. The
// component ships a querier and a browser regardless -- there is no build flag for
// responder-only -- so that decision lives in sdkconfig.defaults, which sizes the
// responder for exactly one service on exactly one interface.
//
// NOTE THE LEADING UNDERSCORES on the service type below. Arduino's ESPmDNS took
// "opendisplay"/"tcp" and prefixed them itself; the IDF API does not, and takes
// "_opendisplay"/"_tcp" as written. Passing the Arduino spelling advertises
// `opendisplay.tcp.local` -- a name no client looks for, and a failure that looks
// identical to the mDNS not working at all.
#define OD_MDNS_SERVICE  "_opendisplay"
#define OD_MDNS_PROTO    "_tcp"

static bool s_mdnsUp = false;          // mdns_init() has succeeded
static bool s_mdnsServiceUp = false;   // the _opendisplay._tcp record is registered

// The advertised BLE address, lowercase colon-separated (SECTION 9 rule 6, key
// `mac`). Prior identity used getChipIdHex() (eFuse), which is NOT what HA
// stores as the device unique_id. The lowercasing and the hardware-validation
// caveat live in BleTransport::addressString(), which already returns a plain
// const char* -- the String wrapper this used to have bought nothing.
static const char* advertisedBleMacLower(void) {
    return ble.addressString();
}

// Push the rolling MSD nibble into the `msd` TXT key.
//
// RATE-LIMITED ON PURPOSE, and it matters more now than it did against the no-op
// shim. Every TXT change makes the responder re-announce the record on the
// multicast group, so an unthrottled caller here is unthrottled radio traffic on a
// battery device that also shares its antenna with BLE. The 400 ms floor and the
// unchanged-payload check below are what keep announcement traffic proportional to
// real state changes rather than to loop() frequency.
void opendisplay_mdns_update_msd_txt(void) {
    if (!wifiConnected || odWifiStatus() != OD_WIFI_CONNECTED) {
        return;
    }
    if (!s_mdnsServiceUp) {
        return;   // nothing registered to attach a TXT item to
    }
    static uint8_t last_msd[14];
    static uint32_t last_ms = 0;
    static bool have_last = false;
    uint8_t cur[14];
    memcpy(cur, &msd_payload[2], sizeof(cur));
    uint32_t now = od_hal_uptime_ms();
    if (have_last && memcmp(cur, last_msd, sizeof(cur)) == 0 && (now - last_ms) < 400) {
        return;
    }
    have_last = true;
    memcpy(last_msd, cur, sizeof(cur));
    last_ms = now;
    char hex[29];
    hex14_lower(cur, hex);
    esp_err_t err = mdns_service_txt_item_set(OD_MDNS_SERVICE, OD_MDNS_PROTO, "msd", hex);
    if (err != ESP_OK) {
        od_log_warn("mDNS: msd TXT update failed (%s)", esp_err_to_name(err));
    }
}

// Tear the responder down completely. Used on LAN teardown before restart; also the
// recovery path if service registration fails, so a half-registered responder does
// not linger advertising a port nothing listens on.
static void odMdnsStop(void) {
    if (s_mdnsUp) {
        mdns_free();
        s_mdnsUp = false;
    }
    s_mdnsServiceUp = false;
}

static void restartLanService(void) {
    char idHex[OD_CHIP_ID_HEX_LEN + 1] = {0};
    getChipIdHex(idHex, sizeof(idHex));
    char deviceName[3 + OD_CHIP_ID_HEX_LEN + 1];
    snprintf(deviceName, sizeof(deviceName), "OD%s", idHex);

    // init ONCE. This function runs on every LAN (re)start -- reconnect, config
    // change, encryption toggle -- and mdns_init() on an already-running responder
    // returns ESP_ERR_INVALID_STATE. Cycling free/init per restart would also drop
    // and recreate a FreeRTOS task each time, so the service record is replaced
    // instead (below) and the responder itself persists.
    if (!s_mdnsUp) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            od_log_error("mDNS responder failed to start (%s)", esp_err_to_name(err));
            return;
        }
        s_mdnsUp = true;
    }
    esp_err_t err = mdns_hostname_set(deviceName);
    if (err != ESP_OK) {
        od_log_error("mDNS hostname set failed (%s)", esp_err_to_name(err));
        odMdnsStop();
        return;
    }
    mdns_instance_name_set(deviceName);

    // Replace, do not append. The port moves when encryption is toggled (plaintext
    // base, TLS base+1), and every TXT value below can change with config, so a
    // restart must not leave the previous record advertised alongside the new one.
    // With MDNS_MULTIPLE_INSTANCE=n a duplicate add would fail rather than
    // duplicate -- but failing is not the behaviour wanted either.
    if (s_mdnsServiceUp) {
        mdns_service_remove_all();
        s_mdnsServiceUp = false;
    }

    uint16_t port = lanActivePort();

    // F1 -- identity/capability TXT keys (SECTION 9 rule 6, ADDITIVE to `msd`).
    // Registered as ONE array at add time rather than as six follow-up calls. The
    // Arduino spelling did the latter, which against a real responder is six
    // separate record mutations and therefore up to six re-announcements for a
    // record that was never once advertised in a complete state.
    char fw[12];
    snprintf(fw, sizeof(fw), "%u.%u", (unsigned)getFirmwareMajor(), (unsigned)getFirmwareMinor());
    char cm[3];
    snprintf(cm, sizeof(cm), "%02x", (unsigned)globalConfig.system_config.communication_modes);
    uint8_t did[4];
    getAuthDeviceIdBytes(did);
    char idhex[9];
    snprintf(idhex, sizeof(idhex), "%02x%02x%02x%02x", did[0], did[1], did[2], did[3]);
    const char* mac = advertisedBleMacLower();
    const char* tls = (od_sec2_state() == OD_SEC2_CLAIMED) ? "1" : "0";

    // Value pointers need only outlive the call -- the component copies both key and
    // value into its own record storage.
    mdns_txt_item_t txt[] = {
        { "mac", mac    },   // REQUIRED
        { "tls", tls    },   // REQUIRED
        { "fw",  fw     },   // RECOMMENDED
        { "cm",  cm     },   // RECOMMENDED
        { "id",  idhex  },   // OPTIONAL
        { "pv",  OD_PROTOCOL_VERSION_STR },  // OPTIONAL
    };
    err = mdns_service_add(NULL, OD_MDNS_SERVICE, OD_MDNS_PROTO, port,
                           txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        od_log_error("mDNS service add failed (%s) -- LAN is up but undiscoverable",
               esp_err_to_name(err));
        odMdnsStop();
        return;
    }
    s_mdnsServiceUp = true;

    od_log_info("mDNS: %s.local", deviceName);
    od_log_info("mDNS: %s.%s port %u tls=%s mac=%s",
           OD_MDNS_SERVICE, OD_MDNS_PROTO, (unsigned)port, tls, mac);
    opendisplay_mdns_update_msd_txt();
}

static void startLanServer(void) {
    // No RX buffer, no listener. Refusing here is the whole degrade path: it covers
    // every caller, and it is better than accepting a socket the parser cannot serve.
    // BLE and the display path are unaffected.
    if (tcpReceiveBuffer == nullptr) {
        od_log_error("LAN RX buffer unavailable -- LAN transport disabled");
        return;
    }
    tlsMode = (od_sec2_state() == OD_SEC2_CLAIMED);
    uint16_t port = lanActivePort();
    // A restart on a new port must rebind, not silently keep the old listener: the
    // Arduino WiFiServer::begin() returned early when a socket already existed, so
    // restartWiFiLanAfterReconnect() after a config change left the device listening
    // on the PREVIOUS port while logging the new one. Closing first makes the log true.
    lanSockClose(&s_lanListenFd);
    if (!lanListenBegin(port)) {
        od_log_error("LAN listener could not bind port %u -- LAN transport disabled",
               (unsigned)port);
        return;
    }
    od_log_info("%s LAN server listening on port %u",
           tlsMode ? "TLS-PSK" : "Plaintext", (unsigned)port);
    restartLanService();
}

// odWifiStatus() collapses every association failure into OD_WIFI_DISCONNECTED (6), which
// is useless for field diagnosis. Log the 802.11/ESP reason code instead: 201
// (NO_AP_FOUND) means the SSID was never seen -- typically a 5 GHz-only or hidden
// network; 15 (4WAY_HANDSHAKE_TIMEOUT) / 202 (AUTH_FAIL) mean a bad password; 200
// (BEACON_TIMEOUT) / 4 (ASSOC_EXPIRE) point at range or BLE coexistence.
static bool wifiDiagEventsRegistered = false;

// ------------------------------------------------------------------ roaming ---
// Move to a stronger AP on the same SSID when the current link degrades. Two halves
// that only work together:
//
//  (a) BEST-AP SELECTION. Arduino's default is scan_method = WIFI_FAST_SCAN, which
//      stops at the FIRST SSID match and associates with it however weak; the
//      sort_method it also defaults to (WIFI_CONNECT_AP_BY_SIGNAL) is INERT unless
//      the scan is WIFI_ALL_CHANNEL_SCAN. So on a multi-AP SSID (mesh/extenders) the
//      device can sit on a distant AP while a strong one exists on another channel,
//      non-deterministically across reboots. Switching to ALL_CHANNEL_SCAN makes the
//      RSSI sort actually apply, so every (re)connect picks the strongest AP.
//      Cost: a full-channel scan per connect (~1.5-2.5 s vs ~0.3-0.5 s) -- radio-on
//      time paid on each deep-sleep wake.
//
//  (b) THE TRIGGER. esp_wifi_set_rssi_threshold() posts WIFI_EVENT_STA_BSS_RSSI_LOW
//      once the averaged RSSI drops below the threshold. It is ONE-SHOT (see the API
//      note in esp_wifi.h) so it must be re-armed after every event and after every
//      re-association. Arduino does not surface this event in arduino_event_id_t, so
//      it needs a raw esp_event handler on WIFI_EVENT.
//
// Without (a), (b) is a footgun: a reconnect under FAST_SCAN can land on the SAME
// weak AP, giving a pointless disconnect/reconnect loop. Do not enable one alone.
//
// The roam itself is DEFERRED to an idle moment (serviceLanRoam) because
// re-association drops the TCP session and a full-channel scan steals the shared
// radio from BLE under coex -- doing that mid-transfer would abort it.
#ifndef OD_LAN_ROAM_RSSI_THRESHOLD
#define OD_LAN_ROAM_RSSI_THRESHOLD (-75)   /* dBm; valid range -100..10 */
#endif

static bool roamPending = false;            // set by the RSSI-low event, cleared on roam
static bool roamEventRegistered = false;

// Event handlers run on tiny stacks (see EVENT-CONTEXT RULE below), so they only set
// these and stash plain scalars; serviceWifiEventFollowUp() does the logging and the
// esp_* calls on the loop task.
static volatile bool assocNoticePending = false;
static volatile bool disconnectNoticePending = false;
static volatile bool gotIpPending = false;
static volatile bool rssiLowNoticePending = false;
static volatile bool cacheFailPending = false;
static volatile int  s_lastAssocChannel = 0;
static volatile int  s_lastDisconnectReason = 0;
static volatile int  s_lastRssiLowDbm = 0;
static volatile uint32_t s_lastGotIp = 0;

// ------------------------------------------------------- RTC-cached AP choice ---
// Deep sleep is a full reset, so without this every wake pays the all-channel scan
// (~1.5-2.5 s of radio-on time) just to rediscover the AP it used seconds earlier.
// Caching the winning BSSID + channel in RTC memory (which survives deep sleep) lets a
// wake connect DIRECTLY to that BSSID on a single channel: no scan at all, faster than
// even the old FAST_SCAN default, while still landing on the strongest AP because the
// cache is only ever written from a scan-selected association.
//
// Kept file-local rather than in main.h (where the other RTC_DATA_ATTR state lives)
// because nothing outside this translation unit uses it.
//
// Self-healing: any failure or degradation invalidates the cache and falls back to a
// full scan -- see lanCacheInvalidate() callers. In particular a cached connect that
// lands on a now-weak AP trips the RSSI threshold, which invalidates and rescans, so a
// stale "best" choice cannot persist across days.
RTC_DATA_ATTR static uint8_t s_cachedBssid[6];
RTC_DATA_ATTR static uint8_t s_cachedChannel;   // 1..14; 0 = unset
RTC_DATA_ATTR static bool    s_cachedValid;

static bool usingCachedAp = false;      // this attempt used the cache (drives fallback)
static bool rescanReconnectPending = false;  // cached BSSID failed; re-begin with a scan

// Scan/sort selection is no longer a pair of sticky pre-begin setters -- it lives in the
// wifi_config_t that odWifiBegin() writes for the non-pinned path, because that is where IDF
// actually reads it from. The two Arduino setters this replaced were no-ops, so this is the
// first time the "strongest AP wins" behaviour described above is real.

// ---------------------------------------------------------------------------------------
// WiFi station control, against esp_wifi/esp_netif directly (phase C step 9b-ii).
//
// This replaces compat/WiFi.h's WiFiClass, which was not merely a thin wrapper: FIVE of its
// methods were SILENT NO-OPS, and the code around them was written assuming they worked. Each
// is implemented here, and each fixes a behaviour this target has not had since the import:
//
//   setScanMethod / setSortMethod  no-ops. So "scan all channels, strongest AP wins" did not
//                                  happen -- on a multi-AP SSID the STA took whichever AP
//                                  answered first, which on a mesh is usually the one you
//                                  just walked away from.
//   setTxPower                     no-op. The radio ran at the driver default rather than the
//                                  15 dBm this code asks for -- a current-draw difference on
//                                  a battery tag, not just a range one.
//   BSSID()                        returned a STATIC ZERO ARRAY. lanCacheStoreCurrentAp()
//                                  therefore cached 00:00:00:00:00:00, so the whole
//                                  deep-sleep-wake fast path was inert while logging success.
//   begin(ssid, pass, ch, bssid)   dropped the channel and BSSID. Same consequence: the
//                                  "no scan" path always scanned.
//   onEvent()                      never dispatched. onWiFiDiagEvent() has NEVER RUN on this
//                                  target: no association notice, no disconnect reason, no
//                                  got-IP line, no cached-AP failure handling -- and with
//                                  setAutoReconnect also a no-op, nothing reconnected a
//                                  dropped link at all.
//
// The handlers below are the real IDF ones, feeding the same flag variables the Arduino-shaped
// handler used, so serviceWifiEventFollowUp() on the loop task is unchanged. The
// EVENT-CONTEXT RULE applies to them exactly as before: flags only.
// ---------------------------------------------------------------------------------------

static esp_netif_t* s_staNetif = nullptr;
static bool s_wifiStackUp = false;

static void odWifiEnsureStack(void) {
    if (s_wifiStackUp) return;
    esp_netif_init();
    esp_event_loop_create_default();
    s_staNetif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_wifiStackUp = true;
}

// Associate. A non-NULL bssid pins the AP and channel (the deep-sleep fast path); NULL scans
// every channel and takes the strongest, which is what lanApplyBestApSelection() used to ask
// for through two no-op setters. Both live in one wifi_config_t, so they are set together
// here rather than as separate "sticky" calls.
static int odWifiBegin(const char* ssid, const char* pass,
                       const uint8_t* bssid, uint8_t channel) {
    if (!ssid) return OD_WIFI_CONNECT_FAIL;
    odWifiEnsureStack();

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) {
        strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }
    if (bssid != nullptr && channel >= 1 && channel <= 14) {
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.channel = channel;
    } else {
        wc.sta.bssid_set = false;
        wc.sta.channel = 0;
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    }
    // Credentials are never logged, at any level -- ARCHITECTURE.md "Secrets are never logged
    // verbatim". Presence and length only.
    od_log_info("WiFi: join SSID (set, %u chars), password %s%s",
                (unsigned)strlen(ssid), (pass && *pass) ? "(set)" : "(empty)",
                wc.sta.bssid_set ? ", BSSID-pinned" : ", all-channel scan");

    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();
    return OD_WIFI_IDLE;
}

static int odWifiStatus(void) {
    if (!s_wifiStackUp) return OD_WIFI_IDLE;
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? OD_WIFI_CONNECTED : OD_WIFI_DISCONNECTED;
}

static int32_t odWifiRssi(void) {
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

static int32_t odWifiChannel(void) {
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.primary : 0;
}

// Real BSSID of the associated AP, or NULL when not associated. The shim returned a static
// zero array unconditionally, which is what made the AP cache inert.
static const uint8_t* odWifiBssid(void) {
    static wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return nullptr;
    return ap.bssid;
}

static void odWifiBssidStr(char* out, size_t n) {
    const uint8_t* b = odWifiBssid();
    if (b == nullptr) {
        if (n) out[0] = '\0';
        return;
    }
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static void odWifiIpStr(char* out, size_t n) {
    esp_netif_ip_info_t ip = {};
    if (s_staNetif && esp_netif_get_ip_info(s_staNetif, &ip) == ESP_OK) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(out, n, "0.0.0.0");
    }
}

static void odWifiDisconnect(bool radioOff) {
    if (!s_wifiStackUp) return;
    esp_wifi_disconnect();
    if (radioOff) esp_wifi_stop();
}

// 0.25 dBm units, so 60 == 15 dBm -- the value Arduino's WIFI_POWER_15dBm encodes. Only valid
// once the STA is started, which is why the call site is after begin().
static void odWifiSetTxPower15dBm(void) {
    esp_wifi_set_max_tx_power(60);
}

static void lanCacheInvalidate(const char* why) {
    if (s_cachedValid) {
        od_log_info("WiFi: AP cache cleared (%s) -- next connect will scan", why);
    }
    s_cachedValid = false;
    s_cachedChannel = 0;
}

// Record the AP we actually associated with, so the next wake can go straight to it.
static void lanCacheStoreCurrentAp(void) {
    const uint8_t* b = odWifiBssid();
    int32_t ch = odWifiChannel();
    if (b == nullptr || ch < 1 || ch > 14) return;
    memcpy(s_cachedBssid, b, 6);
    s_cachedChannel = (uint8_t)ch;
    s_cachedValid = true;
}

// Single entry point for association: cached BSSID when available, full scan otherwise.
static void lanBeginConnect(void) {
    if (s_cachedValid && s_cachedChannel >= 1 && s_cachedChannel <= 14) {
        usingCachedAp = true;
        char b[18];
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_cachedBssid[0], s_cachedBssid[1], s_cachedBssid[2],
                 s_cachedBssid[3], s_cachedBssid[4], s_cachedBssid[5]);
        od_log_info("WiFi: connecting to cached AP %s ch %d (no scan)", b, (int)s_cachedChannel);
        odWifiBegin(wifiSsid, wifiPassword, s_cachedBssid, s_cachedChannel);
        return;
    }
    usingCachedAp = false;
    od_log_info("WiFi: scanning all channels for the strongest AP");
    odWifiBegin(wifiSsid, wifiPassword, nullptr, 0);
}

static void onWifiRssiLow(void* arg, esp_event_base_t base, int32_t id, void* data);

// Register the raw WIFI_EVENT handler LAZILY, retrying until it takes.
//
// esp_event_handler_register() requires the default event loop, which Arduino creates
// while starting the STA -- not before. Doing this pre-WiFi.begin() (where the Arduino
// callback is registered) fails with ESP_ERR_INVALID_STATE and leaves roaming silently
// dead, which is exactly what happened on hardware. Called from the association path,
// by which point the loop and the WiFi task definitely exist; idempotent and self-
// retrying so a transient failure does not permanently disable roaming.
static void ensureRoamEventRegistered(void) {
    if (roamEventRegistered) return;
    esp_err_t rc = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW,
                                              &onWifiRssiLow, NULL);
    if (rc == ESP_OK) {
        roamEventRegistered = true;
        od_log_info("WiFi: roaming armed (RSSI-low handler registered)");
        return;
    }
    // Always report the code -- "could not register" with no reason is undiagnosable.
    od_log_info("WiFi: RSSI-low handler register failed (%s); will retry on next association",
           esp_err_to_name(rc));
}

// One-shot: re-arm after every event and after every association.
static void lanArmRssiThreshold(void) {
    ensureRoamEventRegistered();   // no point arming a threshold nothing listens for
    esp_err_t rc = esp_wifi_set_rssi_threshold(OD_LAN_ROAM_RSSI_THRESHOLD);
    if (rc != ESP_OK) {
        od_log_info("WiFi: RSSI threshold arm failed (%d)", (int)rc);
    }
}

// EVENT-CONTEXT RULE (learned the hard way -- this handler previously panicked the
// device): callbacks here run on tiny stacks -- the raw esp_event handler on the system
// event task, and the Arduino callbacks on "arduino_events" with a 4096-byte stack
// (ARDUINO_NETWORK_EVENT_TASK_STACK_SIZE). Formatting a log line plus esp_wifi_*/esp_event_*
// calls is far too much for that budget -- it was Arduino String concatenation that first
// blew it, but od_log_* formats into a 256-byte frame of its own and is no safer here. So
// handlers ONLY set flags; every log and esp_* call happens in serviceWifiEventFollowUp() on
// the loop task. Keep it that way.
static void onWifiRssiLow(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg; (void)base; (void)id;
    const wifi_event_bss_rssi_low_t* e = (const wifi_event_bss_rssi_low_t*)data;
    s_lastRssiLowDbm = e ? (int)e->rssi : 0;   // plain int store; logged from loop()
    roamPending = true;
    rssiLowNoticePending = true;
}

// The Arduino-shaped diagnostic handler that used to live here is DELETED, not ported. It took
// (arduino_event_id_t, arduino_event_info_t) -- shim types -- and compat/WiFi.h's onEvent() was
// a no-op, so it was never dispatched on this target. Its body now lives in
// odWifiEventHandler() above, against the real IDF event types.


// The real IDF handlers. Same flag-setting bodies as the Arduino-shaped onWiFiDiagEvent()
// they replace -- which the shim never dispatched, so none of this has run on this target
// before. EVENT-CONTEXT RULE: flags only, no logging, no esp_* calls.
static void odWifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t* e = (const wifi_event_sta_connected_t*)data;
        s_lastAssocChannel = e ? (int)e->channel : 0;
        assocNoticePending = true;
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t* e = (const wifi_event_sta_disconnected_t*)data;
        s_lastDisconnectReason = e ? (int)e->reason : 0;
        disconnectNoticePending = true;
        // A BSSID-pinned begin() leaves bssid_set = 1 in the driver config, so retrying would
        // hammer that ONE AP forever -- fatal if it moved, changed channel, or powered off.
        // Ask the loop to drop the cache and re-begin WITHOUT a BSSID, which both clears
        // bssid_set and rescans. That reasoning was already here; it is only now reachable,
        // because the pinning it guards against is only now real.
        if (usingCachedAp) {
            usingCachedAp = false;
            cacheFailPending = true;
            rescanReconnectPending = true;
        } else {
            // Plain drop: reconnect. Arduino's setAutoReconnect(true) was meant to do this and
            // was a no-op, so until now a dropped link stayed down until something else
            // re-initialised WiFi. esp_wifi_connect() from the event task is the documented
            // way to do it and does not block.
            esp_wifi_connect();
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
        s_lastGotIp = e ? (uint32_t)e->ip_info.ip.addr : 0;
        gotIpPending = true;
        return;
    }
}

static void registerWiFiDiagEvents(void) {
    if (wifiDiagEventsRegistered) return;
    // Needs the default event loop, which odWifiEnsureStack() creates. Called from initWiFi()
    // before the first begin(), so bring the stack up here rather than relying on ordering.
    odWifiEnsureStack();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &odWifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &odWifiEventHandler, nullptr);
    wifiDiagEventsRegistered = true;
    // The raw WIFI_EVENT handler is NOT registered here: this runs before WiFi.begin(),
    // and esp_event_handler_register() needs the default event loop, which Arduino only
    // creates while bringing the STA up. Registering here fails with ESP_ERR_INVALID_STATE
    // and silently disabled roaming. It is now done lazily -- see ensureRoamEventRegistered().
}

// Execute a queued roam, but only when nothing is in flight. Called from
// handleWiFiServer() each tick.
// Everything the event handlers deferred, run on the loop task where the stack is ample.
// Called from handleWiFiServer() each tick.
static void serviceWifiEventFollowUp(void) {
    if (assocNoticePending) {
        assocNoticePending = false;
        od_log_info("WiFi event: associated (channel %d)", (int)s_lastAssocChannel);
    }
    if (disconnectNoticePending) {
        disconnectNoticePending = false;
        od_log_info("WiFi event: disconnected, reason %d", (int)s_lastDisconnectReason);
    }
    if (cacheFailPending) {
        cacheFailPending = false;
        lanCacheInvalidate("cached BSSID did not associate");
    }
    if (rssiLowNoticePending) {
        rssiLowNoticePending = false;
        od_log_info("WiFi: RSSI %d dBm below %d dBm -- roam queued (deferred to idle)",
               (int)s_lastRssiLowDbm, (int)OD_LAN_ROAM_RSSI_THRESHOLD);
        lanArmRssiThreshold();   // one-shot: re-arm so a deferred roam still re-triggers
    }
    if (gotIpPending) {
        gotIpPending = false;
        {
            const uint32_t ipRaw = (uint32_t)s_lastGotIp;
            char bssidStr[18] = "";
            odWifiBssidStr(bssidStr, sizeof(bssidStr));
            od_log_info("WiFi event: got IP %u.%u.%u.%u, RSSI %d dBm, ch %d, BSSID %s",
                   (unsigned)(ipRaw & 0xFF), (unsigned)((ipRaw >> 8) & 0xFF),
                   (unsigned)((ipRaw >> 16) & 0xFF), (unsigned)((ipRaw >> 24) & 0xFF),
                   (int)odWifiRssi(), (int)odWifiChannel(), bssidStr);
        }
        lanCacheStoreCurrentAp();   // remember this AP for the next deep-sleep wake
        lanArmRssiThreshold();      // also (re)registers the RSSI-low handler
    }
}

void serviceLanRoam(void) {
    serviceWifiEventFollowUp();
    // A cached BSSID that failed to associate (AP moved, changed channel, or powered
    // off). Re-begin WITHOUT a BSSID from loop context -- never from the event callback --
    // so the driver's bssid_set is cleared and a full scan picks a live AP.
    if (rescanReconnectPending && !wifiConnected) {
        rescanReconnectPending = false;
        od_log_info("WiFi: cached AP unreachable -- falling back to a full scan");
        odWifiDisconnect(false);
        lanBeginConnect();   // cache was invalidated on the disconnect -> scan path
        return;
    }
    if (!roamPending || !wifiConnected) return;
    // A LAN client implies a possible in-flight transfer; BLE transfers would also be
    // disrupted by a full-channel scan under coex. Wait for genuine idle. transferActive()
    // covers DIRECT/PIPE/PARTIAL -- the previous direct+pipe test let a BLE-origin partial
    // write (no LAN client attached, so the check above does not fire either) be
    // interrupted by the scan.
    if (lanClientConnected() || wifiServerConnected) return;
    if (transferActive()) return;

    roamPending = false;
    od_log_info("WiFi: roaming -- re-associating to the strongest AP for this SSID");
    // MUST drop the cache first: the whole point of a roam is to leave this AP, and
    // lanBeginConnect() would otherwise pin us straight back to the one we are escaping.
    lanCacheInvalidate("roaming to a stronger AP");
    usingCachedAp = false;
    rescanReconnectPending = false;
    // Clearing wifiConnected lets handleWiFiServer()'s existing re-association path
    // restart the LAN server once the new link is up.
    wifiConnected = false;
    odWifiDisconnect(false);
    lanBeginConnect();   // cache now invalid -> full scan, strongest AP wins
}

void initWiFi(bool waitForConnection) {
    /* SECURITY_V2 7.3 / 10.1: no WiFi in claim mode, none on a LOCKED device. */
    if (od_claim_mode_active() || od_sec2_state() == OD_SEC2_LOCKED) {
        od_log_info("WiFi not started: %s", od_claim_mode_active() ? "claim mode" : "LOCKED");
        return;
    }
    od_log_info("=== Initializing WiFi ===");

    // WiFi is NOT gated on power_mode: if COMM_MODE_WIFI is enabled the radio comes
    // up on battery too. Radio cost on battery is managed by the driver's default
    // power-save mode and by deep sleep, not by refusing to associate.
    if (!(globalConfig.system_config.communication_modes & COMM_MODE_WIFI)) {
        od_log_info("WiFi not enabled in communication_modes, skipping");
        wifiInitialized = false;
        return;
    }
    if (!wifiConfigured) {
        od_log_info("WiFi: system_config has WiFi mode on, but wifi_config TLV (0x26) is not in saved "
                    "configuration (or failed to parse). Enable Wi-Fi in config, set SSID, and write full "
                    "config to the device.");
        wifiInitialized = false;
        return;
    }
    if (wifiSsid[0] == '\0' || strlen(wifiSsid) == 0) {
        od_log_info("WiFi: wifi_config packet present but SSID field is empty.");
        wifiInitialized = false;
        return;
    }
    // Do not log the SSID or password (credentials); log only presence/length.
    od_log_info("WiFi: connecting to configured SSID (len %u)", (unsigned)strlen(wifiSsid));
    registerWiFiDiagEvents();
    // No setAutoReconnect(): IDF has no such switch, and the Arduino one was a no-op here
    // anyway. Reconnection is driven explicitly by the STA_DISCONNECTED handler below.
    wifiSsid[32] = '\0';
    wifiPassword[32] = '\0';
    od_log_info("Encryption type: 0x%X", (unsigned)wifiEncryptionType);
    wifiConnected = false;
    wifiInitialized = true;
    // Cached BSSID when RTC memory still holds one (the deep-sleep-wake fast path:
    // single channel, no scan), otherwise an all-channel scan for the strongest AP.
    lanBeginConnect();
    // Tx power can only be set once the STA is started, i.e. after begin(); the
    // pre-begin() call this replaces failed with ESP_ERR_WIFI_NOT_START.
    odWifiSetTxPower15dBm();
    if (!waitForConnection) {
        od_log_info("WiFi: STA started (non-blocking; LAN starts when associated)");
        return;
    }
    od_log_info("Waiting for WiFi connection...");
    const int maxRetries = 3;
    const unsigned long timeoutPerRetry = 10000;
    bool connected = false;
    for (int retry = 0; retry < maxRetries && !connected; retry++) {
        uint32_t startAttempt = od_hal_uptime_ms();
        bool abortCurrentRetry = false;
        while (odWifiStatus() != OD_WIFI_CONNECTED && (od_hal_uptime_ms() - startAttempt < timeoutPerRetry)) {
            od_hal_delay_ms(500);
            od_wifi_status_t status = odWifiStatus();
            od_log_info("WiFi status: %d", (int)status);
            if (status == OD_WIFI_CONNECT_FAIL || status == OD_WIFI_NO_SSID) {
                od_log_info("Connection failed immediately (Status: %d)", (int)status);
                abortCurrentRetry = true;
                break;
            }
        }
        if (odWifiStatus() == OD_WIFI_CONNECTED) {
            connected = true;
            break;
        }
        if (!abortCurrentRetry) {
            od_log_info("WiFi attempt %d timed out", (int)(retry + 1));
        }
        if (retry < maxRetries - 1) {
            od_hal_delay_ms(2000);
        }
    }
    if (odWifiStatus() == OD_WIFI_CONNECTED) {
        wifiConnected = true;
        od_log_info("=== WiFi connected ===");
        char ipStr[16];
        odWifiIpStr(ipStr, sizeof(ipStr));
        od_log_info("IP: %s", ipStr);
        startLanServer();
    } else {
        wifiConnected = false;
        od_log_info("=== WiFi connection failed ===");
    }
}

void wifiLanDropOwnedSocket(void) {
    // The LAN arm of abortToKnownState()'s step 10. It exists as its own seam
    // because tlsCloseSession() is file-static, so session_guard.cpp cannot reach
    // the pieces directly.
    //
    // Deliberately the LAN-LOCAL subset of disconnectWiFiServer() below: it closes
    // the socket and clears this file's session bookkeeping, but does NOT call
    // requestTransferSessionCleanup(), which are the
    // abort's own steps 8 and 3-5. Calling them here would nest the two teardowns.
    tlsCloseSession();
    // Ownership, not liveness -- see lanClientConnected(). A peer that has already
    // gone away still leaves us holding its descriptor, and that is exactly the case
    // this has to close.
    if (s_lanClientFd >= 0) {
        od_log_info("Closing LAN client (session abort)");
        lanSockClose(&s_lanClientFd);
    }
    wifiServerConnected = false;
    tcpReceiveBufferPos = 0;
    // Deliberately NO linkRelease() here: this is the abort's drop step, and the
    // abort releases the token itself as its final step (strictly after the drop).
    // Releasing here would free the slot before the drop had completed.
    s_lanEpoch = 0;
}

void disconnectWiFiServer() {
    tlsCloseSession();
    // Ownership, not liveness -- see lanClientConnected(). The dominant caller is
    // wifiLanReapClosedSession(), which runs BECAUSE the peer is gone, so a liveness
    // guard here skipped both the close and the crypto clear on every orderly close.
    if (s_lanClientFd >= 0) {
        od_log_info("Closing LAN client");
        lanSockClose(&s_lanClientFd);
    }
    wifiServerConnected = false;
    tcpReceiveBufferPos = 0;
    // The token is deliberately NOT released here, for the same reason the BLE
    // disconnect callbacks do not release: the socket is closed but this session's
    // transfer state is still live, and its teardown is DEFERRED to the loop below
    // (requestTransferSessionCleanup). Releasing now would let a BLE connect claim
    // the slot before that teardown runs -- and serviceBleDisconnectCleanup would
    // then see the new BLE owner live, skip the abort entirely, and leave the new
    // owner's frames executing against the departed LAN session's transfer state.
    //
    // Release stays the abort's final step (R3a), reached via the cleanup flag
    // raised below. s_lanEpoch is left intact so the identity remains valid until
    // then; a new accept cannot use it, because accept refuses while the slot is
    // held.
    // F4: abort any in-flight direct-write / pipe / partial transfer + tear down a
    // mid-transfer panel session, DEFERRED to loop() so cleanup never races an
    // in-progress EPD refresh. Shared with the BLE disconnect path -- main.cpp
    // works out which transport actually owned the transfer.
    requestTransferSessionCleanup();
}

// lanReadIntoBuffer() return codes. A graceful peer close is deliberately distinct from
// a fault: both end the session, but only one is worth an ERROR in the log.
#define OD_LAN_READ_ERROR   (-1)
#define OD_LAN_READ_CLOSED  (-2)
static int s_lastLanReadErr = 0;   // mbedTLS ret for the last OD_LAN_READ_ERROR

// Drain readable bytes from the active channel into tcpReceiveBuffer. Returns the number
// of bytes appended (>=0), OD_LAN_READ_CLOSED on a graceful peer close, or
// OD_LAN_READ_ERROR on a fatal channel error (caller drops in both cases).
static int lanReadIntoBuffer(void) {
    // Belt-and-braces: startLanServer() refuses to listen without the buffer, so no
    // socket should reach here, but never index a null on the read path.
    if (tcpReceiveBuffer == nullptr) return OD_LAN_READ_ERROR;
    // OD_LAN_RX_BUFFER_SIZE, not sizeof(): tcpReceiveBuffer is a pointer now, and
    // sizeof would silently yield 4/8 -- reads would collapse to a few bytes per tick
    // and read as a network fault rather than a code bug.
    int space = (int)OD_LAN_RX_BUFFER_SIZE - (int)tcpReceiveBufferPos;
    if (space <= 0) {
        od_log_info("LAN RX buffer full, dropping connection");
        return -1;
    }
    if (tlsMode) {
        int r = mbedtls_ssl_read(&tlsSsl, &tcpReceiveBuffer[tcpReceiveBufferPos], (size_t)space);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        // A GRACEFUL close is not a fault. The client sends close_notify right after its
        // last frame (e.g. the 0x0073 ack at the end of a push), so every successful
        // transfer used to end with "channel read error" -- alarming and wrong. Separate
        // it from a real failure so the log distinguishes "done" from "broken".
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return OD_LAN_READ_CLOSED;
        if (r < 0) {
            s_lastLanReadErr = r;      // carried out so the caller can name the code
            return OD_LAN_READ_ERROR;
        }
        tcpReceiveBufferPos += (uint32_t)r;
        return r;
    }
    int available = lanClientAvailable();
    if (available <= 0) return 0;
    int bytesToRead = (available > space) ? space : available;
    int bytesRead = lanClientRead(&tcpReceiveBuffer[tcpReceiveBufferPos], bytesToRead);
    if (bytesRead > 0) tcpReceiveBufferPos += (uint32_t)bytesRead;
    return (bytesRead > 0) ? bytesRead : 0;
}

void wifiLanReapClosedSession(void) {
    // Notice a peer-closed socket EARLY in the pass, so the deferred cleanup that
    // this raises releases the token before handleWiFiServer()'s accept runs later
    // in the same pass. See the call site in loop() for why "early" is the whole
    // point: raising it from inside handleWiFiServer left the accept testing a
    // corpse's token and refusing an ordinary reconnect.
    if (wifiServerConnected && !lanClientConnected()) {
        od_log_info("LAN: peer closed the socket, reaping the session");
        disconnectWiFiServer();
    }
}

// Decide a freshly accepted socket's fate: admit it as the owner, or refuse it.
//
// Split out of handleWiFiServer() so the caller can CONTINUE servicing an existing
// session in the same pass whatever the outcome. When refusal returned early from
// handleWiFiServer, a host reconnecting every pass starved the incumbent -- its TLS
// handshake never advanced and its buffered frames were never dispatched, so the
// shared activity clock stopped being stamped and the end-of-pass idle timeout
// eventually dropped it with valid commands still unread. Refusal must be inert
// (R3), and inbound traffic must be parsed before the idle check (R7d step 3 before
// step 4); an early return broke both.
static void admitOrRefuseLanClient(int incomingFd) {
    // REFUSE while the slot is held -- never evict. Two reasons, both concrete:
    //
    //  - The eviction path this replaces closed the previous socket without
    //    releasing its ownership epoch, and the replacement then lost its own
    //    claim, so the departed session's identity became unreachable and the slot
    //    stayed held until reboot.
    //  - Eviction is unauthenticated by design: LAN-TLS bypasses app-layer auth, so
    //    any host on the network could kill an in-flight display push by opening a
    //    socket, with no credentials.
    //
    // The incumbent is untouched: no teardown, no crypto clear, no cleanup flag.
    // That is R3's "refusal is inert", and it also removes a pre-existing bug in the
    // eviction it replaces -- that path cleared TLS and crypto but never requested
    // transfer cleanup, so an evicted client's in-flight transfer stayed live and
    // the new client's frames (same ORIGIN_LAN, so frameOwnsSession could not tell
    // them apart) landed in it.
    const LinkId held = linkOwnerId();
    if (held.who != OWNER_NONE) {
        od_log_info("LAN: refusing new client, slot held by %s",
               held.who == OWNER_BLE ? "BLE" : "LAN");
        lwip_close(incomingFd);
        return;
    }

    s_lanClientFd = incomingFd;
    // TCP_NODELAY: every LAN write is a complete, self-delimited frame, so there is
    // never a following write for Nagle to coalesce it with -- it can only hold a
    // small frame until the peer's delayed ACK fires (40-200 ms). With per-chunk
    // direct-write ACKs that lands on every frame of a transfer.
    lanSockSetNoDelay(s_lanClientFd);
    lanSockSetRcvTimeout(s_lanClientFd, 30000);
    lanSockSetSndTimeout(s_lanClientFd, LAN_SEND_TIMEOUT_MS);
    tcpReceiveBufferPos = 0;
    wifiServerConnected = true;

    // Claim at TCP ACCEPT, before the TLS handshake (7a row 2): the handshake is
    // driven incrementally across later passes, so deferring the claim until it
    // completes would leave the slot free for a BLE connect or a second socket in
    // the meantime. The accept is this transport's earliest hook, so it is the same
    // "claim at the earliest hook" rule the BLE connect callback follows -- and the
    // same CAS, which is what makes cross-transport arbitration the word itself
    // rather than loop ordering.
    //
    // The slot was free a moment ago, so this normally wins. It can still lose to a
    // BLE connect landing on the host task in between: the callback is the
    // authoritative arbitration point, not this loop-side test (R7d).
    s_lanEpoch = linkNextEpoch();
    if (!linkClaim((LinkId){OWNER_LAN, 0, s_lanEpoch})) {
        od_log_info("LAN: refusing new client, slot claimed concurrently");
        s_lanEpoch = 0;
        lanSockClose(&s_lanClientFd);
        wifiServerConnected = false;
        return;
    }

    char peerIp[16];
    lanSockPeerIpStr(s_lanClientFd, peerIp, sizeof(peerIp));
    od_log_info("LAN client connected from %s", peerIp);
    if (tlsMode && !tlsBeginSession()) {
        od_log_info("LAN: TLS session start failed, dropping");
        disconnectWiFiServer();
    }
}

bool wifiLinkIsUp(void) {
    return odWifiStatus() == OD_WIFI_CONNECTED;
}

void wifiLocalIpStr(char* out, size_t out_size) {
    odWifiIpStr(out, out_size);
}

/* Is a COMPLETE frame already sitting at the head of the receive buffer? The length prefix is
 * [len:2 LE] and the frame is complete when the buffer holds the prefix plus that many bytes.
 *
 * The question is "can the parser make progress without reading more", which is the gate for both
 * pausing the socket and for parsing when no new bytes arrived. One predicate for both, because
 * they are the same condition and drifting them apart is how the deadlock came back. */
static bool lanFrameBuffered(void) {
    if (tcpReceiveBufferPos < 2u) {
        return false;
    }
    const uint32_t flen = (uint32_t)(tcpReceiveBuffer[0] | (tcpReceiveBuffer[1] << 8));
    return tcpReceiveBufferPos >= 2u + flen;
}

void handleWiFiServer() {
    // Execute a queued roam (RSSI dropped below OD_LAN_ROAM_RSSI_THRESHOLD) before any
    // other work; it self-gates on idle, so this is a no-op mid-transfer.
    serviceLanRoam();

    if (wifiInitialized && odWifiStatus() == OD_WIFI_CONNECTED && !wifiConnected) {
        wifiConnected = true;
        od_log_info("=== WiFi connected ===");
        char ipStr[16];
        char bssidStr[18] = "";
        odWifiIpStr(ipStr, sizeof(ipStr));
        odWifiBssidStr(bssidStr, sizeof(bssidStr));
        od_log_info("IP: %s, RSSI %d dBm, ch %d, BSSID %s",
               ipStr, (int)odWifiRssi(), (int)odWifiChannel(), bssidStr);
        startLanServer();
    }
    if (!wifiConnected || odWifiStatus() != OD_WIFI_CONNECTED) {
        if (wifiServerConnected || lanClientConnected()) {
            od_log_info("WiFi lost, closing LAN session");
            disconnectWiFiServer();
        }
        return;
    }

    // Accept-side refusal must NOT return from this function. Everything below --
    // driving the incumbent's TLS handshake, reading its socket, dispatching its
    // frames -- is what stamps the shared activity clock, and the idle timeout is
    // evaluated at the end of this same pass. An early return on refusal therefore
    // lets a contender starve the incumbent: a host that reconnects every pass
    // stops the incumbent being serviced at all, and after OD_LAN_READ_TIMEOUT_S
    // the idle drop kills it -- with valid commands still sitting unread in its
    // socket. That is both an R3 violation (refusal must be inert) and an R7d one
    // (step 3 must precede step 4). So the refusal branch closes the contender and
    // falls through.
    int incomingFd = lanAcceptOne();
    if (incomingFd >= 0) {
        admitOrRefuseLanClient(incomingFd);
        // Deliberately no return here on either outcome -- see the note above.
    }

    if (!wifiServerConnected || !lanClientConnected()) {
        if (wifiServerConnected) {
            od_log_info("LAN client disconnected");
            disconnectWiFiServer();
        }
        return;
    }

    // Drive the TLS handshake incrementally; return until it completes.
    if (tlsMode && tlsSessionActive && !tlsHandshakeDone) {
        int hs = mbedtls_ssl_handshake(&tlsSsl);
        if (hs == 0) {
            tlsHandshakeDone = true;
            // The idle baseline starts HERE, not at TCP accept: handshake traffic
            // is not a command (R4/7a). A handshake that never completes therefore
            // leaves the clock at its accept-time stamp and the ordinary 30 s drop
            // reclaims the socket -- which is why no separate handshake deadline is
            // needed.
            linkStampOwnerCommand();
            od_log_info("LAN: TLS handshake complete");
        } else if (hs == MBEDTLS_ERR_SSL_WANT_READ || hs == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // still handshaking; but honor the idle timeout below
        } else {
            // The handshake struct is another internal-DRAM allocation, so annotate the
            // heap here too -- an OOM mid-handshake looks like a protocol error otherwise.
            char note[128];
            od_log_info("LAN: TLS handshake failed%s, dropping", tlsFailNote(hs, note, sizeof(note)));
            disconnectWiFiServer();
            return;
        }
    }

    // Bounded drain: keep reading and dispatching until the channel is dry or a
    // per-tick byte budget (one full tcpReceiveBuffer) is spent. Parsing INSIDE
    // the loop frees buffer space before the next read -- essential on TLS, where
    // one mbedtls_ssl_read can return a whole max-size record. The budget bounds
    // LAN's hold on loop() so BLE drain, buttons/touch/buzzer, and the deep-sleep
    // gate still run under a saturating streaming client.
    uint32_t drainedBytes = 0;
    int got;
    do {
        /* STOP READING WHILE A FRAME IS RETAINED. od_dispatch returns DEFERRED without consuming
         * the frame, so it stays at the head of this buffer to be re-offered unchanged -- and
         * until it is consumed, nothing behind it can be dispatched either.
         *
         * Reading on regardless is not merely useless, it is fatal with a pipelining client: bytes
         * keep accumulating behind a head that cannot move, and when the 16 KiB buffer fills
         * lanReadIntoBuffer() reports an error and the connection is dropped -- turning ordinary
         * backpressure into a disconnect. Skipping the read applies real TCP backpressure instead:
         * the receive window closes and the peer stops sending, which is what the transport is for.
         *
         * A retained frame is also exactly the case where no new bytes will arrive -- a client
         * waiting on its response sends nothing more -- so this is the same condition that would
         * otherwise return early below. */
        if (lanFrameBuffered()) {
            got = 0;
        } else {
            got = lanReadIntoBuffer();
        }
        if (got == OD_LAN_READ_CLOSED) {
            // Normal end of a push: the client finished and sent close_notify.
            od_log_info("LAN: client closed the connection");
            disconnectWiFiServer();
            return;
        }
        if (got < 0) {
            // Real fault -- name the mbedTLS code, otherwise this is undiagnosable.
            char note[128];
            od_log_info("LAN: channel read error%s, dropping",
                   tlsFailNote(s_lastLanReadErr, note, sizeof(note)));
            disconnectWiFiServer();
            return;
        }
        if (got > 0) {
            // NOT an activity stamp. R4 defines activity as a RECOGNISED COMMAND
            // from the owner, and this site fires on any bytes read -- before
            // framing, opcode recognition, ownership or authentication -- so a
            // plain-mode flooder could hold the slot indefinitely with garbage and
            // defeat both the 30 s read timeout and anything built on this clock.
            // That is the same defect the BLE clock had in its intake-stamping
            // draft, and it is fixed the same way: the stamp moved to the dispatch
            // site below, which is reached only by a framed, recognised command.
            drainedBytes += (uint32_t)got;
        } else if (drainedBytes == 0) {
            // Nothing NEW to read this tick -- but a complete frame may already be buffered,
            // and if it is, this must fall through to the parser rather than return.
            //
            // A DEFERRED frame is left in this buffer deliberately, to be re-offered unchanged.
            // The client that sent it is now waiting for its response and will send nothing more,
            // so "no new bytes" is the steady state, not a reason to stop: returning here would
            // strand that command until the connection died. The reason it was deferred -- TX
            // capacity, or a live CONFIG_READ -- clears from the loop, so re-parsing the same
            // bytes on a later pass is exactly what makes progress.
            if (!lanFrameBuffered()) {
                // The idle DROP is not decided here any more: serviceIdleTimeout() owns it for
                // both transports, and it must run after inbound traffic has been parsed (7d
                // step 4) or a LAN client is dropped with its command already sitting in the
                // buffer.
                return;
            }
        }

        while (tcpReceiveBufferPos >= 2) {
            uint16_t flen = (uint16_t)(tcpReceiveBuffer[0] | (tcpReceiveBuffer[1] << 8));
            if (flen == 0 || flen > OD_LAN_MAX_PAYLOAD) {
                od_log_info("LAN: invalid frame length, closing");
                disconnectWiFiServer();
                return;
            }
            if (tcpReceiveBufferPos < (uint32_t)(2 + flen)) {
                break;
            }
            // THE FRAME'S REPLY CONTEXT, built here because this is where both halves are
            // known. The origin makes the dispatcher bypass app-layer CCM on TLS (already
            // secure) and routes the response back over LAN only. The instance is the live
            // owner word: LAN frames never traverse the BLE ring, so there is no queued tag
            // to carry -- the socket is parsed and dispatched within this pass and its buffer
            // dies with the session -- and publishing the owner directly is exact, because if
            // LAN does not own the slot the word will not match and the frame stamps nothing.
            //
            // No stamp here: od_dispatch_app_frame() stamps the shared clock itself, and only
            // for a RECOGNISED command from the owner, which is the whole of R4's definition
            // and what this site could not enforce.
            const LinkId lanOwner = linkOwnerId();
            const od_reply_t rp = {
                tlsMode ? OD_ORIGIN_LAN_TLS : OD_ORIGIN_LAN_PLAIN,
                (lanOwner.who == OWNER_LAN) ? linkIdWord(lanOwner) : 0u
            };
            const od_frame_outcome_t outcome =
                od_dispatch_app_frame(&rp, tcpReceiveBuffer + 2, flen);
            // DEFERRED means the dispatcher did not consume this frame and it must be re-offered
            // byte-identical. Leave it at the head of the socket buffer and stop draining: the
            // reason it was deferred -- no response capacity, or a live config read -- clears from
            // the loop, not from another parse of the same bytes. Removing it here would be the
            // silent command loss DEFERRED exists to prevent, and on LAN there is no retransmit
            // behind it to cover for that.
            if (!od_frame_policy(outcome).consume_rx) {
                return;
            }
            uint32_t consumed = 2u + (uint32_t)flen;
            uint32_t rem = tcpReceiveBufferPos - consumed;
            if (rem > 0) {
                memmove(tcpReceiveBuffer, tcpReceiveBuffer + consumed, rem);
            }
            tcpReceiveBufferPos = rem;
        }
        // A dispatched command may have torn the session down (reboot, power-off,
        // config-driven LAN restart). Never read from a dead client.
        if (!wifiServerConnected || !lanClientConnected()) {
            return;
        }
    } while (got > 0 && drainedBytes < OD_LAN_RX_BUFFER_SIZE);
}

void restartWiFiLanAfterReconnect() {
    if (!wifiConnected || odWifiStatus() != OD_WIFI_CONNECTED) {
        return;
    }
    disconnectWiFiServer();
    startLanServer();
}

// F5 -- reboot teardown: drop the LAN client + TLS context, stop the TCP server,
// and disconnect WiFi so esp_restart() does not leave the radio/socket half-up.
// Extends PR #114's BLE-only teardown (device_control.cpp) to the WiFi surface.
void opendisplay_lan_teardown(void) {
    tlsCloseSession();
    // Ownership, not liveness -- see lanClientConnected(). Reboot teardown must hand
    // the descriptor back whatever the peer's state; lanSockClose() no-ops on -1.
    lanSockClose(&s_lanClientFd);
    lanSockClose(&s_lanListenFd);
    wifiServerConnected = false;
    tcpReceiveBufferPos = 0;
    // Withdraw the advertisement. The shim's MDNS.end() was a no-op like the rest of
    // it, so this never happened: against a real responder, skipping it means no
    // goodbye packet is sent and clients keep a cached record pointing at a device
    // that is rebooting -- they retry the port through the whole restart.
    odMdnsStop();
    if (tlsInited) {
        mbedtls_ssl_config_free(&tlsConf);
        mbedtls_ctr_drbg_free(&tlsDrbg);
        mbedtls_entropy_free(&tlsEntropy);
        tlsInited = false;
    }
    odWifiDisconnect(true);
    od_log_info("LAN/WiFi torn down before restart");
}

#endif  // OPENDISPLAY_HAS_WIFI
