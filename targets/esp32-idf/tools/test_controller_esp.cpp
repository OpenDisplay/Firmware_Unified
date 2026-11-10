// Host test for src/od_controller_esp.cpp: the 0xB3 display_controller packet and the legacy
// panel_ic_type table that describe which built-in driver drives a display. The stored config
// is a fake od_hal_nvs holding a real record; od_cfg_index is the shared one, compiled with
// OD_CFG_INDEX_CONTROLLER=1 as on the target.
#include "od_controller_esp.h"

#include <cstdio>
#include <cstring>

#include "od_cfg_index.h"
#include "od_config_store.h"
#include "od_hal_nvs.h"
#include "protocol_pending.h"
#include "structs.h"

struct od_config globalConfig;

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);   \
        }                                                                              \
    } while (0)

extern "C" void _od_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

static uint8_t g_store[OD_CONFIG_STORE_HEADER_SIZE + 1024u];
static uint32_t g_store_len;
static uint32_t g_payload_len;

static void store_reset()
{
    std::memset(g_store, 0, sizeof g_store);
    g_payload_len = 3u;
    g_store[OD_CONFIG_STORE_HEADER_SIZE + 2u] = 1u;
    g_store_len = 0u;
}

static void store_packet(uint8_t id, const uint8_t *body, uint16_t len)
{
    uint8_t *p = &g_store[OD_CONFIG_STORE_HEADER_SIZE + g_payload_len];

    p[0] = 0u;
    p[1] = id;
    p[2] = (uint8_t)len;
    p[3] = (uint8_t)(len >> 8);
    std::memcpy(p + 4, body, len);
    g_payload_len += 4u + len;
}

/* The record is framed by the shared od_config_store, which writes it back through the fake
 * od_hal_nvs below. The two trailing payload bytes stand in for the config CRC. */
static void store_commit()
{
    static uint8_t work[sizeof g_store];
    const uint32_t data_len = g_payload_len + 2u;

    std::memcpy(od_config_store_payload(work), &g_store[OD_CONFIG_STORE_HEADER_SIZE], data_len);
    CHECK(od_config_store_save(work, sizeof work, od_config_store_payload(work), data_len) ==
          OD_CONFIG_STORE_OK);
}

extern "C" int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }
extern "C" int od_hal_nvs_size(uint32_t *len_out)
{
    if (g_store_len == 0u) return OD_HAL_NVS_ENOENT;
    *len_out = g_store_len;
    return OD_HAL_NVS_OK;
}
extern "C" int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    if (g_store_len == 0u) return OD_HAL_NVS_ENOENT;
    if (offset > g_store_len || len > g_store_len - offset) return OD_HAL_NVS_E2BIG;
    std::memcpy(buf, &g_store[offset], len);
    return OD_HAL_NVS_OK;
}
extern "C" int od_hal_nvs_write(const void *record, uint32_t len)
{
    if (len > sizeof g_store) return OD_HAL_NVS_E2BIG;
    std::memcpy(g_store, record, len);
    g_store_len = len;
    return OD_HAL_NVS_OK;
}
extern "C" int od_hal_nvs_erase(void) { g_store_len = 0u; return OD_HAL_NVS_OK; }

static void display(uint16_t ic, uint8_t instance)
{
    std::memset(&globalConfig, 0, sizeof globalConfig);
    globalConfig.loaded = true;
    globalConfig.display_count = 1u;
    DisplayConfig &d = globalConfig.displays[0];
    d.instance_number = instance;
    d.panel_ic_type = ic;
    d.dc_pin = 0xFFu;
    d.reset_pin = 0xFFu;
    globalConfig.system_config.pwr_pin_2 = 0xFFu;
    globalConfig.system_config.pwr_pin_3 = 0xFFu;
}

static void controller(uint8_t instance, uint8_t cls, uint8_t flags, uint16_t vcom,
                       int8_t temp, uint8_t parallel, uint8_t miso, uint8_t en, uint8_t ite_en,
                       uint16_t len = 12u, uint8_t version = 0u)
{
    uint8_t b[16] = {instance, version, cls, flags, (uint8_t)vcom, (uint8_t)(vcom >> 8),
                     (uint8_t)temp, parallel, miso, en, ite_en, 0u, 0u, 0u, 0u, 0u};
    store_packet(OD_PKT_DISPLAY_CONTROLLER, b, len);
}

int main()
{
    const od_controller_desc *c;

    g_case = "no config: command-driven";
    store_reset();
    std::memset(&globalConfig, 0, sizeof globalConfig);
    od_controller_esp_reload();
    CHECK(od_controller_esp()->cls == OD_CTRL_CLASS_COMMAND);

    g_case = "0xB3 IT8951 overrides an SPI panel id";
    display(0x0002u, 0u);
    store_reset();
    controller(0u, OD_CTRL_CLASS_IT8951, OD_CTRL_FLAG_MIRRORED | OD_CTRL_FLAG_GRAY16, 1750u, 22,
               0u, 8u, 11u, 0xFFu);
    store_commit();
    od_controller_esp_reload();
    c = od_controller_esp();
    CHECK(c->from_packet && c->cls == OD_CTRL_CLASS_IT8951);
    CHECK(c->flags == (OD_CTRL_FLAG_MIRRORED | OD_CTRL_FLAG_GRAY16) && c->vcom_mv == 1750u);
    CHECK(c->default_temp_c == 22 && c->miso_pin == 8u && c->en_pin == 11u);
    CHECK(c->ite_en_pin == OD_CTRL_PIN_NONE);

    g_case = "0xB3 for another instance does not apply";
    display(0x0002u, 1u);
    od_controller_esp_reload();
    CHECK(!od_controller_esp()->from_packet && od_controller_esp()->cls == OD_CTRL_CLASS_COMMAND);

    g_case = "0xB3 class 0 makes a legacy IT8951 id command-driven";
    display(OD_PANEL_IC_ED103TC2_1872X1404, 0u);
    store_reset();
    controller(0u, OD_CTRL_CLASS_COMMAND, 0u, 0u, 0x7F, 0u, 0xFFu, 0xFFu, 0xFFu);
    store_commit();
    od_controller_esp_reload();
    CHECK(od_controller_esp()->from_packet && od_controller_esp()->cls == OD_CTRL_CLASS_COMMAND);

    g_case = "0xB3 parallel PaperS3, longer body accepted";
    display(0x0002u, 0u);
    store_reset();
    controller(0u, OD_CTRL_CLASS_PARALLEL, 0u, 0u, 0x7F, OD_CTRL_PARALLEL_M5PAPERS3, 0xFFu,
               0xFFu, 0xFFu, 16u);
    store_commit();
    od_controller_esp_reload();
    c = od_controller_esp();
    CHECK(c->from_packet && c->cls == OD_CTRL_CLASS_PARALLEL &&
          c->parallel_panel == OD_CTRL_PARALLEL_M5PAPERS3);

    g_case = "short, future-version or unknown-class 0xB3 is ignored (legacy table applies)";
    for (int k = 0; k < 3; k++) {
        display(OD_PANEL_IC_INKPLATE10_1200X825, 0u);
        store_reset();
        controller(0u, k == 2 ? 7u : OD_CTRL_CLASS_IT8951, 0u, 0u, 0x7F, 0u, 0xFFu, 0xFFu, 0xFFu,
                   k == 0 ? 11u : 12u, k == 1 ? 1u : 0u);
        store_commit();
        od_controller_esp_reload();
        c = od_controller_esp();
        CHECK(!c->from_packet && c->cls == OD_CTRL_CLASS_PARALLEL &&
              c->parallel_panel == OD_CTRL_PARALLEL_INKPLATE10);
    }

    g_case = "legacy table: ED103TC2 4-gray (E1003 wiring defaults)";
    display(OD_PANEL_IC_ED103TC2_1872X1404_4GRAY, 0u);
    store_reset();
    od_controller_esp_reload();
    c = od_controller_esp();
    CHECK(c->cls == OD_CTRL_CLASS_IT8951 && c->vcom_mv == 1500u);
    CHECK(c->flags == (OD_CTRL_FLAG_GRAY16 | OD_CTRL_FLAG_MIRRORED | OD_CTRL_FLAG_TEMP_SENSOR));
    CHECK(c->miso_pin == 8u && c->en_pin == 11u && c->ite_en_pin == 21u);

    g_case = "legacy table: ED103TC2 with pwr_pin_2/3 and dc_pin set";
    display(OD_PANEL_IC_ED103TC2_1872X1404, 0u);
    globalConfig.displays[0].dc_pin = 40u;
    globalConfig.system_config.pwr_pin_2 = 41u;
    globalConfig.system_config.pwr_pin_3 = 42u;
    od_controller_esp_reload();
    c = od_controller_esp();
    CHECK(c->flags == (OD_CTRL_FLAG_MIRRORED | OD_CTRL_FLAG_TEMP_SENSOR));
    CHECK(c->miso_pin == 40u && c->en_pin == 41u && c->ite_en_pin == 42u);

    g_case = "legacy table: M5Paper unmirrored, stored VCOM, reset doubles as EN";
    display(OD_PANEL_IC_M5PAPER_960X540, 0u);
    od_controller_esp_reload();
    c = od_controller_esp();
    CHECK(c->cls == OD_CTRL_CLASS_IT8951 && c->vcom_mv == 0u &&
          (c->flags & OD_CTRL_FLAG_MIRRORED) == 0u);
    CHECK(c->miso_pin == 13u && c->en_pin == 23u && c->ite_en_pin == 23u);

    g_case = "legacy table: display_technology other than 0/1 is not claimed";
    display(OD_PANEL_IC_INKPLATE5V2_1280X720, 0u);
    globalConfig.displays[0].display_technology = 2u;
    od_controller_esp_reload();
    CHECK(od_controller_esp()->cls == OD_CTRL_CLASS_COMMAND);

    std::printf("controller_esp: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
