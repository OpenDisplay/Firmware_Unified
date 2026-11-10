/* Writes the committed seed corpus for fuzz_config_drive: every representative panel config
 * (tests/vectors/drive_panels.inc) as [ctl][temp][payload], under a few ctl/temp choices, plus a
 * 2.x config without drive packets and one carrying an action table. Built with the fuzz
 * targets, not registered as a test; run by hand when the panel set changes:
 *   build/fuzz/od_fuzz_seed_config_drive tests/fuzz/corpus/config_drive
 */
#include "od_check.h"
#include "drive_fixture.h"

static void emit(const char *dir, const char *name, uint8_t ctl, uint8_t temp, const fx_cfg_t *c)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/seed_%s_%02x_%02x", dir, name, ctl, temp);
    f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    fputc(ctl, f);
    fputc(temp, f);
    fwrite(c->b, 1u, c->n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    static const uint8_t ctls[] = { 0x00u, 0x31u, 0xC5u };
    const uint8_t table[22] = { 10u, 5u, 0, 0, 1u, 1u, 1u, 1u, 16u, 0u,
                                12u, 6u, 0x14u, 0x1Eu, 1u, 1u, 3u, 0u, 0u, 1u, 0u, 0u };
    fx_cfg_t c;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return 2;
    }
    for (unsigned p = 0; p < FX_PANEL_COUNT; p++) {
        struct DisplayConfig d = fx_display(0u, 16u, 6u, FX_PANELS[p].scheme);
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        fx_cfg_panel(&c, &FX_PANELS[p], 0u);
        fx_cfg_finish(&c);
        if (c.n > 4096u) {
            continue;
        }
        for (unsigned k = 0; k < sizeof ctls; k++) {
            emit(argv[1], FX_PANELS[p].name, ctls[k], k == 1u ? 0x7Fu : 25u, &c);
        }
    }
    {
        struct DisplayConfig d = fx_display(0u, 16u, 6u, 0u);
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        fx_cfg_finish(&c);
        emit(argv[1], "legacy_2x", 0x80u, 20u, &c);
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, table, sizeof table);
        fx_cfg_finish(&c);
        emit(argv[1], "actions", 0x00u, 25u, &c);
    }
    return 0;
}
