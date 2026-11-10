/* See od_controller_esp.h. */
#include "od_controller_esp.h"

#include <string.h>

#include "od_cfg_index.h"
#include "od_log.h"
#include "protocol_pending.h"
#include "structs.h"

extern struct od_config globalConfig;

/* 0xB3 body, version 0. Longer bodies are accepted; the tail is reserved. */
#define CTRL_BODY_MIN 12u
#define CTRL_VERSION  0u

static struct od_controller_desc s_desc;

static bool from_packet(uint8_t instance, struct od_controller_desc *out)
{
    uint8_t b[CTRL_BODY_MIN];

    for (uint8_t nth = 0; nth < 4u; nth++) {
        const od_cfg_entry_t *e = od_cfg_find(OD_PKT_DISPLAY_CONTROLLER, nth);

        if (e == nullptr) {
            return false;
        }
        if (e->instance != instance) {
            continue;
        }
        if (e->len < CTRL_BODY_MIN || od_cfg_read(e, 0u, b, sizeof b) != sizeof b) {
            od_log_warn("display_controller: %u B body is short; ignored", (unsigned)e->len);
            return false;
        }
        if (b[1] != CTRL_VERSION || b[2] > OD_CTRL_CLASS_PARALLEL) {
            od_log_warn("display_controller: version %u class %u not understood; ignored",
                        (unsigned)b[1], (unsigned)b[2]);
            return false;
        }
        out->cls = b[2];
        out->flags = b[3];
        out->vcom_mv = (uint16_t)(b[4] | (b[5] << 8));
        out->default_temp_c = (int8_t)b[6];
        out->parallel_panel = b[7];
        out->miso_pin = b[8];
        out->en_pin = b[9];
        out->ite_en_pin = b[10];
        out->from_packet = true;
        return true;
    }
    return false;
}

static uint8_t aux_pin(uint8_t p, uint8_t fallback)
{
    return (p == 0u || p == 0xFFu) ? fallback : p;
}

/* The panels that shipped before 0xB3, described as the packet would describe them. */
static void from_panel_id(const struct DisplayConfig *d, const struct SystemConfig *sys,
                          struct od_controller_desc *out)
{
    if (d->display_technology != 0u && d->display_technology != 1u) {
        return;
    }
    switch (d->panel_ic_type) {
    case OD_PANEL_IC_ED103TC2_1872X1404_4GRAY:
        out->flags |= OD_CTRL_FLAG_GRAY16;
        /* fall through */
    case OD_PANEL_IC_ED103TC2_1872X1404:
        /* Seeed reTerminal E1003: mirrored, 1500 mV, MISO on dc_pin, EN/ITE_EN on pwr_pin_2/3. */
        out->cls = OD_CTRL_CLASS_IT8951;
        out->flags |= OD_CTRL_FLAG_MIRRORED | OD_CTRL_FLAG_TEMP_SENSOR;
        out->vcom_mv = 1500u;
        out->miso_pin = d->dc_pin != 0xFFu ? d->dc_pin : 8u;
        out->en_pin = sys != nullptr ? aux_pin(sys->pwr_pin_2, 11u) : 11u;
        out->ite_en_pin = sys != nullptr ? aux_pin(sys->pwr_pin_3, 21u) : 21u;
        break;
    case OD_PANEL_IC_M5PAPER_960X540:
        /* M5Paper ED047TC1: unmirrored, keeps the stored VCOM. The IT8951 reset is not wired;
         * GPIO23 gates its supply, so reset_pin doubles as EN and ITE_EN. */
        out->cls = OD_CTRL_CLASS_IT8951;
        out->flags |= OD_CTRL_FLAG_TEMP_SENSOR;
        out->vcom_mv = 0u;
        out->miso_pin = d->dc_pin != 0xFFu ? d->dc_pin : 13u;
        out->en_pin = d->reset_pin != 0xFFu ? d->reset_pin : 23u;
        out->ite_en_pin = out->en_pin;
        break;
    case OD_PANEL_IC_INKPLATE5V2_1280X720:
        out->cls = OD_CTRL_CLASS_PARALLEL;
        out->parallel_panel = OD_CTRL_PARALLEL_INKPLATE5V2;
        break;
    case OD_PANEL_IC_INKPLATE10_1200X825:
        out->cls = OD_CTRL_CLASS_PARALLEL;
        out->parallel_panel = OD_CTRL_PARALLEL_INKPLATE10;
        break;
    case OD_PANEL_IC_M5PAPERS3_960X540:
        out->cls = OD_CTRL_CLASS_PARALLEL;
        out->parallel_panel = OD_CTRL_PARALLEL_M5PAPERS3;
        break;
    default:
        break;
    }
}

void od_controller_esp_describe(const struct DisplayConfig *d, const struct SystemConfig *sys,
                                struct od_controller_desc *out)
{
    memset(out, 0, sizeof(*out));
    out->cls = OD_CTRL_CLASS_COMMAND;
    out->default_temp_c = OD_CTRL_TEMP_DEFAULT;
    out->miso_pin = out->en_pin = out->ite_en_pin = OD_CTRL_PIN_NONE;
    if (d == nullptr) {
        return;
    }
    if (od_cfg_index_build() >= 0 && from_packet(d->instance_number, out)) {
        return;
    }
    from_panel_id(d, sys, out);
}

void od_controller_esp_reload(void)
{
    od_controller_esp_describe(globalConfig.display_count > 0u ? &globalConfig.displays[0] : nullptr,
                               &globalConfig.system_config, &s_desc);
    if (s_desc.cls != OD_CTRL_CLASS_COMMAND) {
        od_log_info("display 0: built-in %s driver (%s)",
                    s_desc.cls == OD_CTRL_CLASS_IT8951 ? "IT8951" : "parallel",
                    s_desc.from_packet ? "0xB3" : "panel_ic_type");
    }
}

const struct od_controller_desc *od_controller_esp(void)
{
    return &s_desc;
}
