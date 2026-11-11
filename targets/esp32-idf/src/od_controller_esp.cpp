/* See od_controller_esp.h. */
#include "od_controller_esp.h"

#include <string.h>

#include "od_cfg_index.h"
#include "od_log.h"
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

void od_controller_esp_describe(const struct DisplayConfig *d, struct od_controller_desc *out)
{
    memset(out, 0, sizeof(*out));
    out->cls = OD_CTRL_CLASS_COMMAND;
    out->default_temp_c = OD_CTRL_TEMP_DEFAULT;
    out->miso_pin = out->en_pin = out->ite_en_pin = OD_CTRL_PIN_NONE;
    if (d == nullptr) {
        return;
    }
    if (od_cfg_index_build() >= 0) {
        (void)from_packet(d->instance_number, out);
    }
}

void od_controller_esp_reload(void)
{
    od_controller_esp_describe(globalConfig.display_count > 0u ? &globalConfig.displays[0] : nullptr,
                               &s_desc);
    if (s_desc.cls != OD_CTRL_CLASS_COMMAND) {
        od_log_info("display 0: built-in %s driver (0xB3)",
                    s_desc.cls == OD_CTRL_CLASS_IT8951 ? "IT8951" : "parallel");
    }
}

const struct od_controller_desc *od_controller_esp(void)
{
    return &s_desc;
}
