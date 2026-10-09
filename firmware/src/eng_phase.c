/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Shared CZ / phase-distortion infrastructure: patch storage, voice state types,
 * envelope tick, pd_cos, and phase_note_on (used by ENG_CZ). The full PHASE
 * engine is available in melodee but not registered here. */
#include "cz_patch.h"
static cz_patch_t cz_patch[NTRK] __attribute__((section(".pool")));
static void cz_init(void) { for (uint32_t k = 0; k < NTRK; k++) cz_patch_init(cz_patch[k].raw); }

/* Eight rate/target points, with explicit sustain/end. Rates are Q24 per control tick. */
typedef struct { uint32_t rate[8]; int32_t level[8]; uint8_t sustain, end; } cz_env_def_t;
typedef struct { int32_t level; uint8_t stage, gate; } cz_env_t;
typedef struct { cz_env_t eg[2][3]; uint32_t vib_phase, vib_ticks, noise, phaser_ph; int32_t phaser_s[8]; } cz_voice_t;
typedef struct { cz_voice_t v[NPOLY]; } cz_part_t;
static cz_part_t *cz_part(uint32_t part);              /* engines.c: static array */
static cz_voice_t *cz_voice(track_t *t, voice_t *v)
{
    return &cz_part((uint32_t)(t - trk) % NPART)->v[(uint32_t)(v - t->v) % NPOLY];
}

static int32_t cz_env_tick(cz_env_t *e, const cz_env_def_t *d, uint32_t gate)
{
    uint32_t left = 65536u;
    if (e->gate && !gate && d->sustain <= d->end && e->stage <= d->sustain)
        e->stage = d->sustain + 1u;
    e->gate = (uint8_t)gate;
    for (uint32_t k = 0; k < 8u && e->stage <= d->end; k++) {
        uint32_t st = e->stage, rate = d->rate[st];
        int32_t target = st == d->end ? 0 : d->level[st];
        int32_t delta = target - e->level;
        uint32_t dist = (uint32_t)(delta < 0 ? -delta : delta);
        uint32_t step = (uint32_t)mulq16((int32_t)rate, left);
        if (dist > step) {
            e->level += delta < 0 ? -(int32_t)step : (int32_t)step;
            break;
        }
        e->level = target;
        if (gate && st == d->sustain) break;
        e->stage++;
        if (dist && rate) {
            uint32_t used = (uint32_t)(((uint64_t)dist << 16) / rate);
            left = used < left ? left - used : 0;
        }
        if (!left) break;
    }
    return e->level;
}

static inline int32_t pd_cos(uint32_t ph16)             /* -cos, Q15, from the 16-bit PD phase */
{
    return -sine_i(((ph16 & 0xFFFFu) << 16) + 0x40000000u);
}

static void phase_note_on(track_t *t, voice_t *v)
{
    cz_voice_t *c = cz_voice(t, v);
    memset(c, 0, sizeof *c);
    for (uint32_t l = 0; l < 2u; l++)
        for (uint32_t e = 0; e < 3u; e++) c->eg[l][e].gate = 1;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
    v->s[0] = v->s[1] = v->s[4] = 0;
    if (!voice_was) v->s[2] = v->s[3] = 0;
    c->noise = 0x6D2B79F5u ^ (uint32_t)(v - t->v) * 0x9E3779B9u;
}
