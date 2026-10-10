/* SPDX-License-Identifier: GPL-3.0-only */
/* Song order, independent of UI/flash/audio drivers. Durations are 4/4 bars.
 * Audio calls next BEFORE rendering a block, then elapse AFTER it. The clock
 * accumulates samples * BPM, keeping the remainder at every bar boundary. */
#ifndef FM1_ARRANGER_H
#define FM1_ARRANGER_H
#include <stdint.h>
#define ARR_STEPS  16u
#define ARR_SCENES 6u
#define ARR_TRACKS 4u
#define ARR_MUTE   6u   /* track[k] == ARR_MUTE: this track is silent in the section */
#define ARR_NONE    (-1)
#define ARR_DONE    (-2)
#define ARR_INVALID (-3)
#define ARR_PATCH_NONE 0xFFu  /* engine/preset override: use whatever the scene set */
#define ARR_VOL_NONE   0xFFu  /* per-fragment volume override: use whatever the scene set (0xFF) */
#define ARR_FLAG_INKEY 0x01u  /* rsv[2] bit 0: chromatic transpose snapped to track's scale */
/* Each entry independently selects a scene (A-F = 0-5) or ARR_MUTE per track. */
typedef struct { uint8_t track[ARR_TRACKS]; uint8_t bars; uint8_t rsv[3]; } arr_entry_t;
/* Per-entry per-track patch override; 0xFF in either field = use the scene's engine/preset. */
typedef struct { uint8_t engine[ARR_TRACKS]; uint8_t preset[ARR_TRACKS]; } arr_patch_t;
typedef struct {
    uint8_t count, loop, reserved[2];
    arr_entry_t entry[ARR_STEPS];
    arr_patch_t patch[ARR_STEPS];   /* patch overrides, appended last for backward compat */
} arr_config_t;
typedef struct {
    uint32_t phase;
    uint8_t running, index, bar, error;
} arr_clock_t;

static void arr_defaults(arr_config_t *c)
{
    uint32_t i, k;
    c->count = 4; c->loop = 0; c->reserved[0] = c->reserved[1] = 0;
    for (i = 0; i < ARR_STEPS; i++) {
        for (k = 0; k < ARR_TRACKS; k++) {
            c->entry[i].track[k] = (uint8_t)(i % ARR_SCENES);
            c->patch[i].engine[k] = ARR_PATCH_NONE;
            c->patch[i].preset[k] = ARR_PATCH_NONE;
        }
        c->entry[i].bars = 8;
        c->entry[i].rsv[0] = 0xFFu;  /* 0xFF = no punch FX */
        c->entry[i].rsv[1] = c->entry[i].rsv[2] = 0;
    }
}
static int arr_valid(const arr_config_t *c, uint32_t ready)
{
    uint32_t i, k;
    if (!c->count || c->count > ARR_STEPS || c->loop > 1u) return 0;
    for (i = 0; i < c->count; i++) {
        const arr_entry_t *e = &c->entry[i];
        if (!e->bars || e->bars > 128u) return 0;
        for (k = 0; k < ARR_TRACKS; k++) {
            if (e->track[k] == ARR_MUTE) continue;
            if (e->track[k] >= ARR_SCENES || !(ready & (1u << e->track[k]))) return 0;
        }
    }
    return 1;
}
/* Returns the entry index (>= 0) when playback begins, ARR_INVALID if invalid. */
static int arr_begin(arr_clock_t *r, const arr_config_t *c, uint32_t ready)
{
    r->phase = 0; r->index = 0; r->bar = 0; r->running = 0;
    r->error = (uint8_t)!arr_valid(c, ready);
    if (r->error) return ARR_INVALID;
    r->running = 1;
    return 0;
}
/* Returns the new entry index (>= 0) when crossing a section boundary, else ARR_NONE/ARR_DONE. */
static int arr_next(arr_clock_t *r, const arr_config_t *c, uint32_t sample_rate)
{
    int result = ARR_NONE;
    uint32_t period = sample_rate * 120u;
    if (!r->running || !period) return ARR_NONE;
    while (r->phase >= period) {
        r->phase -= period;
        if (++r->bar < c->entry[r->index].bars) continue;
        r->bar = 0;
        if (++r->index >= c->count) {
            if (!c->loop) { r->running = 0; return ARR_DONE; }
            r->index = 0;
        }
        result = (int)r->index;
    }
    return result;
}
static void arr_elapse(arr_clock_t *r, uint32_t samples, uint32_t bpm)
{
    if (r->running) r->phase += samples * bpm;
}
#endif
