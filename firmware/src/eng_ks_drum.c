/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ian Schwartz */
/* KS membrane drum synthesis (Karplus-Strong with Schroeder allpass comb).
 * Included from drums.c.  One delay line per drum lane (monophonic retrigger).
 *
 * Per-lane state (ks_dr_t):
 *   N, M          main and allpass delay line lengths
 *   idx, ap_idx   circular buffer read/write pointers
 *   prev          previous main buffer sample Q15 (loop LP filter)
 *   g0, g1        loop filter weights g*(1-S) and g*S  Q15
 *   k             Schroeder allpass comb gain Q15
 *
 * P parameters (via DM macros):
 *   DM_TUNE   ±24 semitones — shifts N via PITCH_INC table
 *   DM_DECAY  ±40 steps     — adjusts loop decay factor g
 *   DM_LEVEL  ±20 dB        — output gain (handled by drums.gain)
 *   DM_PAN / DM_REV / DM_DLY / DM_CHOKE — standard per-lane
 */

#define KS_DR_BUF 600    /* main delay max: covers 73.5 Hz (D2) at 44.1 kHz */
#define KS_DR_AP  200    /* Schroeder allpass comb delay max */

static int16_t ks_dr_main[DRUM_LANES][KS_DR_BUF] __attribute__((section(".pool")));
static int16_t ks_dr_ap_b[DRUM_LANES][KS_DR_AP]  __attribute__((section(".pool")));

typedef struct {
    int16_t N, M;
    int32_t idx, ap_idx, prev, g0, g1, k;
} ks_dr_t;
static ks_dr_t ks_dr[DRUM_LANES] __attribute__((section(".pool")));

/* Per-lane patch: base pitch, AP delay length, loop decay, brightness, AP gain */
typedef struct { uint16_t base_p16, M; int16_t g; int8_t S, k; } ks_dr_def_t;
static const ks_dr_def_t KS_DR_DEF[DRUM_LANES] = {
    /* base_p16   M     g       S    k   -- lane / drum sound */
    {   624,  138, 28000,  32,  80 },  /*  0  KICK    ~80 Hz */
    {   688,  110, 27000,  32,  70 },  /*  1  KICK2  ~100 Hz */
    {   912,   50, 26000,  48,  50 },  /*  2  SNARE  ~220 Hz */
    {  1040,   31, 22000,  16,  30 },  /*  3  CLAP   ~350 Hz */
    {  1264,    7, 20000,  16,  90 },  /*  4  CHH    ~800 Hz metallic */
    {  1184,    9, 30000,  16,  90 },  /*  5  OHH    ~600 Hz metallic */
    {  1264,    7, 20000,  16,  90 },  /*  6  PEDAL  ~800 Hz metallic */
    {  1136,   22, 22000,  16,  40 },  /*  7  RIM    ~500 Hz */
    {   752,   85, 27500,  32,  70 },  /*  8  L TOM  ~130 Hz */
    {   848,   61, 27000,  32,  70 },  /*  9  M TOM  ~180 Hz */
    {   944,   44, 26500,  32,  60 },  /* 10  H TOM  ~250 Hz */
    {  1072,   28, 31000,  32,  85 },  /* 11  CRASH  ~400 Hz */
    {  1184,   18, 30500,  32,  75 },  /* 12  RIDE   ~600 Hz */
    {  1264,    7, 24000,  24,  85 },  /* 13  COWBEL ~800 Hz */
    {  1040,   31, 24000,  24,  50 },  /* 14  BONGO  ~350 Hz */
    {  1296,   12, 22000,  16,  30 },  /* 15  CLAV   ~900 Hz */
};

/* Choke groups: lanes 4/5/6 (hats) share group 1 */
static const uint8_t KS_CHOKE_DEF[DRUM_LANES] = {0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0};

static void ks_dr_hit(uint32_t lane, uint32_t vel, int32_t tune, int32_t decay,
                      int32_t bright, int32_t sweep_mac, int32_t noise_mac)
{
    if (lane >= DRUM_LANES) return;
    const ks_dr_def_t *def = &KS_DR_DEF[lane];
    ks_dr_t *v = &ks_dr[lane];
    int16_t *buf = ks_dr_main[lane];
    int16_t *ap  = ks_dr_ap_b[lane];
    uint32_t j;

    /* N from PITCH_INC table with TUNE offset */
    uint32_t p16 = (uint32_t)clamp((int32_t)def->base_p16 + tune * 16, 0, 2047);
    uint32_t inc = PITCH_INC[p16];
    uint64_t period_q8 = (4294967296ULL * 256ULL) / (uint64_t)inc;
    uint32_t N = (uint32_t)(period_q8 > 0xFFFFFFFFULL ? 0xFFFFFFFFu : period_q8) >> 8;
    if (N < 2u)        N = 2u;
    if (N >= KS_DR_BUF) N = KS_DR_BUF - 1u;

    /* M: allpass comb delay; DM_SWEEP scales inharmonicity (neg=less, pos=more) */
    int32_t M_i = clamp((int32_t)def->M * (40 + sweep_mac) / 40, 2, KS_DR_AP - 1);
    uint32_t M = (uint32_t)M_i;

    /* loop filter gain; DECAY macro tightens or loosens decay */
    int32_t g    = clamp((int32_t)def->g + decay * 200, 4096, 32767);
    /* S: tone brightness; DM_BRIGHT shifts the LP weight (higher S = brighter) */
    int32_t S_q15 = clamp((int32_t)(uint8_t)def->S * 258 + bright * 328, 0, 32767);
    int32_t g0    = mulq15(g, 32767 - S_q15);
    int32_t g1    = mulq15(g, S_q15);
    int32_t k_q15 = (int32_t)(uint8_t)def->k * 258;

    /* excitation: white noise scaled by velocity; DM_NOISE trims excitation amplitude */
    int32_t exc_vel = clamp((int32_t)vel + noise_mac * 2, 1, 127);
    int32_t nst = v->g0 ^ (int32_t)(lane * 0x9E3779B9u + (uint32_t)vel * 2654435761u);
    if (!nst) nst = 1;
    for (j = 0; j < N; j++) {
        int32_t s = (int32_t)(noise32(&nst) >> 17);
        buf[j] = (int16_t)clamp((s * exc_vel) >> 7, -32767, 32767);
    }
    for (j = 0; j < M; j++) ap[j] = 0;

    v->N = (int16_t)N;  v->M = (int16_t)M;
    v->idx = 0;         v->ap_idx = 0;
    v->prev = (int32_t)buf[N - 1u];
    v->g0 = g0;         v->g1 = g1;
    v->k  = k_q15;
}

static void ks_dr_run(uint32_t lane, int32_t *out, uint32_t n)
{
    if (lane >= DRUM_LANES) return;
    ks_dr_t *v = &ks_dr[lane];
    int16_t *buf = ks_dr_main[lane];
    int16_t *ap  = ks_dr_ap_b[lane];
    int32_t N    = v->N,  M  = v->M;
    int32_t idx  = v->idx, ap_idx = v->ap_idx;
    int32_t g0   = v->g0, g1 = v->g1, k = v->k, prev = v->prev;
    uint32_t i;

    if (N < 2) return;
    for (i = 0; i < n; i++) {
        int32_t cur = (int32_t)buf[idx];
        out[i] += cur;

        int32_t lp = mulq15(g0, cur) + mulq15(g1, prev);
        prev = cur;

        int32_t new_val;
        if (k) {
            /* Schroeder allpass comb: single-buffer form
             * b_old = ap[ptr]; w = b_old - k*x; ap[ptr] = x + k*w; y = w */
            int32_t b_old = (int32_t)ap[ap_idx];
            int32_t w     = b_old - mulq15(k, lp);
            ap[ap_idx] = (int16_t)clamp(lp + mulq15(k, w), -32767, 32767);
            if (++ap_idx >= M) ap_idx = 0;
            new_val = w;
        } else {
            new_val = lp;
        }

        buf[idx] = (int16_t)clamp(new_val, -32767, 32767);
        if (++idx >= N) idx = 0;
    }

    v->idx    = idx;
    v->ap_idx = ap_idx;
    v->prev   = prev;
}
