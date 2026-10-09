/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; sloopDX: (C) 2026 Sven Trogus */
/* Engine table: DX7, LOFI, VOICE, CZ-1, ANALOG. */
#include "dsp.c"
#include "eng_phase.c"
#include "eng_cz.c"
#include "eng_formant.c"
#include "eng_lofi.c"
#include "eng_dx7.c"
#include "eng_analog.c"

/* CZ-1 per-part state (resolves the forward declaration in eng_phase.c) */
static cz_part_t CZ_PARTS[NPART];
static cz_part_t *cz_part(uint32_t p) { return &CZ_PARTS[p % NPART]; }

static const engine_t *const ENGINES[NENGINES] = {&ENG_DX7, &ENG_LOFI, &ENG_FORMANT, &ENG_CZ,
                                                   &ENG_ANALOG};

/* every factory sound as loud as the others: a level trim per preset, 1/2 dB (tools/level_presets.py
 * writes preset_trim.h); a track keeps it in P_ED_FX */
#include "preset_trim.h"
static int16_t preset_trim(uint32_t e, uint32_t pi)
{
    return e < PT_ENGINES && pi < PT_MAX ? PRESET_TRIM[e][pi] : 0;
}
