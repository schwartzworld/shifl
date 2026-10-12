/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ian Schwartz */
/* KARPLUS-STRONG: plucked-string synthesis (Karplus & Strong 1983).
 *
 * A circular noise burst -> loop filter (weighted two-point average) + first-order
 * allpass fractional-delay tuning (Smith/Jaffe extensions).  Everything is
 * integer only (Q15 signals, Q15 coefficients).
 *
 * voice_t state layout
 *   s[0]  idx           circular buffer read/write index
 *   s[1]  N             delay line length (integer part)
 *   s[2]  ap_v          allpass: previous output y[n-1] (Q15)
 *   s[3]  ap_u          allpass: previous input  u[n-1] (Q15)
 *   s[4]  packed        bits 31..16 = eta (Q15, allpass coefficient)
 *                       bits 15..0  = prev (Q15, buf sample before oldest)
 *   s[5]  nst           xorshift noise state
 *   s[6]  g0            g * (1-S)   Q15  (loop filter weight on cur)
 *   s[7]  g1            g * S       Q15  (loop filter weight on prev)
 *
 * P_E parameters
 *   P_E0  DECA  T60 decay: 0 = 0.5 s, 127 = 8 s
 *   P_E1  TONE  brightness S: 0 = sharp (S≈0), 64 = classic (S≈0.5), 127 = mellow
 *   P_E2  PLUK  pluck position 0-127; depth also increases with value (0 = open)
 *   P_E3  BODY  excitation low-pass: 0 = bright/snappy, 127 = warm/round
 */

#define KS_NPOLY   2                          /* poly voice cap */
#define KS_BUF_MAX 540                        /* covers MIDI >= 40 (E2 = 82.4 Hz) at 44.1 kHz */

static int16_t ks_buf[NPART][KS_NPOLY][KS_BUF_MAX] __attribute__((section(".pool")));

/* -------------------------------------------------------------------------- */

static void ks_note_on(track_t *t, voice_t *v)
{
    uint32_t pt = (uint32_t)(t - trk);
    uint32_t vi = (uint32_t)(v - t->v);
    if (vi >= KS_NPOLY)
        return;

    const int16_t *p = t->p;
    int16_t *buf = ks_buf[pt][vi];

    /* ---- pitch: compute integer delay N and allpass fractional delay d ---- */
    /* Total loop delay = N + S (filter phase delay) + d (allpass) = fs/f0     */
    /* S = p[P_E1] / 127.0 (range 0..1); phase delay of (1-S)*z0 + S*z-1      */

    uint32_t p16 = (uint32_t)clamp(v->pitch16, 0, 2047);
    uint32_t inc  = PITCH_INC[p16];

    uint32_t S_q8 = (uint32_t)p[P_E1] * 2u;            /* S in Q8: 0..254 */

    /* period in Q8 = 2^32 * 256 / inc; subtract S to get D = period - S */
    uint64_t period_q8 = (4294967296ULL * 256ULL) / (uint64_t)inc;
    uint32_t D_q8      = (uint32_t)(period_q8 > 0xFFFFFFFFULL ? 0xFFFFFFFFu : period_q8) - S_q8;

    uint32_t N   = D_q8 >> 8;
    uint32_t d_q8 = D_q8 & 0xFFu;                      /* fractional part, range [0,1) in Q8 */

    if (N < 2u) N = 2u;
    if (N >= KS_BUF_MAX) N = KS_BUF_MAX - 1u;

    /* allpass: eta = (1-d)/(1+d).  d_q15 = d_q8 * 128 */
    int32_t d_q15   = (int32_t)(d_q8 * 128u);
    int32_t eta_q15 = ((32768 - d_q15) * 32768) / (32768 + d_q15);
    if (eta_q15 > 32767) eta_q15 = 32767;

    /* ---- decay factor g: target 60 dB fall in T60 seconds ---- */
    /* T60 = 0.5 + p[P_E0] * 7.5 / 127 s  =>  22050..375375 samples          */
    uint32_t t60_samp = 22050u + (uint32_t)p[P_E0] * 2598u;

    /* M = loops in T60 = f0 * t60_samp = inc * t60_samp / 2^32               */
    uint64_t M64 = ((uint64_t)inc * (uint64_t)t60_samp) >> 32;
    uint32_t M   = (uint32_t)(M64 < 1u ? 1u : M64);

    /* g ≈ exp(-6.908/M) ≈ 1 - 6.908/M  (first-order; good for M >> 7)       */
    int32_t g_q15;
    if (M < 8u) {
        g_q15 = 4096;                                   /* fast decay floor */
    } else {
        g_q15 = (int32_t)(32768u - 226529u / M);
        if (g_q15 < 4096)  g_q15 = 4096;
        if (g_q15 > 32767) g_q15 = 32767;
    }

    /* loop filter weights: g0 = g*(1-S), g1 = g*S (S in Q15)
     * Two-piece linear: spreads the audible range — more sweep in the bright half,
     * compressed in the "very mellow" half where differences are inaudible. */
    int32_t S_q15;
    if (p[P_E1] <= 64)
        S_q15 = (int32_t)p[P_E1] * 357;               /* 0..64  -> 0..22848  (≈0.70) */
    else
        S_q15 = 22848 + (int32_t)(p[P_E1] - 64) * 154; /* 64..127 -> 22848..32570 (≈0.99) */
    int32_t g0_q15 = mulq15(g_q15, 32767 - S_q15);
    int32_t g1_q15 = mulq15(g_q15, S_q15);

    /* ---- fill excitation buffer with white noise ---- */
    int32_t nst = v->s[5];
    if (!nst) nst = (int32_t)(v->age + 1u);
    uint32_t j;
    for (j = 0; j < N; j++)
        buf[j] = (int16_t)((int32_t)(noise32(&nst) >> 17));   /* Q15 range */

    /* ---- pluck position comb: x'[j] = x[j] - depth*x[(j-k+N)%N] ----
     * depth scales with PLUK: 0.25 at PLUK=1 up to 0.85 at PLUK=127
     * (fixed 50% depth was barely audible at low-mid values) */
    if (p[P_E2]) {
        uint32_t k = (uint32_t)p[P_E2] * N / 127u;
        if (k < 1u)  k = 1u;
        if (k >= N)  k = N - 1u;
        int32_t depth = 8192 + (int32_t)p[P_E2] * 193; /* Q15: 8385..32643 ≈ 0.26..1.0, capped */
        if (depth > 27853) depth = 27853;               /* cap at 0.85 to avoid instability */
        for (j = 0; j < N; j++) {
            uint32_t back = (j >= k) ? (j - k) : (j + N - k);
            int32_t v2 = (int32_t)buf[j] - mulq15(depth, (int32_t)buf[back]);
            buf[j] = (int16_t)clamp(v2, -32767, 32767);
        }
    }

    /* ---- BODY: low-pass the excitation for warm/round attack character ---- */
    if (p[P_E3]) {
        int32_t a = (int32_t)p[P_E3] * 258;             /* Q15 LP coefficient 0..32766 */
        int32_t lp_y = 0;
        for (j = 0; j < N; j++) {
            lp_y = mulq15(32767 - a, (int32_t)buf[j]) + mulq15(a, lp_y);
            buf[j] = (int16_t)clamp(lp_y, -32767, 32767);
        }
    }

    /* ---- pack and store state ---- */
    int32_t prev_init = (int32_t)buf[N - 1];
    v->s[0] = 0;
    v->s[1] = (int32_t)N;
    v->s[2] = 0;                                        /* ap_v */
    v->s[3] = 0;                                        /* ap_u */
    v->s[4] = ((eta_q15 & 0xFFFF) << 16) | ((uint32_t)(int16_t)prev_init & 0xFFFFu);
    v->s[5] = nst;
    v->s[6] = g0_q15;
    v->s[7] = g1_q15;
}

/* -------------------------------------------------------------------------- */

static void ks_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    uint32_t pt = (uint32_t)(t - trk);
    uint32_t vi = (uint32_t)(v - t->v);
    if (vi >= KS_NPOLY)
        return;

    int16_t *buf     = ks_buf[pt][vi];
    int32_t  idx     = v->s[0];
    int32_t  N       = v->s[1];
    int32_t  ap_v    = v->s[2];
    int32_t  ap_u    = v->s[3];
    int32_t  eta     = (int32_t)(v->s[4] >> 16);
    int32_t  prev    = (int32_t)(int16_t)(v->s[4] & 0xFFFFu);
    int32_t  g0      = v->s[6];
    int32_t  g1      = v->s[7];

    if (N < 2) N = 2;

    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t cur = (int32_t)buf[idx];

        /* output: cur is Q15; match the analog engine's scale convention */
        out[i] += mulq15(mulq15((int32_t)cur << 1, amp_at(m, i)), VOICE_FS) << 1;

        /* loop lowpass: lp = g0*cur + g1*prev  (= g * [(1-S)*cur + S*prev]) */
        int32_t lp = mulq15(g0, cur) + mulq15(g1, prev);
        prev = cur;

        /* allpass fractional delay: new_v = eta*(lp - ap_v) + ap_u */
        int32_t new_ap = mulq15(eta, lp - ap_v) + ap_u;
        ap_u = lp;
        ap_v = new_ap;

        buf[idx] = (int16_t)clamp(new_ap, -32767, 32767);

        if (++idx >= N)
            idx = 0;
    }

    v->s[0] = idx;
    v->s[2] = ap_v;
    v->s[3] = ap_u;
    v->s[4] = ((eta & 0xFFFF) << 16) | ((uint32_t)(int16_t)prev & 0xFFFFu);
}

/* -------------------------------------------------------------------------- */

static const char *const N_KS_TONE[] = {"BRIGHT", "MED", "MELLOW", 0};

static const preset_t KS_PRESETS[] = {
    /* name               e[0..7]={DECA,TONE,PLUK,...}   env={atk,dec,sus,rel}  mono  */
    /* DECA: 0=0.5s 127=8s  TONE: 0=sharp 127=mellow  PLUK: 0=clean 127=heavy comb */
    /* e[3] = BODY: 0=snappy bright attack, higher=warm/round */
    {"NYLON GT",  {80, 72,  22, 45,  0,  0,  0,  0}, {  0,  90, 127,  65}, 0, FX(0, 0, 14, 28)},
    {"STEEL GT",  {52, 15,  55, 10,  0,  0,  0,  0}, {  0,  70, 127,  38}, 0, FX(0, 0, 20, 18)},
    {"HARP",      {108, 55,  4, 18,  0,  0,  0,  0}, {  0, 110, 127,  85}, 0, FX(0, 0, 10, 45)},
    {"BANJO",     {30,  6,  82,  0,  0,  0,  0,  0}, {  0,  40, 127,  22}, 0, FX(5, 0,  6, 10)},
    {"KALIMBA",   {44, 38,   6, 28,  0,  0,  0,  0}, {  0,  50, 127,  42}, 0, FX(0, 0, 28, 22)},
    {"SITAR",     {68, 28,  98,  8,  0,  0,  0,  0}, {  0,  78, 127,  52}, 0, FX(0, 0, 10, 20)},
    {"KOTO",      {100, 88, 40, 55,  0,  0,  0,  0}, {  0,  95, 127,  75}, 0, FX(0, 0, 18, 40)},
    {"PLUCK BASS",{62, 50,  40, 35,  0,  0,  0,  0}, {  0,  65, 127,  40}, 0, FX(0, 0,  8, 12), XP(P_TRANS+1, -12)},
};

static const engine_t ENG_KS = {
    "KARPLUS", {"TONE", ""},
    {
        {"DECA", F_TIME,  0, 127,  80, 0, 0},
        {"TONE", F_PCT,   0, 127,  60, 0, 0},
        {"PLUK", F_PCT,   0, 127,  20, 0, 0},
        {"BODY", F_PCT,   0, 127,   0, 0, 0},
        {0}, {0}, {0}, {0},
    },
    KS_PRESETS, NELEM(KS_PRESETS),
    -1,                  /* fil_page: no filter page */
    ks_note_on, ks_render,
    0x07FF,              /* color: deep blue-green */
    {P_LEVEL, P_E0, P_E1, P_E2},   /* HOME knobs: level, decay, tone, pluck */
    KS_NPOLY,            /* poly cap */
};
