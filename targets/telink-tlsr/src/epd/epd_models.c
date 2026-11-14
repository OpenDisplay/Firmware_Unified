/* Model table and lookups, copied from Firmware_NRF52 EPD/EPD_driver.c (lines 264-364) at
 * 71b870c12855b2bab4db11fdf1fb213818f3f3a8. The rest of that file is the nRF GPIO/SPI layer, which
 * epd_io.c replaces. Only change: the Telink-target models epd_ssd16xx_hs_266_bwr, epd_ssd16xx_hs_200_bwy, epd_uc8151_hs_350_bwy and epd_ti_970_bwr are registered. */
#include "EPD_driver.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))


extern epd_model_t epd_uc8176_420_bw;
extern epd_model_t epd_uc8176_420_bwr;
extern epd_model_t epd_uc8159_750_bw;
extern epd_model_t epd_uc8159_750_bwr;
extern epd_model_t epd_uc8179_750_bw;
extern epd_model_t epd_uc8179_750_bwr;
extern epd_model_t epd_uc8151_029_bw;
extern epd_model_t epd_uc8151_029_bwr;
extern epd_model_t epd_ssd1619_420_bwr;
extern epd_model_t epd_ssd1619_420_bw;
extern epd_model_t epd_ssd1619_016_bw;
extern epd_model_t epd_ssd1619_016_bwr;
extern epd_model_t epd_ssd1619_022_bw;
extern epd_model_t epd_ssd1619_022_bwr;
extern epd_model_t epd_ssd1619_026_bw;
extern epd_model_t epd_ssd1619_026_bwr;
extern epd_model_t epd_ssd1619_029_bw;
extern epd_model_t epd_ssd1619_029_bwr;
extern epd_model_t epd_uc8151_027_bw;
extern epd_model_t epd_uc8151_027_bwr;
extern epd_model_t epd_ucvar43_430_bw;
extern epd_model_t epd_ucvar43_430_bwr;
extern epd_model_t epd_ssd1677_750_bwr;
extern epd_model_t epd_ssd1677_750_bw;
extern epd_model_t epd_ssd1619_013_bw;
extern epd_model_t epd_ssd1619_013_bwr;
extern epd_model_t epd_ssd1619_022_lite_bw;
extern epd_model_t epd_ssd1619_022_lite_bwr;
extern epd_model_t epd_jd79668_420_bwry;
extern epd_model_t epd_jd79665_750_bwry;
extern epd_model_t epd_jd79665_583_bwry;
extern epd_model_t epd_ssd16xx_hs_266_bwr;   /* Telink-target additions */
extern epd_model_t epd_ssd16xx_hs_200_bwy;
extern epd_model_t epd_uc8151_hs_350_bwy;
extern epd_model_t epd_ti_970_bwr;

static epd_model_t* epd_models[] = {
    &epd_uc8176_420_bw,    &epd_uc8176_420_bwr,   &epd_uc8159_750_bw,    &epd_uc8159_750_bwr,  &epd_uc8179_750_bw,
    &epd_uc8179_750_bwr,   &epd_uc8151_029_bw,    &epd_uc8151_029_bwr,   &epd_ssd1619_420_bwr,  &epd_ssd1619_420_bw,
    &epd_ssd1619_016_bw,   &epd_ssd1619_016_bwr,  &epd_ssd1619_022_bw,   &epd_ssd1619_022_bwr,  &epd_ssd1619_026_bw,
    &epd_ssd1619_026_bwr,  &epd_uc8151_027_bw,    &epd_uc8151_027_bwr,   &epd_ucvar43_430_bw,   &epd_ucvar43_430_bwr,
    &epd_ssd1619_029_bw,   &epd_ssd1619_029_bwr,
    &epd_ssd1619_013_bw,  &epd_ssd1619_013_bwr,
    &epd_ssd1619_022_lite_bw, &epd_ssd1619_022_lite_bwr,
    &epd_ssd1677_750_bwr, &epd_ssd1677_750_bw,
    &epd_jd79668_420_bwry, &epd_jd79665_750_bwry, &epd_jd79665_583_bwry,
    &epd_ssd16xx_hs_266_bwr, &epd_ssd16xx_hs_200_bwy, &epd_uc8151_hs_350_bwy, &epd_ti_970_bwr,
};

epd_model_t* epd_init(epd_model_id_t id) {
    epd_model_t* epd = NULL;
    for (uint8_t i = 0; i < ARRAY_SIZE(epd_models); i++) {
        if (epd_models[i]->id == id) {
            epd = epd_models[i];
        }
    }
    if (epd == NULL) epd = epd_models[0];
    epd->drv->init(epd);
    return epd;
}

epd_model_id_t map_panel_ic_to_model_id(uint16_t panel_ic_type, uint8_t color_scheme) {
    if (panel_ic_type >= 1000 && panel_ic_type <= 1999) {
        panel_ic_type -= 999;
    }
    epd_color_t target_color = (color_scheme == 0) ? COLOR_BW : COLOR_BWR;
    if (panel_ic_type >= 1 && panel_ic_type <= 31) {
        epd_model_id_t model_id = (epd_model_id_t)panel_ic_type;
        switch (model_id) {
            case SSD1619_420_BW:
            case SSD1619_420_BWR:
                return (target_color == COLOR_BW) ? SSD1619_420_BW : SSD1619_420_BWR;
            case SSD1619_016_BW:
            case SSD1619_016_BWR:
                return (target_color == COLOR_BW) ? SSD1619_016_BW : SSD1619_016_BWR;
            case SSD1619_022_BW:
            case SSD1619_022_BWR:
                return (target_color == COLOR_BW) ? SSD1619_022_BW : SSD1619_022_BWR;
            case SSD1619_026_BW:
            case SSD1619_026_BWR:
                return (target_color == COLOR_BW) ? SSD1619_026_BW : SSD1619_026_BWR;
            case UC8151_027_BW:
            case UC8151_027_BWR:
                return (target_color == COLOR_BW) ? UC8151_027_BW : UC8151_027_BWR;
            case UCVAR43_430_BW:
            case UCVAR43_430_BWR:
                return (target_color == COLOR_BW) ? UCVAR43_430_BW : UCVAR43_430_BWR;
            case SSD1619_029_BW:
            case SSD1619_029_BWR:
                return (target_color == COLOR_BW) ? SSD1619_029_BW : SSD1619_029_BWR;
            case UC8151_029_BW:
            case UC8151_029_BWR:
                return (target_color == COLOR_BW) ? UC8151_029_BW : UC8151_029_BWR;
            case SSD1619_013_BW:
            case SSD1619_013_BWR:
                return (target_color == COLOR_BW) ? SSD1619_013_BW : SSD1619_013_BWR;
            case SSD1619_022_LITE_BW:
            case SSD1619_022_LITE_BWR:
                return (target_color == COLOR_BW) ? SSD1619_022_LITE_BW : SSD1619_022_LITE_BWR;
            default:
                return model_id;
        }
    }
    return UC8176_420_BW;
}
