/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* DRUM engine: all 12 chromatic notes map to percussion sounds per octave.
 * Octave selects the sub-kit variation (C1=deep, C2=classic, C3=tight, C4=lo-fi).
 *
 *   C  = kick         C# = hi tom
 *   D  = snare        D# = clap
 *   E  = closed hat   F# = ride/cymbal
 *   F  = open hat     G# = cowbell
 *   G  = low tom      A  = kick+snare  (pre-mixed single voice)
 *                     A# = kick+hat+snare  (triple pre-mixed voice)
 *   B  = snare+hat   (pre-mixed single voice)
 *
 * Each track has its own circuit state; an older voice on the same slot retires
 * immediately when a newer hit supersedes it.
 *
 * P_E0  KIT   808 / 909 / FM  (909/FM: placeholder — 808 active)
 * P_E1  TUNE  0-127 (64=neutral): global pitch trim
 * P_E2  TONE  0-127 (64=neutral): timbre (BD/SD tone LP)
 * P_E3  DECY  0-127 (64=neutral): open hat / cymbal decay
 * P_E4  SNAP  0-127 (64=neutral): snare snap / noise level
 * P_E5  ACC   0-40: accent sensitivity
 * P_E6  ROUND 0/1: kick rounded mode (no attack transient)
 * P_E7  DRV   0-40: drive */

/* DVT roles used by drum_808.c's GM fallback map */
enum { DVT_PUNCH=0, DVT_ROUND, DVT_SNARE, DVT_CLAP, DVT_HATC, DVT_HATO, DVT_TOM, DVT_CONGA,
       DVT_RIM, DVT_CLAVE, DVT_BELL, DVT_CYM, DVT_COUNT };

/* Soft-float comparison stubs for the pi32v2 target (drum_808.c uses float in dr8_filter). */
static int32_t _fp_ord(float x)
{
    union { float f; uint32_t u; } b; b.f = x;
    return (int32_t)(b.u & 0x80000000u ? 0x80000000u - b.u : b.u);
}
int __gtsf2(float a, float b)  { return _fp_ord(a) > _fp_ord(b) ? 1 : 0; }
int __ltsf2(float a, float b)  { return _fp_ord(a) < _fp_ord(b) ? -1 : 0; }
int __gesf2(float a, float b)  { return _fp_ord(a) >= _fp_ord(b) ? 0 : -1; }
int __lesf2(float a, float b)  { return _fp_ord(a) <= _fp_ord(b) ? 0 : 1; }
int __eqsf2(float a, float b)
{
    union { float f; uint32_t u; } ua, ub; ua.f = a; ub.f = b;
    return (ua.u == ub.u || ((ua.u | ub.u) << 1) == 0) ? 0 : 1;
}
int __nesf2(float a, float b)  { return __eqsf2(a, b) == 0 ? 1 : 0; }
int __cmpsf2(float a, float b) { int32_t ia = _fp_ord(a), ib = _fp_ord(b); return ia < ib ? -1 : ia > ib ? 1 : 0; }
int __unordsf2(float a, float b) { (void)a; (void)b; return 0; }

#include "x0x/fastmath.h"
#include "drum_808.c"

/* ---- instrument slots ---------------------------------------------------- */
/* 9 single-instrument circuits + 7 combo component circuits = 16 dr8_t slots */
enum {
    DS_KICK  = 0,   /* C  kick            DR_BD */
    DS_HTOM  = 1,   /* C# hi tom          DR_HT */
    DS_SNARE = 2,   /* D  snare           DR_SD */
    DS_CLAP  = 3,   /* D# clap            DR_CP */
    DS_HAT   = 4,   /* E  closed hi-hat   DR_CH */
    DS_OHAT  = 5,   /* F  open hi-hat     DR_OH */
    DS_RIDE  = 6,   /* F# ride/cymbal     DR_CY */
    DS_LTOM  = 7,   /* G  low tom         DR_LT */
    DS_BELL  = 8,   /* G# cowbell         DR_CB */
    /* A combo: kick+snare */
    DS_CKS_K = 9,   DS_CKS_S = 10,
    /* A# combo: kick+hat+snare (triple) */
    DS_KHS_K = 11,  DS_KHS_H = 12,  DS_KHS_S = 13,
    /* B combo: snare+hat */
    DS_CSH_S = 14,  DS_CSH_H = 15,
    DS_TOTAL = 16
};

/* chromatic note index 0-11 → primary, secondary, tertiary slots (0xFF = none) */
static const uint8_t DS_SLOT1[12] = {
/*C */ DS_KICK,  /*C#*/ DS_HTOM,  /*D */ DS_SNARE, /*D#*/ DS_CLAP,
/*E */ DS_HAT,   /*F */ DS_OHAT,  /*F#*/ DS_RIDE,  /*G */ DS_LTOM,
/*G#*/ DS_BELL,  /*A */ DS_CKS_K, /*A#*/ DS_KHS_K, /*B */ DS_CSH_S
};
static const uint8_t DS_SLOT2[12] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, DS_CKS_S, DS_KHS_H, DS_CSH_H
};
static const uint8_t DS_SLOT3[12] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, DS_KHS_S, 0xFF
};

/* 808 instrument per circuit slot */
static const uint8_t DS_808[DS_TOTAL] = {
    DR_BD, DR_HT, DR_SD, DR_CP, DR_CH, DR_OH, DR_CY, DR_LT, DR_CB,
    DR_BD, DR_SD,           /* A combo */
    DR_BD, DR_CH, DR_SD,    /* A# combo */
    DR_SD, DR_CH            /* B combo */
};

/* sub-kit parameter index for each slot (0-8 = the 9 single instruments) */
static const uint8_t DS_SK_IDX[DS_TOTAL] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8,   /* singles: kick htom snare clap hat ohat ride ltom bell */
    0, 2,                          /* A combo: kick, snare */
    0, 4, 2,                       /* A# combo: kick, hat, snare */
    2, 4                           /* B combo: snare, hat */
};

/* ---- sub-kit and kit tables ---------------------------------------------- */
/* 4 sub-kits keyed by octave: C1=0 deep, C2=1 classic, C3=2 tight, C4+=3 lo-fi.
 * tune16: applied to dr8_t.pitch (semitones × 16); decay: applied to dr8_t.decay. */
#define DRUM_NSUBKIT 4
typedef struct { int8_t tune16; int8_t decay; } dsk_t;
static const dsk_t DR8_SK[DRUM_NSUBKIT][9] = {
    /*           kick        htom         snare        clap         hat          ohat         ride         ltom         bell       */
    /* 0 deep  */{{-3*16,25},{-2*16,20},  {0,   8},   {0,  12},   {0,   0},   {0,   0},   {0,  20},   {-2*16,22},  {0,  15}},
    /* 1 class */{{0,    0}, {0,    0},   {0,   0},   {0,   0},   {0,   0},   {0,   0},   {0,   0},   {0,    0},   {0,   0}},
    /* 2 tight */{{2*16,-18},{2*16, -15}, {2*16,-12}, {0, -10},   {0, -12},   {0, -25},   {0, -15},   {2*16,-15},  {0, -10}},
    /* 3 lo-fi */{{-5*16,35},{-3*16, 28}, {3*16,-5},  {2*16, 8},  {3*16, 18}, {0,  -18},  {-2*16,25}, {-4*16,30},  {3*16, 5}},
};

/* Per-kit offsets added on top of sub-kit: 808=zeros baseline; 909=punchy/tight; FM=metallic/synthetic.
 * 9 instruments: kick, htom, snare, clap, hat, ohat, ride, ltom, bell */
static const dsk_t DR8_KT[3][9] = {
    /* 808 */{{0,   0}, {0,   0},  {0,   0},  {0,  0}, {0,   0},  {0,   0},  {0,   0},  {0,   0},  {0,   0}},
    /* 909 */{{16,-35}, {16, -25}, {8,  -20}, {16, 0}, {16, -20}, {8,  -25}, {0,  -15}, {16, -25}, {8,  -10}},
    /* FM  */{{64, 30}, {48,  20}, {48, -10}, {64,10}, {64,  15}, {48,   5}, {48,  25}, {64,  30}, {48,  10}},
};

/* ---- per-track state ----------------------------------------------------- */
static dr8_t    ENG_DR8    [NTRK][DS_TOTAL];
static uint32_t ENG_DR8_AGE[NTRK][DS_TOTAL];
static int32_t  drum_tmp   [HALF_FRAMES];
static int32_t  drum_mix   [HALF_FRAMES];  /* per-voice scratch so voices accumulate into out */

static uint32_t drum_pi(const track_t *t) { return (uint32_t)(t - trk); }

/* ---- note decode --------------------------------------------------------- */
static uint32_t drum_ni(uint32_t note) { return note % 12; }  /* chromatic index 0-11 */

/* octave (note/12) → sub-kit: C1(24)=0 deep, C2(36)=1, C3(48)=2, C4(60)=3 lo-fi */
static uint32_t drum_sk(uint32_t note)
{
    int32_t oct = (int32_t)(note / 12) - 2;
    return (uint32_t)clamp(oct, 0, DRUM_NSUBKIT - 1);
}

static void drum_apply_sk(dr8_t *d, uint32_t sk, uint32_t slot, uint32_t kit)
{
    uint32_t idx = DS_SK_IDX[slot];
    int32_t t   = (int32_t)DR8_SK[sk][idx].tune16 + (int32_t)DR8_KT[kit][idx].tune16;
    int32_t dec = (int32_t)DR8_SK[sk][idx].decay   + (int32_t)DR8_KT[kit][idx].decay;
    d->pitch = clamp(t,   -127, 127);
    d->decay = clamp(dec, -127, 127);
}

/* ---- engine callbacks ---------------------------------------------------- */
static void drum_note_on(track_t *t, voice_t *v)
{
    uint32_t pi  = drum_pi(t);
    uint32_t ni  = drum_ni(v->note);
    uint32_t sk  = t->p[P_GLMODE]
                   ? rng() % DRUM_NSUBKIT
                   : (uint32_t)clamp((int32_t)drum_sk(v->note) + (int32_t)t->p[P_GLIDE], 0, DRUM_NSUBKIT - 1);
    uint32_t kit = (uint32_t)clamp((int32_t)t->p[P_E0], 0, 2);
    uint32_t s1  = DS_SLOT1[ni], s2 = DS_SLOT2[ni], s3 = DS_SLOT3[ni];
    uint32_t i1  = DS_808[s1];

    drum_apply_sk(&ENG_DR8[pi][s1], sk, s1, kit);
    dr8_hit(&ENG_DR8[pi][s1], i1, v->vel, t->p);
    ENG_DR8_AGE[pi][s1] = v->age;

    if (s2 != 0xFF) {
        drum_apply_sk(&ENG_DR8[pi][s2], sk, s2, kit);
        dr8_hit(&ENG_DR8[pi][s2], DS_808[s2], v->vel, t->p);
        ENG_DR8_AGE[pi][s2] = v->age;
    }
    if (s3 != 0xFF) {
        drum_apply_sk(&ENG_DR8[pi][s3], sk, s3, kit);
        dr8_hit(&ENG_DR8[pi][s3], DS_808[s3], v->vel, t->p);
        ENG_DR8_AGE[pi][s3] = v->age;
    }
}

static void drum_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t pi = drum_pi(t), i;
    uint32_t ni = drum_ni(v->note);
    uint32_t s1 = DS_SLOT1[ni], s2 = DS_SLOT2[ni], s3 = DS_SLOT3[ni];

    /* superseded by a newer hit: render nothing so the circuit isn't double-run */
    if (v->age < ENG_DR8_AGE[pi][s1])
        return;

    /* render into drum_mix so all voices accumulate into out rather than overwriting */
    dr8_run(&ENG_DR8[pi][s1], t->p, drum_mix, n);

    if (s2 != 0xFF) {
        dr8_run(&ENG_DR8[pi][s2], t->p, drum_tmp, n);
        for (i = 0; i < n; i++)
            drum_mix[i] = drum_mix[i] / 2 + drum_tmp[i] / 2;
    }
    if (s3 != 0xFF) {
        dr8_run(&ENG_DR8[pi][s3], t->p, drum_tmp, n);
        for (i = 0; i < n; i++)
            drum_mix[i] = (drum_mix[i] * 2 + drum_tmp[i]) / 3;
    }

    /* zero-crossing gate: when the voice budget forces this voice off (m->amp1 == 0),
     * find the first zero crossing and silence everything after it */
    if (m->amp1 == 0 && m->amp0 != 0) {
        uint32_t zc = n;
        for (i = 1; i < n; i++) {
            if ((drum_mix[i - 1] >= 0) != (drum_mix[i] >= 0)) { zc = i; break; }
        }
        for (i = zc; i < n; i++)
            drum_mix[i] = 0;
    }

    for (i = 0; i < n; i++)
        out[i] += drum_mix[i];
}

static int drum_done(track_t *t, voice_t *v)
{
    uint32_t pi = drum_pi(t);
    uint32_t ni = drum_ni(v->note);
    uint32_t s1 = DS_SLOT1[ni], s2 = DS_SLOT2[ni], s3 = DS_SLOT3[ni];

    if (v->age < ENG_DR8_AGE[pi][s1])
        return 1;
    if (s2 == 0xFF)
        return !ENG_DR8[pi][s1].on;
    {
        int s1_done = !ENG_DR8[pi][s1].on;
        int s2_done = !ENG_DR8[pi][s2].on || v->age < ENG_DR8_AGE[pi][s2];
        int s3_done = s3 == 0xFF || !ENG_DR8[pi][s3].on || v->age < ENG_DR8_AGE[pi][s3];
        return s1_done && s2_done && s3_done;
    }
}

static int32_t drum_amp(track_t *t, voice_t *v, int32_t adsr)
{
    (void)t; (void)v; (void)adsr;
    return 32767;
}

/* ---- presets ------------------------------------------------------------- */
/* e[]: KIT, TUNE, TONE, DECY, SNAP, ACC, ROUND, DRV */
static const preset_t DRUM_PRESETS[] = {
    {"808 KIT", {0, 64, 60, 75, 55, 20, 0, 10}, {0, 60, 0, 30}, 0, 0, FX(0, 0, 0, 15)},
    {"909 KIT", {1, 64, 80, 55, 85, 25, 0,  5}, {0, 60, 0, 30}, 0, 0, FX(0, 0, 0, 20)},
};

/* ---- parameter descriptors ----------------------------------------------- */
static const char *const N_DRUM_KIT[] = {"808", "909", "FM"};

static const engine_t ENG_DRUM = {
    .name        = "DRUM",
    .page_title  = {"DRUM", "DRUM"},
    .edit = {
        [0] = {"KIT",   F_ENUM,  0,   2,   0, N_DRUM_KIT},
        [1] = {"TUNE",  F_INT,   0,   127, 64},
        [2] = {"TONE",  F_INT,   0,   127, 64},
        [3] = {"DECY",  F_INT,   0,   127, 64},
        [4] = {"SNAP",  F_INT,   0,   127, 64},
        [5] = {"ACC",   F_INT,   0,   40,  0},
        [6] = {"ROUND", F_ONOFF, 0,   1,   0},
        [7] = {"DRV",   F_INT,   0,   40,  0},
    },
    .presets  = DRUM_PRESETS,
    .npresets = sizeof(DRUM_PRESETS) / sizeof(DRUM_PRESETS[0]),
    .color    = 0xF800,
    .knob     = {P_LEVEL, P_DLY, P_SPICE, P_DICE},
    .poly     = 0,
    .oneshot  = 1,
    .ownenv   = 1,
    .note_on  = drum_note_on,
    .render   = drum_render,
    .amp      = drum_amp,
    .done     = drum_done,
};
