/* od_epd_shim.cpp -- see od_epd_shim.h. The ONLY translation unit that sees the adapter. */
#include "od_epd_shim.h"

#include "od_cfg_index.h"
#include "od_epd_adapter.h"
#include "od_log.h"
#include "opendisplay_structs.h"

static OdEpdAdapter s_epd;
static bool s_in_pass;

bool od_epd_shim_available(uint8_t display_instance)
{
    /* The panel packet is the one that cannot be defaulted: it carries BUSY trust and sense,
     * the colour code map and the sleep sequence. Modes and waveforms without it are not
     * drivable, so this is the honest test for "can od_epd drive this device". */
    const od_cfg_entry_t *e = od_cfg_find(OD_PKT_DISPLAY_PANEL, display_instance);
    return e != NULL && e->len > 0u;
}

bool od_epd_shim_begin(uint8_t display_instance, uint8_t mode_index,
                       uint8_t rotation_quarters,
                       uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                       uint8_t mosi, uint8_t sck)
{
    s_in_pass = false;

    if (s_epd.setPanelType(display_instance) != BBEP_SUCCESS) {
        od_log_warn("od_epd: no drive data for display %u", (unsigned)display_instance);
        return false;
    }
    s_epd.setRotation((int)rotation_quarters * 90);
    s_epd.initIO(dc, rst, busy, cs, mosi, sck, 0u);

    if (s_epd.selectModeIndex(mode_index) != BBEP_SUCCESS) {
        od_log_warn("od_epd: mode %u not in config (have %u)",
                    (unsigned)mode_index, (unsigned)s_epd.modeCount());
        return false;
    }
    if (s_epd.beginPass(0u) != BBEP_SUCCESS) {
        od_log_warn("od_epd: beginPass failed rc=%d", s_epd.lastRc());
        return false;
    }
    s_in_pass = true;
    return true;
}

bool od_epd_shim_write(const uint8_t *data, uint32_t len)
{
    if (!s_in_pass || data == NULL || len == 0u) {
        return false;
    }
    s_epd.writeData(data, (int)len);
    return true;
}

bool od_epd_shim_next_plane(void)
{
    return s_in_pass && s_epd.nextPlane() == BBEP_SUCCESS;
}

bool od_epd_shim_finish(void)
{
    if (!s_in_pass) {
        return false;
    }
    s_in_pass = false;
    return s_epd.finishPass() == BBEP_SUCCESS;
}

void od_epd_shim_sleep(void)
{
    s_in_pass = false;
    if (s_epd.ready()) {
        s_epd.sleep(DEEP_SLEEP);
    }
}
