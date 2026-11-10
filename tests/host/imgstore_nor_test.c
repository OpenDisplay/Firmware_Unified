/* imgstore_nor: od_imgstore + od_flash_nor over a simulated SPI NOR.
 *
 * The simulator is the od_hal_spi seam: it decodes 06/05/03/02/20/9F/B9/AB frames against a
 * byte array with real NOR semantics -- program ANDs, needs WEL, wraps inside its page; erase
 * sets a 4 KB sector to 0xFF; deep power-down ignores everything but 0xAB; addresses are 24-bit
 * and alias modulo the array. It counts programs per byte since the last erase (the Puya
 * P25Q16H corrupts a range programmed twice) and can cut power at any byte of any program or
 * erase, after which every transfer fails until "reboot".
 *
 * Built twice: default headers, and OD_IMGSTORE_WRITE_ONCE_HEADER=1 (the Nordic XIAO build).
 */
#include "od_check.h"
#include "drive_fixture.h"   /* KNOWN_BUG */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "od_flash_nor.h"
#include "od_hal_spi.h"
#include "od_hal_time.h"
#include "od_imgstore.h"

#ifndef OD_IMGSTORE_WRITE_ONCE_HEADER
#define OD_IMGSTORE_WRITE_ONCE_HEADER 0
#endif
#define HDR (OD_IMGSTORE_WRITE_ONCE_HEADER ? 256u : 32u)

/* ---------------------------------------------------------------------------- simulator -- */

#define SIM_MAX (16u * 1024u * 1024u)

static uint8_t  s_mem[SIM_MAX];
static uint8_t  s_prog[SIM_MAX];          /* programs since last erase, saturating */
static uint32_t s_size = 64u * 1024u;     /* array actually present (24-bit aliasing) */
static uint8_t  s_id[3] = { 0xEF, 0x40, 0x10 };
static bool     s_wel, s_pd, s_open, s_dead;
static unsigned s_ops;                    /* programs + erases started */
static long     s_cut_op = -1;            /* op index that loses power, -1 = never */
static uint32_t s_cut_byte;               /* bytes of that op that land */
static unsigned s_wrapped;                /* page programs that wrapped inside a page */
static unsigned s_unaligned_erase;
static char     s_log[256];
static size_t   s_log_n;

static uint8_t  s_frame[4u + 4096u];
static uint32_t s_frame_n;
static bool     s_in_frame;

static void sim_log(uint8_t op)
{
    if (s_log_n + 4u < sizeof s_log) {
        s_log_n += (size_t)snprintf(s_log + s_log_n, sizeof s_log - s_log_n, "%02x ", op);
    }
}

static uint32_t a24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

/* true = the op ran to completion */
static bool sim_cut_check(uint32_t len, uint32_t *land)
{
    const long me = (long)s_ops++;
    *land = len;
    if (me == s_cut_op) {
        *land = s_cut_byte < len ? s_cut_byte : len;
        s_dead = true;
        return false;
    }
    return true;
}

static void sim_exec(const uint8_t *tx, uint32_t n, uint8_t *rx, uint32_t rx_n)
{
    if (n == 0u) {
        return;
    }
    const uint8_t op = tx[0];
    if (s_pd && op != 0xABu) {
        if (rx) memset(rx, 0xFF, rx_n);
        return;
    }
    sim_log(op);
    switch (op) {
    case 0x06: s_wel = true; break;
    case 0x05: if (rx && rx_n) { memset(rx, 0, rx_n); } break;   /* never busy */
    case 0x9F: if (rx) { for (uint32_t i = 0; i < rx_n; i++) rx[i] = i < 3u ? s_id[i] : 0xFFu; } break;
    case 0xB9: s_pd = true; break;
    case 0xAB: s_pd = false; break;
    case 0x03: {
        const uint32_t a = a24(&tx[1]);
        for (uint32_t i = 0; i < rx_n; i++) rx[i] = s_mem[(a + i) % s_size];
        break;
    }
    case 0x02: {
        if (!s_wel || n < 4u) break;
        s_wel = false;
        const uint32_t a = a24(&tx[1]) % s_size;
        const uint32_t len = n - 4u;
        uint32_t land;
        (void)sim_cut_check(len, &land);
        if (len > 256u || (a % 256u) + len > 256u) s_wrapped++;
        for (uint32_t i = 0; i < land; i++) {
            const uint32_t at = (a & ~0xFFu) | ((a + i) & 0xFFu);
            s_mem[at] &= tx[4u + i];
            /* A 0xFF byte leaves its cells alone: only count bytes that program something. */
            if (tx[4u + i] != 0xFFu && s_prog[at] < 255u) s_prog[at]++;
        }
        break;
    }
    case 0x20: {
        if (!s_wel || n < 4u) break;
        s_wel = false;
        const uint32_t raw = a24(&tx[1]) % s_size;
        if (raw % 4096u) s_unaligned_erase++;
        const uint32_t a = raw & ~4095u;
        uint32_t land;
        (void)sim_cut_check(4096u, &land);
        memset(&s_mem[a], 0xFF, land);
        memset(&s_prog[a], 0, land);
        break;
    }
    default: break;
    }
}

int od_hal_spi_open(const od_hal_spi_pins_t *pins) { (void)pins; s_open = true; return OD_HAL_SPI_OK; }
void od_hal_spi_close(void) { s_open = false; }

int od_hal_spi_xfer(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
    if (s_dead || !s_open) return OD_HAL_SPI_EIO;
    sim_exec(tx, tx_len, rx, rx_len);
    return OD_HAL_SPI_OK;
}
int od_hal_spi_begin(void)
{
    if (s_dead || !s_open) return OD_HAL_SPI_EIO;
    s_in_frame = true;
    s_frame_n = 0u;
    return OD_HAL_SPI_OK;
}
int od_hal_spi_cont(const uint8_t *tx, uint32_t len)
{
    if (s_dead || !s_in_frame || s_frame_n + len > sizeof s_frame) return OD_HAL_SPI_EIO;
    memcpy(&s_frame[s_frame_n], tx, len);
    s_frame_n += len;
    return OD_HAL_SPI_OK;
}
int od_hal_spi_end(void)
{
    if (s_dead) return OD_HAL_SPI_EIO;
    if (s_in_frame) sim_exec(s_frame, s_frame_n, NULL, 0u);
    s_in_frame = false;
    return OD_HAL_SPI_OK;
}
void od_hal_delay_us(uint32_t us) { (void)us; }

static void sim_power_on(uint32_t size, uint8_t cap_code)
{
    s_size = size;
    s_id[2] = cap_code;
    memset(s_mem, 0xFF, size);
    memset(s_prog, 0, size);
    s_wel = s_pd = s_dead = false;
    s_ops = 0u;
    s_cut_op = -1;
    s_wrapped = s_unaligned_erase = 0u;
}

static const od_hal_spi_pins_t PINS = { 1, 2, 3, 4 };
#define GEOM 0x12345678u
#define PAYLOAD_MAX 5000u

/* ------------------------------------------------------------------------------ helpers -- */

static void pattern(uint8_t *p, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 13u + (i >> 8));
}

static bool put_image(uint8_t slot, uint8_t seed, uint32_t n, uint32_t chunk, uint32_t *digest)
{
    static uint8_t img[PAYLOAD_MAX];
    pattern(img, n, seed);
    if (!od_imgstore_begin(slot, 400u, 300u, 0u, 1u, n)) return false;
    for (uint32_t o = 0; o < n; o += chunk) {
        const uint32_t c = n - o < chunk ? n - o : chunk;
        if (!od_imgstore_write(img + o, c)) return false;
    }
    return od_imgstore_finish(digest);
}

/* VALID, right header, payload and digest match image `seed`. */
static bool image_is(uint8_t slot, uint8_t seed, uint32_t n)
{
    static uint8_t want[PAYLOAD_MAX], got[PAYLOAD_MAX];
    od_imgstore_slot_t s;
    pattern(want, n, seed);
    if (!od_imgstore_slot(slot, &s) || s.state != OD_SLOT_VALID || s.payload_len != n ||
        s.width != 400u || s.height != 300u) return false;
    if (!od_imgstore_read(slot, 0u, got, n) || memcmp(want, got, n) != 0) return false;
    return s.digest == od_imgstore_crc32(0u, want, n);
}

/* --------------------------------------------------------------------------- flash_nor -- */

static void test_nor(void)
{
    od_flash_nor_info_t info;
    uint8_t b[300];

    CASE("init wakes the part before reading the id");
    sim_power_on(64u * 1024u, 0x10u);
    s_pd = true;                          /* slept by a previous boot */
    s_log_n = 0u; s_log[0] = '\0';
    CHECK(od_flash_nor_init(&PINS, &info));
    CHECK(info.present && info.size_bytes == 65536u && info.manufacturer == 0xEFu);
    CHECK(strncmp(s_log, "ab 9f", 5) == 0);

    CASE("deinit sleeps it; a second init still finds it");
    od_flash_nor_deinit();
    CHECK(s_pd);
    CHECK(od_flash_nor_init(&PINS, &info));

    CASE("id 00 / FF (floating bus) and capacity codes outside 0x10..0x1C are absent");
    s_id[0] = 0xFFu;
    CHECK(!od_flash_nor_init(&PINS, &info));
    s_id[0] = 0x00u;
    CHECK(!od_flash_nor_init(&PINS, &info));
    s_id[0] = 0xEFu; s_id[2] = 0x0Fu;
    CHECK(!od_flash_nor_init(&PINS, &info));
    s_id[2] = 0x1Du;
    CHECK(!od_flash_nor_init(&PINS, &info));

    CASE("NOR-4BYTE: a 32 MiB part (code 0x19) is accepted at full size on 3-byte addressing");
    s_id[2] = 0x19u;
    CHECK(od_flash_nor_init(&PINS, &info));
    /* addr3() drops bit 24; no 0xB7 / 4-byte opcodes are sent, so every address >= 16 MiB
     * aliases onto the bottom of the part (or a bank register's current bank). */
    KNOWN_BUG("NOR-4BYTE", info.size_bytes <= SIM_MAX);
    s_id[2] = 0x10u;
    CHECK(od_flash_nor_init(&PINS, &info));

    CASE("program: no page crossing, no zero length, nothing past the end");
    memset(b, 0x00, sizeof b);
    CHECK(!od_flash_nor_program_page(0x0F0u, b, 32u));        /* 0xF0 + 32 crosses 0x100 */
    CHECK(od_flash_nor_program_page(0x0F0u, b, 16u));
    CHECK(!od_flash_nor_program_page(0u, b, 0u));
    CHECK(!od_flash_nor_program_page(0u, b, 257u));
    CHECK(!od_flash_nor_program_page(65536u - 8u, b, 16u));
    CHECK(od_flash_nor_program_page(65536u - 16u, b, 16u));
    CHECK(s_wrapped == 0u);

    CASE("erase: aligned, in range");
    CHECK(!od_flash_nor_erase_sector(100u));
    CHECK(!od_flash_nor_erase_sector(65536u));
    CHECK(od_flash_nor_erase_sector_sync(61440u, 10u));
    CHECK(s_mem[65535] == 0xFFu && s_unaligned_erase == 0u);

    CASE("read bounds");
    CHECK(od_flash_nor_read(65536u - 4u, b, 4u));
    CHECK(!od_flash_nor_read(65536u - 4u, b, 5u));
    CHECK(!od_flash_nor_read(0u, b, 0u));
    CHECK(!od_flash_nor_read(65537u, b, 1u));
    od_flash_nor_deinit();
}

/* ----------------------------------------------------------------------------- imgstore -- */

static void test_store_basics(void)
{
    od_imgstore_slot_t s;
    uint32_t dg = 0u;

    CASE("fresh part: mount formats; slot geometry");
    sim_power_on(64u * 1024u, 0x10u);
    CHECK(od_imgstore_mount(&PINS, GEOM, PAYLOAD_MAX));
    CHECK(od_imgstore_status()->slot_stride == 8192u);
    CHECK(od_imgstore_status()->slot_count == 7u);
    for (uint8_t i = 0; i < 7u; i++) {
        CHECK(od_imgstore_slot(i, &s) && s.state == OD_SLOT_FREE);
    }
    CHECK(!od_imgstore_slot(7u, &s));

    CASE("write + read back, odd chunk sizes crossing every page boundary");
    CHECK(put_image(0u, 1u, PAYLOAD_MAX, 77u, &dg));
    CHECK(image_is(0u, 1u, PAYLOAD_MAX));
    CHECK(s_wrapped == 0u);
    CHECK(put_image(1u, 2u, 1u, 1u, NULL));       /* a one-byte image */
    CHECK(image_is(1u, 2u, 1u));

    CASE("Puya write-once: no byte programmed twice between erases");
    {
        unsigned twice = 0u;
        for (uint32_t a = 0; a < s_size; a++) twice += s_prog[a] > 1u;
#if OD_IMGSTORE_WRITE_ONCE_HEADER
        CHECK(twice == 0u);
#else
        /* Default headers program the state byte twice (WRITING, then VALID) in each of the
         * two written slots: why the P25Q16H build needs the write-once layout. */
        CHECK(twice == 2u);
#endif
    }

    CASE("rewrite in place erases first");
    CHECK(put_image(0u, 9u, 3000u, 256u, NULL));
    CHECK(image_is(0u, 9u, 3000u));

    CASE("remount with the same geometry keeps slots; a different one drops them");
    od_imgstore_unmount();
    CHECK(od_imgstore_mount(&PINS, GEOM, PAYLOAD_MAX));
    CHECK(image_is(0u, 9u, 3000u) && image_is(1u, 2u, 1u));
    od_imgstore_unmount();
    CHECK(od_imgstore_mount(&PINS, GEOM ^ 1u, PAYLOAD_MAX));
    CHECK(od_imgstore_slot(0u, &s) && s.state == OD_SLOT_FREE);
    od_imgstore_unmount();
    CHECK(od_imgstore_mount(&PINS, GEOM ^ 1u, 9000u));       /* stride change: reformat */
    CHECK(od_imgstore_status()->slot_stride == 12288u);

    CASE("refusals: bad index, zero / oversize payload, write past length, nested begin");
    CHECK(!od_imgstore_begin(od_imgstore_status()->slot_count, 1u, 1u, 0u, 1u, 10u));
    CHECK(!od_imgstore_begin(0u, 1u, 1u, 0u, 1u, 0u));
    CHECK(!od_imgstore_begin(0u, 1u, 1u, 0u, 1u, 12288u - HDR + 1u));
    CHECK(od_imgstore_begin(0u, 1u, 1u, 0u, 1u, 12288u - HDR));
    CHECK(!od_imgstore_begin(1u, 1u, 1u, 0u, 1u, 10u));
    CHECK(!od_imgstore_erase(1u) && !od_imgstore_erase_all());
    od_imgstore_abort();
    CHECK(od_imgstore_begin(0u, 1u, 1u, 0u, 1u, 10u));
    {
        uint8_t x[11] = { 0 };
        CHECK(!od_imgstore_write(x, 11u));
        CHECK(od_imgstore_write(x, 4u));
        CASE("a short payload is a failed transfer: not VALID");
        CHECK(!od_imgstore_finish(NULL));
        CHECK(od_imgstore_slot(0u, &s) && s.state != OD_SLOT_VALID);
        CHECK(!od_imgstore_write(x, 1u));
    }

    CASE("full store: every slot written, then erase one / erase all");
    od_imgstore_unmount();
    CHECK(od_imgstore_mount(&PINS, GEOM, PAYLOAD_MAX));
    for (uint8_t i = 0; i < od_imgstore_status()->slot_count; i++) {
        CHECK(put_image(i, (uint8_t)(i + 40u), 4000u + i, 500u, NULL));
    }
    for (uint8_t i = 0; i < od_imgstore_status()->slot_count; i++) {
        CHECK(image_is(i, (uint8_t)(i + 40u), 4000u + i));
    }
    CHECK(od_imgstore_erase(3u));
    CHECK(od_imgstore_slot(3u, &s) && s.state == OD_SLOT_FREE);
    CHECK(image_is(2u, 42u, 4002u) && image_is(4u, 44u, 4004u));
    CHECK(od_imgstore_erase_all());
    for (uint8_t i = 0; i < od_imgstore_status()->slot_count; i++) {
        CHECK(od_imgstore_slot(i, &s) && s.state == OD_SLOT_FREE);
    }

    CASE("read: offset/length bounds and non-VALID slots");
    CHECK(put_image(0u, 5u, 100u, 100u, NULL));
    {
        uint8_t x[8];
        CHECK(od_imgstore_read(0u, 96u, x, 4u));
        CHECK(!od_imgstore_read(0u, 97u, x, 4u));
        CHECK(!od_imgstore_read(1u, 0u, x, 1u));
    }
    od_imgstore_unmount();

    CASE("unmounted: everything refuses");
    CHECK(!od_imgstore_slot(0u, &s) && !od_imgstore_erase(0u));
    CHECK(!od_imgstore_begin(0u, 1u, 1u, 0u, 1u, 1u));
}

/* Power cut at every byte of every flash op while overwriting slot 0 (image A) with C; slot 1
 * holds B throughout. After "reboot" (remount), slot 0 must be A, C or not VALID, slot 1 must
 * be B, and the store must not have reformatted. */
static void test_power_cut(void)
{
    static uint8_t base[64u * 1024u], base_prog[64u * 1024u];
    unsigned runs = 0u, bad_neigh = 0u, valid_wrong = 0u, ended_c = 0u, ended_free = 0u;
    unsigned overread = 0u;
    long op;

    CASE("setup");
    sim_power_on(64u * 1024u, 0x10u);
    CHECK(od_imgstore_mount(&PINS, GEOM, PAYLOAD_MAX));
    CHECK(put_image(0u, 0xA0u, 3000u, 200u, NULL));
    CHECK(put_image(1u, 0xB0u, 2000u, 200u, NULL));
    od_imgstore_unmount();
    memcpy(base, s_mem, sizeof base);
    memcpy(base_prog, s_prog, sizeof base_prog);

    CASE("power cut at every byte of every op");
    for (op = 0;; op++) {
        bool reached = false;
        for (uint32_t k = 0; k <= 256u; k++) {
            memcpy(s_mem, base, sizeof base);
            memcpy(s_prog, base_prog, sizeof base_prog);
            s_wel = s_pd = s_dead = false;
            s_ops = 0u;
            /* Mount is not the op under test: start counting after it. */
            (void)od_imgstore_mount(&PINS, GEOM, PAYLOAD_MAX);
            s_ops = 0u;
            s_cut_op = op;
            s_cut_byte = k;
            (void)put_image(0u, 0xC0u, 3000u, 200u, NULL);
            od_imgstore_unmount();
            if ((long)s_ops <= op) {
                break;                    /* the write finished before op `op` */
            }
            reached = true;
            runs++;
            s_dead = false;
            s_cut_op = -1;
            s_pd = false;
            if (!od_imgstore_mount(&PINS, GEOM, PAYLOAD_MAX) || !image_is(1u, 0xB0u, 2000u)) {
                bad_neigh++;
            }
            od_imgstore_slot_t s;
            if (od_imgstore_slot(0u, &s) && s.state == OD_SLOT_VALID) {
                if (image_is(0u, 0xC0u, 3000u)) {
                    ended_c++;
                } else if (!image_is(0u, 0xA0u, 3000u)) {
                    valid_wrong++;
                    uint8_t x[16];
                    /* 16 bytes starting at the neighbour's header: past slot 0's own end. */
                    if (s.payload_len > od_imgstore_status()->slot_stride - HDR &&
                        od_imgstore_read(0u, od_imgstore_status()->slot_stride - HDR, x,
                                         sizeof x)) {
                        overread++;
                    }
                }
            } else {
                ended_free++;
            }
            od_imgstore_unmount();
            if (k >= 256u) {
                break;
            }
        }
        if (!reached) {
            break;
        }
    }
    s_cut_op = -1;
    printf("  power cut: %u runs over %ld ops: %u ended C, %u not VALID, %u VALID-but-wrong "
           "(%u with payload_len past the slot), %u neighbour/superblock damage\n",
           runs, op, ended_c, ended_free, valid_wrong, overread, bad_neigh);
    CHECK(runs > 1000u);
    CHECK(bad_neigh == 0u);
    CHECK(ended_c > 0u && ended_free > 0u);
    /* The VALID transition and the fields it vouches for share one page program. A cut
     * part-way through it -- state byte down, digest (and in write-once mode payload_len,
     * width, height) not yet -- leaves a slot that reads VALID with an erased digest or
     * length. Default mode: payload intact, digest 0xFFFFFFFF/partial (the LIST answer is
     * wrong). Write-once mode: payload_len reads up to 0xFFFFFFFF, so od_imgstore_read()
     * bounds-checks against it and reads past the slot into the neighbour. */
    KNOWN_BUG("IMG-TORN-VALID", valid_wrong == 0u);
#if OD_IMGSTORE_WRITE_ONCE_HEADER
    KNOWN_BUG("IMG-TORN-VALID-OVERREAD", overread == 0u);
#else
    CHECK(overread == 0u);
#endif
}

static void test_4byte_alias(void)
{
    CASE("NOR-4BYTE: a 32 MiB part, slots past 16 MiB alias onto the superblock");
    sim_power_on(SIM_MAX, 0x19u);         /* 16 MiB of real array behind 3-byte addressing */
    CHECK(od_imgstore_mount(&PINS, GEOM, 600000u));
    const uint8_t n = od_imgstore_status()->slot_count;
    CHECK(n == 32u);
    CHECK(put_image(0u, 0x11u, 4000u, 256u, NULL));
    /* Slot 27 spans 16261120..16863231: its second sector erase is address 0x1000000, which
     * 3-byte addressing sends as 0x000000 -- the superblock. */
    CHECK(put_image(27u, 0x22u, 4000u, 256u, NULL));
    od_imgstore_unmount();
    const bool kept = od_imgstore_mount(&PINS, GEOM, 600000u) && image_is(0u, 0x11u, 4000u);
    KNOWN_BUG("NOR-4BYTE", kept);
    od_imgstore_unmount();
}

int main(void)
{
    test_nor();
    test_store_basics();
    test_power_cut();
    test_4byte_alias();
    printf("imgstore_nor (write-once header %d): %u known bugs\n", OD_IMGSTORE_WRITE_ONCE_HEADER,
           fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("imgstore_nor", 100);
}
