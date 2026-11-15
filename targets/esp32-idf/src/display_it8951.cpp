/* See display_it8951.h. The IT8951 runs its own waveforms, so this only moves commands and pixels.
 *
 * 16-gray frames stream straight to the controller, row by row, so the panel needs no framebuffer.
 * 1-bit mode keeps one in PSRAM because a partial update is composed into the previous frame.
 * Frames always load big-endian. On a mirrored panel (ED103TC2 on the E1003) each row is reversed
 * on the way out. On an unmirrored one (ED047TC1 on the M5Paper) gray rows go as-is and every
 * 1-bit byte is bit-reversed, because the IT8951 takes the leftmost pixel from bit 0.
 */
#include "display_it8951.h"

#if defined(OPENDISPLAY_FASTEPD)

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "od_log.h"

#define CMD_SYS_RUN      0x0001u
#define CMD_STANDBY      0x0002u
#define CMD_SLEEP        0x0003u
#define CMD_REG_RD       0x0010u
#define CMD_REG_WR       0x0011u
#define CMD_LD_IMG_AREA  0x0021u
#define CMD_LD_IMG_END   0x0022u
#define CMD_DPY_AREA     0x0034u
#define CMD_VCOM         0x0039u
#define CMD_TEMP         0x0040u
#define CMD_GET_DEV_INFO 0x0302u

#define REG_I80CPCR 0x0004u
#define REG_LISAR   0x0208u
#define REG_UP1SR   0x1138u
#define REG_LUTAFSR 0x1224u
#define REG_BGVR    0x1250u

#define PRE_CMD   0x6000u
#define PRE_WRITE 0x0000u
#define PRE_READ  0x1000u

#define FMT_4BPP 2u
#define FMT_8BPP 3u
#define BIG_ENDIAN_LOAD 1u

#define MODE_DU   1u
#define MODE_GC16 2u
#define MODE_A2   6u

#define SPI_PROBE_HZ 1000000
#define SPI_RUN_HZ   20000000
#define VCOM_MV      1500u
/* Used when no sensor reading is available. 22 C measured best of 14/22/30 on an ED103TC2. */
#define TEMP_DEFAULT_C 22
#define TEMP_MIN_C     0
#define TEMP_MAX_C     50
#define HRDY_TIMEOUT_US 3000000
#define LUT_TIMEOUT_US  30000000

static int8_t s_mosi, s_miso, s_sclk, s_cs, s_busy, s_rst, s_en, s_ite_en;
static uint16_t s_w, s_h;
static bool s_gray16;
static bool s_mirrored = true;
static uint16_t s_vcom_mv = VCOM_MV;   /* 0 = keep the controller's stored VCOM */
static uint16_t s_pitch;

static spi_device_handle_t s_dev;
static bool s_bus_up;
static bool s_failed;
static bool s_hw_ready;
static bool s_running;
static uint32_t s_img_buf;
static int s_temp_c = TEMP_DEFAULT_C;

static uint8_t *s_row;          /* DMA-capable, one panel row */
static uint8_t *s_fb;           /* 1-bit only, in PSRAM */
static uint32_t s_row_fill;
static uint32_t s_rows_sent;
static bool s_streaming;
static uint32_t s_fb_off;

static uint16_t s_px, s_py, s_pw, s_ph;
static uint32_t s_plane, s_pwritten, s_pexpected;

static void cs(int level)
{
    gpio_set_level((gpio_num_t)s_cs, level);
}

static void wait_hrdy(void)
{
    const int64_t start = esp_timer_get_time();

    while (gpio_get_level((gpio_num_t)s_busy) == 0) {
        if (esp_timer_get_time() - start > HRDY_TIMEOUT_US) {
            od_log_warn("it8951: HRDY timeout");
            return;
        }
        vTaskDelay(1);
    }
}

static uint16_t xfer16(uint16_t v)
{
    spi_transaction_t t = {};

    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 16;
    t.rxlength = 16;
    t.tx_data[0] = (uint8_t)(v >> 8);
    t.tx_data[1] = (uint8_t)v;
    (void)spi_device_polling_transmit(s_dev, &t);
    return (uint16_t)((t.rx_data[0] << 8) | t.rx_data[1]);
}

static void write_cmd(uint16_t cmd)
{
    cs(0);
    wait_hrdy();
    (void)xfer16(PRE_CMD);
    wait_hrdy();
    (void)xfer16(cmd);
    cs(1);
}

static void write_data(uint16_t v)
{
    cs(0);
    wait_hrdy();
    (void)xfer16(PRE_WRITE);
    wait_hrdy();
    (void)xfer16(v);
    cs(1);
}

static uint16_t read_data(void)
{
    uint16_t v;

    cs(0);
    wait_hrdy();
    (void)xfer16(PRE_READ);
    (void)xfer16(0u);
    wait_hrdy();
    v = xfer16(0u);
    cs(1);
    return v;
}

static void read_words(uint16_t *buf, size_t n)
{
    cs(0);
    wait_hrdy();
    (void)xfer16(PRE_READ);
    wait_hrdy();
    (void)xfer16(0u);
    wait_hrdy();
    for (size_t i = 0u; i < n; i++) {
        buf[i] = xfer16(0u);
    }
    cs(1);
}

static void write_reg(uint16_t addr, uint16_t v)
{
    write_cmd(CMD_REG_WR);
    write_data(addr);
    write_data(v);
}

static uint16_t read_reg(uint16_t addr)
{
    write_cmd(CMD_REG_RD);
    write_data(addr);
    return read_data();
}

static void wait_lut(void)
{
    const int64_t start = esp_timer_get_time();

    while (read_reg(REG_LUTAFSR) != 0u) {
        if (esp_timer_get_time() - start > LUT_TIMEOUT_US) {
            od_log_warn("it8951: LUT busy timeout");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool set_clock(int hz)
{
    spi_device_interface_config_t dev = {};

    if (s_dev != NULL) {
        (void)spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    dev.clock_speed_hz = hz;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    return spi_bus_add_device(SPI2_HOST, &dev, &s_dev) == ESP_OK;
}

static bool bus_up(void)
{
    spi_bus_config_t bus = {};
    esp_err_t err;

    if (s_bus_up) {
        return true;
    }
    bus.mosi_io_num = s_mosi;
    bus.miso_io_num = s_miso;
    bus.sclk_io_num = s_sclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    s_bus_up = true;
    return true;
}

/* A negative pin is not wired (a 0xB3 controller description may leave EN/ITE_EN out). */
static void pin_level(int8_t pin, int level)
{
    if (pin >= 0) {
        gpio_set_level((gpio_num_t)pin, level);
    }
}

static void pin_out(int8_t pin, int level)
{
    if (pin < 0) {
        return;
    }
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, level);
}

static void set_running(bool on)
{
    if (on != s_running) {
        write_cmd(on ? CMD_SYS_RUN : CMD_STANDBY);
        s_running = on;
    }
}

static bool init_controller(void)
{
    uint16_t info[20] = {};

    pin_out(s_cs, 1);
    pin_out(s_rst, 1);
    pin_out(s_en, 0);
    pin_out(s_ite_en, 0);
    gpio_reset_pin((gpio_num_t)s_busy);
    gpio_set_direction((gpio_num_t)s_busy, GPIO_MODE_INPUT);
    if (!bus_up() || !set_clock(SPI_PROBE_HZ)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    pin_level(s_en, 1);
    pin_level(s_ite_en, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)s_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)s_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    wait_hrdy();

    write_cmd(CMD_SYS_RUN);
    s_running = true;
    vTaskDelay(1);
    write_cmd(CMD_GET_DEV_INFO);
    read_words(info, 20u);
    if (info[0] == 0u || info[0] >= 10000u || info[1] == 0u || info[1] >= 10000u) {
        od_log_error("it8951: no device info (%u x %u)", info[0], info[1]);
        return false;
    }
    s_img_buf = ((uint32_t)info[3] << 16) | info[2];

    if (s_vcom_mv != 0u) {
        write_cmd(CMD_VCOM);
        write_data(0x0001u);
        write_data(s_vcom_mv);
    }
    write_reg(REG_I80CPCR, 0x0001u);
    return set_clock(SPI_RUN_HZ);
}

static bool ensure_buffers(void)
{
    if (s_row == NULL) {
        s_row = (uint8_t *)heap_caps_malloc(s_pitch, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (!s_gray16 && s_fb == NULL) {
        s_fb = (uint8_t *)heap_caps_malloc((size_t)s_pitch * s_h, MALLOC_CAP_SPIRAM);
    }
    return s_row != NULL && (s_gray16 || s_fb != NULL);
}

static bool hw_ready(void)
{
    if (!s_hw_ready) {
        s_failed = !init_controller() || !ensure_buffers();
        s_hw_ready = !s_failed;
    }
    return s_hw_ready;
}

/* Raw pixel words; the caller holds CS low across the whole image. */
static void send_row_bytes(const uint8_t *p, size_t n)
{
    spi_transaction_t t = {};

    t.length = n * 8u;
    t.tx_buffer = p;
    (void)spi_device_polling_transmit(s_dev, &t);
}

static void load_begin(uint16_t fmt, uint16_t w, uint16_t h)
{
    set_running(true);
    wait_lut();
    write_reg(REG_LISAR + 2u, (uint16_t)(s_img_buf >> 16));
    write_reg(REG_LISAR, (uint16_t)s_img_buf);
    if (fmt == FMT_4BPP) {
        write_reg(REG_UP1SR + 2u, (uint16_t)(read_reg(REG_UP1SR + 2u) & ~(1u << 2)));
    }
    write_cmd(CMD_LD_IMG_AREA);
    write_data((uint16_t)((BIG_ENDIAN_LOAD << 8) | (fmt << 4)));
    write_data(0u);
    write_data(0u);
    write_data(w);
    write_data(h);
    cs(0);
    wait_hrdy();
    (void)xfer16(PRE_WRITE);
    wait_hrdy();
}

static void load_end(void)
{
    cs(1);
    write_cmd(CMD_LD_IMG_END);
}

static void display_area(uint16_t mode)
{
    write_cmd(CMD_DPY_AREA);
    write_data(0u);
    write_data(0u);
    write_data(s_w);
    write_data(s_h);
    write_data(mode);
}

/* Waveforms are chosen by temperature, so this is sent before every refresh. */
static void apply_temperature(void)
{
    write_cmd(CMD_TEMP);
    write_data(0x0001u);
    write_data((uint16_t)s_temp_c);
}

static void flush_gray_row(void)
{
    for (uint32_t a = 0u, b = s_pitch - 1u; s_mirrored && a <= b && b < s_pitch; a++, b--) {
        const uint8_t x = (uint8_t)((s_row[a] >> 4) | (s_row[a] << 4));

        s_row[a] = (uint8_t)((s_row[b] >> 4) | (s_row[b] << 4));
        s_row[b] = x;
        if (a == b) {
            break;
        }
    }
    send_row_bytes(s_row, s_pitch);
    s_rows_sent++;
    s_row_fill = 0u;
    if ((s_rows_sent & 7u) == 0u) {
        taskYIELD();
    }
}

static void stream_close(void)
{
    if (s_streaming) {
        memset(s_row, 0xFF, s_pitch);
        while (s_rows_sent < s_h) {
            memset(s_row, 0xFF, s_pitch);
            send_row_bytes(s_row, s_pitch);
            s_rows_sent++;
        }
        load_end();
        s_streaming = false;
    }
    s_row_fill = 0u;
}

static void stream_open(void)
{
    stream_close();
    load_begin(FMT_4BPP, s_w, s_h);
    s_rows_sent = 0u;
    s_row_fill = 0u;
    s_streaming = true;
}

static void gray_append(const uint8_t *data, uint32_t len)
{
    while (len > 0u && s_rows_sent < s_h) {
        uint32_t n = s_pitch - s_row_fill;

        if (n > len) {
            n = len;
        }
        memcpy(s_row + s_row_fill, data, n);
        s_row_fill += n;
        data += n;
        len -= n;
        if (s_row_fill == s_pitch) {
            flush_gray_row();
        }
    }
}

static void refresh_gray(void)
{
    if (!s_streaming) {
        return;
    }
    if (s_row_fill > 0u) {
        memset(s_row + s_row_fill, 0xFF, s_pitch - s_row_fill);
        flush_gray_row();
    }
    stream_close();
    apply_temperature();
    display_area(MODE_GC16);
    wait_hrdy();
    wait_lut();
}

/* The whole frame, once: the image words carry 8 bits per unit in 1-bit mode. */
static void refresh_mono(uint16_t mode)
{
    load_begin(FMT_8BPP, (uint16_t)(s_w / 8u), s_h);
    for (uint32_t y = 0u; y < s_h; y++) {
        const uint8_t *src = s_fb + y * s_pitch;

        for (uint32_t i = 0u; i < s_pitch; i++) {
            if (s_mirrored) {
                s_row[s_pitch - 1u - i] = src[i];
            } else {
                uint8_t b = src[i];
                b = (uint8_t)((b >> 4) | (b << 4));
                b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
                s_row[i] = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
            }
        }
        send_row_bytes(s_row, s_pitch);
        if ((y & 7u) == 0u) {
            taskYIELD();
        }
    }
    load_end();
    write_reg(REG_UP1SR + 2u, (uint16_t)(read_reg(REG_UP1SR + 2u) | (1u << 2)));
    write_reg(REG_BGVR, 0x00FFu);
    apply_temperature();
    display_area(mode);
    wait_hrdy();
    wait_lut();
}

extern "C" void od_it8951_set_panel(int8_t mosi, int8_t miso, int8_t sclk, int8_t cs_pin,
                                    int8_t busy, int8_t rst, int8_t en, int8_t ite_en,
                                    uint16_t width, uint16_t height, bool gray16)
{
    s_mosi = mosi;
    s_miso = miso;
    s_sclk = sclk;
    s_cs = cs_pin;
    s_busy = busy;
    s_rst = rst;
    s_en = en;
    s_ite_en = ite_en;
    s_w = width;
    s_h = height;
    s_gray16 = gray16;
    s_pitch = gray16 ? (uint16_t)((width + 1u) / 2u) : (uint16_t)((width + 7u) / 8u);
}

extern "C" void od_it8951_set_layout(bool mirrored, uint16_t vcom_mv)
{
    s_mirrored = mirrored;
    s_vcom_mv = vcom_mv;
}

extern "C" void od_it8951_set_temperature(bool valid, int temp_c)
{
    int t = valid ? temp_c : TEMP_DEFAULT_C;

    if (t < TEMP_MIN_C) {
        t = TEMP_MIN_C;
    } else if (t > TEMP_MAX_C) {
        t = TEMP_MAX_C;
    }
    s_temp_c = t;
    od_log_info("it8951: refresh at %d C (%s)", t, valid ? "sensor" : "default");
}

extern "C" bool od_it8951_init_failed(void) { return s_failed; }
extern "C" void od_it8951_prepare_hardware(void) {}

extern "C" void od_it8951_epaper_begin(void)
{
    s_hw_ready = false;
    (void)hw_ready();
}

extern "C" void od_it8951_full_update(void)
{
    if (!s_hw_ready) {
        return;
    }
    if (s_gray16) {
        refresh_gray();
    } else {
        refresh_mono(MODE_GC16);
    }
}

extern "C" bool od_it8951_wait_refresh(int timeout_sec)
{
    (void)timeout_sec;
    return !s_failed;
}

static void power_down(void)
{
    if (!s_hw_ready) {
        return;
    }
    stream_close();
    set_running(false);
    write_cmd(CMD_SLEEP);
    pin_level(s_en, 0);
    pin_level(s_ite_en, 0);
    s_hw_ready = false;
}

extern "C" void od_it8951_sleep_after_refresh(void) { power_down(); }
extern "C" void od_it8951_direct_sleep(void) { power_down(); }

extern "C" void od_it8951_boot_write_row(uint16_t y, const uint8_t *row, unsigned pitch)
{
    if (!s_hw_ready || row == NULL || pitch < s_pitch) {
        return;
    }
    if (!s_gray16) {
        memcpy(s_fb + (size_t)y * s_pitch, row, s_pitch);
        return;
    }
    if (!s_streaming) {
        stream_open();
    }
    gray_append(row, s_pitch);
}

extern "C" void od_it8951_boot_skip_planes(void) {}

extern "C" void od_it8951_direct_write_reset(void)
{
    if (!hw_ready()) {
        return;
    }
    if (s_gray16) {
        stream_open();
    } else {
        memset(s_fb, 0xFF, (size_t)s_pitch * s_h);
        s_fb_off = 0u;
    }
}

extern "C" void od_it8951_direct_write_chunk(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0u || !s_hw_ready) {
        return;
    }
    if (s_gray16) {
        if (s_streaming) {
            gray_append(data, len);
        }
        return;
    }
    const uint32_t size = (uint32_t)s_pitch * s_h;
    const uint32_t n = s_fb_off >= size ? 0u : (len > size - s_fb_off ? size - s_fb_off : len);

    memcpy(s_fb + s_fb_off, data, n);
    s_fb_off += n;
}

extern "C" void od_it8951_direct_refresh(int refresh_mode)
{
    if (!s_hw_ready) {
        return;
    }
    if (s_gray16) {
        refresh_gray();
    } else {
        refresh_mono(refresh_mode == 1 ? MODE_DU : MODE_GC16);
    }
}

extern "C" void od_it8951_mark_hw_deinitialized(void)
{
    s_hw_ready = false;
    s_streaming = false;
    s_running = false;
}

extern "C" void od_it8951_partial_prepare(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!hw_ready()) {
        return;
    }
    s_px = x;
    s_py = y;
    s_pw = w;
    s_ph = h;
    s_plane = ((uint32_t)(w + 7u) / 8u) * h;
    s_pexpected = s_plane * 2u;
    s_pwritten = 0u;
}

extern "C" bool od_it8951_partial_write_chunk(const uint8_t *data, uint32_t len)
{
    const uint32_t rect_pitch = (s_pw + 7u) / 8u;

    if (data == NULL || len == 0u) {
        return true;
    }
    if (s_gray16 || s_fb == NULL || len > s_pexpected - s_pwritten || rect_pitch == 0u) {
        return false;
    }
    /* The first plane is the old image, which the framebuffer already holds. */
    while (len > 0u) {
        if (s_pwritten < s_plane) {
            const uint32_t skip = s_plane - s_pwritten < len ? s_plane - s_pwritten : len;

            s_pwritten += skip;
            data += skip;
            len -= skip;
            continue;
        }
        const uint32_t off = s_pwritten - s_plane;
        const uint32_t row = off / rect_pitch;
        const uint32_t col = off % rect_pitch;
        const uint32_t n = rect_pitch - col < len ? rect_pitch - col : len;

        memcpy(s_fb + (size_t)(s_py + row) * s_pitch + (s_px / 8u) + col, data, n);
        s_pwritten += n;
        data += n;
        len -= n;
    }
    return true;
}

extern "C" bool od_it8951_partial_refresh(int refresh_mode)
{
    if (s_pwritten != s_pexpected || s_fb == NULL || !s_hw_ready) {
        return false;
    }
    refresh_mono(refresh_mode == 0 ? MODE_GC16 : MODE_DU);
    return true;
}

extern "C" void od_it8951_release_bus(void)
{
    if (s_dev != NULL) {
        (void)spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus_up) {
        (void)spi_bus_free(SPI2_HOST);
        s_bus_up = false;
    }
}

#endif /* OPENDISPLAY_FASTEPD */
