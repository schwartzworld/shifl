/* SPDX-License-Identifier: GPL-3.0-only */
/* Firmware adapter. Scene snapshots are already resident in RAM. No flash
 * operation, UI draw or allocation is allowed from these audio callbacks. */
#include "arranger.h"
static arr_config_t arrangement;
static arr_clock_t arrangement_clock;
static uint8_t arrangement_enabled;
static uint32_t arrangement_ready(void);               /* project.c */
static void arrangement_apply(uint32_t entry_index);   /* project.c: per-track per-entry switch */
static void arrangement_apply_scene(uint32_t scene);   /* project.c: all-tracks-same-scene switch */

static uint8_t arrangement_start_index;  /* set by UI before transport start to begin from a specific entry */

static int arrangement_start(void)
{
    int ei;
    uint8_t si;
    if (!arrangement_enabled) return 1;
    ei = arr_begin(&arrangement_clock, &arrangement, arrangement_ready());
    si = arrangement_start_index;
    arrangement_start_index = 0;
    if (ei < 0) return 0;
    if (si > 0u && si < arrangement.count) {
        arrangement_clock.index = si;
        arrangement_clock.bar = 0;
        ei = (int)si;
    }
    arrangement_apply((uint32_t)ei);
    punch.song = arrangement.entry[ei].rsv[0] == 0xFFu ? (int8_t)-1 : (int8_t)arrangement.entry[ei].rsv[0];
    song.rec = 0;                                    /* song playback does not overwrite patterns */
    return 1;
}
