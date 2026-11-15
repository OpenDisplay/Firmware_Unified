/* Controls for fake_fastepd.cpp. */
#pragma once

extern int fake_fastepd_init_panel_calls;   /* initPanel() calls since start */
extern int fake_fastepd_init_rc;            /* what initPanel() returns; BBEP_SUCCESS default */
extern int fake_fastepd_updates;             /* fullUpdate() + fastUpdate() calls */
