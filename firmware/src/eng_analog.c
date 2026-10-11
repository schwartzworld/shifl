/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* ANALOG: two band-limited oscillators (saw / square / tri / sine / PWM),
 * noise, drive and a trapezoidal low-pass.
 *
 * Jangada EDIT 3 / 4: SUPR up to 6 more copies of oscillator 1, spread by SDTN (a superwave:
 * copy k sits k steps of the spread above or below, so one accumulator of the spread phase drives
 * them all); SUB a square an octave below; DRFT a slow random wander of the pitch per voice;
 * FTYP LP12 (original), LP24, BP, HP or LADR (a four-pole transistor ladder); SAT a saturation
 * per voice after the filter, before the VCA (analog_sat), SDRV how hard; SPOS PRE moves it
 * before the filter. With more than 4 voices sounding the superwave keeps fewer copies (the CPU,
 * see analog_render_x). With all new params at their defaults the original render runs, sample
 * for sample (analog_render_lp); otherwise analog_render_x. */
static const char *const N_ANALOG_WAVE[] = {"SAW", "SQR", "TRI", "SIN", "PWM"};
static const char *const N_ANALOG_FTYP[] = {"LP12", "LP24", "BP", "HP", "LADR"};
static const char *const N_ANALOG_SAT[]  = {"OFF", "WARM", "HARD", "FOLD"};
static const char *const N_ANALOG_SPOS[] = {"POST", "PRE"};

/* SAT: three shapers, per voice after the filter (OSC -> FLT -> SAT -> VCA) or before (SPOS PRE).
 * WARM: tube tanh with a bias (even harmonics); HARD: hard wall; FOLD: sine folder. */
typedef struct { int32_t g, bias, b0, mk; } asat_t;
static inline void analog_sat_coef(asat_t *c, uint32_t type, int32_t d)
{
    if (type == 1) {
        c->g = 4096 + d * 48;
        c->bias = d * 40;
        c->b0 = softclip(c->bias);
        c->mk = (int32_t)(((uint32_t)32767u << 12) / (uint32_t)c->g);
    } else if (type == 2) {
        c->g = 4096 + d * 161;
        c->bias = c->b0 = 0;
        c->mk = 32767 - d * 90;
    } else {
        c->g = 32768 + d * 1806;
        c->bias = (int32_t)((uint32_t)d << 21);
        c->b0 = sine_i((uint32_t)c->bias);
        c->mk = 26000 - d * 80;
    }
}
static inline int32_t analog_sat(const asat_t *c, uint32_t type, int32_t x)
{
    if (type == 1)
        return mulq15(softclip(((clamp(x, -65536, 65535) * c->g) >> 12) + c->bias) - c->b0, c->mk);
    if (type == 2)
        return mulq15(clamp((clamp(x, -65536, 65535) * c->g) >> 12, -22000, 22000), c->mk);
    return mulq15(sine_i((uint32_t)clamp(x, -65536, 65535) * (uint32_t)c->g + (uint32_t)c->bias) - c->b0, c->mk);
}

/* LADR: four-pole transistor ladder with tanh feedback. State in v->s[0..4] (four stages) and
 * v->s[7] (last output / feedback). */
typedef struct { int32_t g, k, comp; } ladder_t;
static inline void ladder_coef(ladder_t *c, int32_t cut, int32_t reso)
{
    int32_t i, g;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = SVF_G[i];
    if (i < 127)
        g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    c->g = (g << 12) / (4096 + g);
    c->k = reso * 16900 / 127;
    c->comp = 4096 + (c->k >> 1);
}
static inline int32_t ladder_stage(int32_t x, int32_t g, int32_t *st)
{
    int32_t v = ((x - *st) * g) >> 12, y = v + *st;
    *st = clamp(y + v, -150000, 150000);
    return y;
}
static inline int32_t ladder_run(const ladder_t *c, int32_t in, int32_t *st, int32_t *y4)
{
    int32_t y = ((in * c->comp) >> 12) - ((*y4 * c->k) >> 12);
    y = soft_knee(ladder_stage(y, c->g, &st[0]), 24000);
    y = soft_knee(ladder_stage(y, c->g, &st[1]), 24000);
    y = soft_knee(ladder_stage(y, c->g, &st[2]), 24000);
    return *y4 = soft_knee(ladder_stage(y, c->g, &st[3]), 24000);
}

static void analog_note_on(track_t *t, voice_t *v)
{
    (void)t;
    if (!v->env && !v->env_out)
        v->ph[0] = 0;
    v->ph[1] = v->ph[0] + 0x40000000u;
    v->ph[2] = 0;                                     /* superwave spread phase */
    v->s[0] = v->s[1] = 0;                            /* filter (stage 1) */
    v->s[3] = v->s[4] = 0;                            /* filter (stage 2, LP24 / LADR) */
    v->s[5] = (int32_t)(v->ph[0] >> 1);               /* sub oscillator phase */
    v->s[6] = 0;                                      /* drift state */
    v->s[7] = 0;                                      /* LADR output */
    if (!v->s[2])
        v->s[2] = 0x1234567 + (int32_t)v->age;        /* noise state */
}

static uint8_t analog_nv;   /* voices sounding all parts: set in fx.c mix_block before parts render */

static inline int32_t analog_osc(uint32_t wave, uint32_t ph, uint32_t inc, uint32_t pw)
{
    switch (wave) {
    case 1: return osc_pulse(ph, inc, 0x80000000u);
    case 2: return osc_tri(ph);
    case 3: return osc_sine(ph);
    case 4: return osc_pulse(ph, inc, pw);
    default: return osc_saw(ph, inc);
    }
}

/* the superwave render: SUPR, SUB, DRFT, FTYP, SAT */
__attribute__((noinline)) static void analog_render_x(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                      const vmod_t *m)
{
    static const uint32_t COPY_PH[6] = {0x2B7E1516u, 0x9E3779B9u, 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au};
    static const int8_t COPY_AT[6] = {1, -1, 2, -2, 3, -3};
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0], i, k, ncopy = (uint32_t)p[P_E8], ftyp = (uint32_t)p[P_E12];
    uint32_t sat = (uint32_t)p[P_E13], pre = sat && p[P_E15];
    asat_t sc;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3], sub = p[P_E10], drift = p[P_E11];
    int32_t cut = (p[P_E4] << 8) + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    tsvf_t flt;
    ladder_t lad;
    int32_t drive = 32768 + p[P_E6] * 512, kq = tsvf_k(p[P_E5]);
    uint32_t inc1 = m->inc, inc2, dinc, subinc;
    int32_t d16 = det * 16 / 100, rem = det * 16 - d16 * 100;
    uint32_t pw = 0x80000000u + (uint32_t)((m->shape - (64 << 8)) << 15);
    int32_t m2 = mix * 258, m1 = 32767 - m2, nz = noise * 200, drv = p[P_E6];
    int32_t cg;
    /* reduce copy count under high voice load to keep CPU in budget */
    if (ncopy > 4u && analog_nv > 4u)
        ncopy = 4;
    if (ncopy > 2u && analog_nv > 6u)
        ncopy = 2;
    cg = 32767 * 10 / (10 + 6 * (int32_t)ncopy);
    int32_t sg = sub * 200;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], spr = v->ph[2], sph = (uint32_t)v->s[5];
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2], jc1 = v->s[3], jc2 = v->s[4];
    int32_t ls[4] = {ic1, ic2, jc1, jc2}, l4 = v->s[7];
    if (drift) {
        int32_t d = v->s[6], lim = drift * 30 * 256 / 127;
        d += ((int32_t)(noise32(&nst) >> 24) - 128) * drift / 8;
        d -= d >> 7;
        d = clamp(d, -lim, lim);
        v->s[6] = d;
        inc1 += (uint32_t)((int32_t)(inc1 >> 12) * ((d >> 8) * 2367 / 1000));
    }
    inc2 = PITCH_INC[clamp(m->pitch16 + d16, 0, 2047)];
    inc2 += (uint32_t)((int32_t)(inc2 >> 12) * (rem * 2367 / 16000));
    if (drift)
        inc2 += (uint32_t)((int32_t)(inc2 >> 12) * ((v->s[6] >> 8) * 2367 / 1000));
    if (det == 0)
        inc2 = inc1;
    /* SDTN 127 puts the outer copies (3 steps) about 50 cents away */
    dinc = (uint32_t)((int32_t)(inc1 >> 12) * (p[P_E9] * 50 * 2367 / (127 * 3 * 1000)));
    subinc = inc1 >> 1;
    if (ftyp == 4)
        ladder_coef(&lad, cut, p[P_E5]);
    else
        tsvf_coef(&flt, cut, p[P_E5]);
    if (sat)
        analog_sat_coef(&sc, sat, p[P_E14]);
    for (i = 0; i < n; i++) {
        int32_t a = analog_osc(wave, ph0, inc1, pw), b = analog_osc(wave, ph1, inc2, pw), s, y, bp, ab;
        if (ncopy) {
            int32_t sum = 0;
            for (k = 0; k < ncopy; k++)
                sum += analog_osc(wave, ph0 + (uint32_t)(int32_t)COPY_AT[k] * spr + COPY_PH[k],
                                  inc1 + (uint32_t)(int32_t)COPY_AT[k] * dinc, pw);
            sum = ((sum >> 2) * 19661) >> 13;
            a = mulq15(a, cg) + (((sum >> 2) * cg) >> 13);
        }
        ph0 += inc1;
        ph1 += inc2;
        spr += dinc;
        s = mulq15(a, m1) + mulq15(b, m2);
        if (sg) {
            s += mulq15(osc_pulse(sph, subinc, 0x80000000u), sg);
            sph += subinc;
        }
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        if (pre)
            s = analog_sat(&sc, sat, s);
        if (ftyp == 4)
            y = ladder_run(&lad, s >> 1, ls, &l4);
        else
            y = tsvf_lpbp(&flt, s >> 1, &ic1, &ic2, &bp);
        if (ftyp == 1)
            y = tsvf_lp(&flt, clamp(y, -100000, 100000), &jc1, &jc2);   /* LP24: second stage */
        else if (ftyp == 2)
            y = bp;
        else if (ftyp == 3)
            y = (s >> 1) - ((kq * bp) >> 12) - y;                        /* HP = in - k*bp - lp */
        ab = y < 0 ? -y : y;
        if (ab > 16000) {
            ab = 16000 + (softclip((ab - 16000) * 2) >> 1);
            y = y < 0 ? -ab : ab;
        }
        if (sat && !pre)
            y = analog_sat(&sc, sat, y << 1) >> 1;
        out[i] += mulq15(mulq15(y << 1, amp_at(m, i)), VOICE_FS) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = spr;
    if (ftyp == 4) {
        ic1 = ls[0]; ic2 = ls[1]; jc1 = ls[2]; jc2 = ls[3];
        v->s[7] = l4;
    }
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
    v->s[3] = jc1;
    v->s[4] = jc2;
    v->s[5] = (int32_t)sph;
}

/* the original two-osc LP12 render (all new params at default 0 take this path) */
__attribute__((noinline)) static void analog_render_lp(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                       const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t wave = (uint32_t)p[P_E0], i;
    int32_t det = p[P_E1], mix = p[P_E2], noise = p[P_E3];
    int32_t cut = (p[P_E4] << 8) + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    tsvf_t flt;
    int32_t drive = 32768 + p[P_E6] * 768;
    int32_t dw = p[P_E6] * 258;
    uint32_t inc1 = m->inc;
    int32_t d16 = det * 16 / 100, rem = det * 16 - d16 * 100;
    uint32_t inc2 = tuned_pitch_inc((uint32_t)clamp(m->pitch16 + d16, 0, 2047));
    inc2 += (uint32_t)((int32_t)(inc2 >> 12) * (rem * 2367 / 16000));
    uint32_t pw = 0x80000000u + (uint32_t)((m->shape - (64 << 8)) << 15);
    int32_t m2 = mix * 258, m1 = 32767 - m2;
    int32_t nz = noise * 200, drv = p[P_E6];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1];
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    tsvf_coef(&flt, cut, p[P_E5]);
    if (det == 0)
        inc2 = inc1;
    for (i = 0; i < n; i++) {
        int32_t a, b = 0, s;
        switch (wave) {
        case 1:
            a = osc_pulse(ph0, inc1, 0x80000000u);
            if (m2) b = osc_pulse(ph1, inc2, 0x80000000u);
            break;
        case 2:
            a = osc_tri(ph0);
            if (m2) b = osc_tri(ph1);
            break;
        case 3:
            a = osc_sine(ph0);
            if (m2) b = osc_sine(ph1);
            break;
        case 4:
            a = osc_pulse(ph0, inc1, pw);
            if (m2) b = osc_pulse(ph1, inc2, pw);
            break;
        default:
            a = osc_saw(ph0, inc1);
            if (m2) b = osc_saw(ph1, inc2);
            break;
        }
        ph0 += inc1;
        ph1 += inc2;
        s = m2 ? mulq15(a, m1) + mulq15(b, m2) : a;
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        if (drv)
            s += mulq15(softclip(((s >> 2) * (drive >> 2)) >> 11) - s, dw);
        {
            int32_t y = tsvf_lp(&flt, s >> 1, &ic1, &ic2), a = y < 0 ? -y : y;
            if (a > 16000) {
                a = 16000 + (softclip((a - 16000) * 2) >> 1);
                y = y < 0 ? -a : a;
            }
            s = y << 1;
        }
        out[i] += mulq15(mulq15(s, amp_at(m, i)), VOICE_FS) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
}

static void analog_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    if (p[P_E8] || p[P_E10] || p[P_E11] || p[P_E12] || p[P_E13])
        analog_render_x(t, v, out, n, m);
    else
        analog_render_lp(t, v, out, n, m);
}

static const preset_t ANALOG_PRESETS[] = {
    /* SLOOP hip-hop / drum & bass bank */
    {"808 BOOM",  {3, 0,  0,  0, 127,  0, 48,  0}, {0, 112,   0, 52}, 0, 1, FX(0,  0,  0,  0), XP(P_GLIDE+1, 59, P_GLMODE+1, 1, P_ED_PIT+1, 18, P_TRANS+1, -24)},
    {"808 DIRTY", {3, 0,  0,  0,  96, 12,118, 30}, {0, 108,   0, 46}, 0, 1, FX(24, 0,  0,  0), XP(P_GLIDE+1, 56, P_GLMODE+1, 1, P_ED_PIT+1, 24, P_TRANS+1, -24)},
    {"SUB BASS",  {3, 0,  0,  0, 127,  0, 10,  0}, {0,  60, 118, 24}, 0, 1, FX(0,  0,  0,  0), XP(P_GLIDE+1, 40, P_TRANS+1, -24)},
    {"808 SLIDE", {3, 0,  0,  0, 127,  0, 72,  0}, {0, 118,   0, 60}, 0, 1, FX(30, 0,  0,  0), XP(P_GLIDE+1, 82, P_GLMODE+1, 1, P_ED_PIT+1, 12, P_TRANS+1, -24)},
    {"ACID 303",  {0, 0,  0,  0,  36,112, 44,  64}, {0,  56,  24, 20}, 52, 1, FX(28, 0, 22, 10), XP(P_GLIDE+1, 34, P_GLMODE+1, 1, P_TRANS+1, -12)},
    {"PLUGG BASS",{2, 0,  0,  0,  92,  0, 30,  0}, {0,  96,  92, 34}, 0, 1, FX(0,  0,  0,  6), XP(P_GLIDE+1, 52, P_GLMODE+1, 1, P_TRANS+1, -24)},
    {"REESE",     {0,22, 64,  0,  56, 22, 36, 40}, {0,  70, 118, 28}, 0, 1, FX(0, 22,  0,  6), XP(P_LRATE+1, 22, P_LD_FLT+1, 10, P_TRANS+1, -24)},
    {"WOBBLE",    {0, 9, 64,  0,  38, 72, 62, 30}, {0,  64, 127, 22}, 0, 1, FX(10, 0,  0,  4), XP(P_LRATE+1, 74, P_LD_FLT+1, 40, P_TRANS+1, -24)},
    {"FUNK BASS", {1, 6, 50,  0,  50, 30, 22, 64}, {0,  55,  62, 20}, 30, 1, FX(0,  0,  0,  4), XP(P_GLIDE+1, 50, P_GLMODE+1, 1, P_TRANS+1, -24)},
    {"G-FUNK LD", {2, 0,  0,  0,  92,  0, 10, 64}, {6,  70, 112, 46}, 0, 1, FX(0,  0, 28, 30), XP(P_GLIDE+1, 74, P_LD_PIT+1, 2, P_LRATE+1, 80, P_LFADE+1, 50)},
    {"TRAP PLUCK",{0, 8, 64,  0,  52, 35, 20, 64}, {0,  80,  24, 45}, 48, 0, FX(0, 15, 34, 26)},
    {"SYN BRASS", {0,10, 64,  0,  45, 20, 16, 64}, {12, 70,  96, 35}, 34, 0, FX(0, 15, 12, 24)},
    {"SUPERSAW",  {0,14, 64,  0,  96, 12, 10, 64}, {4,  70, 112, 46}, 10, 1, FX(0, 20, 30, 36), XP(P_VOICE+1, 3, P_DETUNE+1, 64, P_GLIDE+1, 30)},
    {"WARM PAD",  {4,12, 64,  6,  52,  8,  0, 32}, {75, 90, 115, 90}, 0, 0, FX(0, 55, 20, 55), XP(P_LRATE+1, 30, P_LD_SHP+1, 20)},
    {"DARK STR",  {0,18, 64,  0,  48,  6,  0, 32}, {60, 90, 118, 85}, 0, 0, FX(0, 50, 18, 60)},
    {"ATMOS PAD", {4,25, 64, 20,  60, 15,  0, 32}, {90,100, 120,100}, 0, 0, FX(0, 60, 30, 75), XP(P_LRATE+1, 18, P_LD_PIT+1, 1)},
    {"PAN FLUTE", {2, 0,  0, 30,  70, 10,  0, 64}, {14, 70, 110, 40}, 0, 1, FX(0, 10, 24, 45), XP(P_LD_PIT+1, 1, P_LRATE+1, 80, P_LFADE+1, 70)},
    {"FAT LEAD",  {0, 8, 64,  0,  70, 30, 30, 64}, {2,  70, 110, 30}, 30, 1, FX(0,  0, 26, 20), XP(P_GLIDE+1, 40)},
    {"MONO BASS", {0, 4, 64,  0,  48, 36, 30, 64}, {0,  60,  70, 20}, 44, 1, FX(10, 0,  0,  4), XP(P_TRANS+1, -24)},
    {"PWM LEAD",  {4, 0,  0,  0,  80, 20, 10, 64}, {2,  70, 110, 30}, 0, 1, FX(0, 10, 26, 22), XP(P_LD_PIT+1, 1, P_LRATE+1, 80, P_LFADE+1, 60, P_GLIDE+1, 30)},
    {"SAW BASS",  {0, 6, 64,  0,  52, 30, 40, 64}, {0,  60,  60, 20}, 30, 1, FX(8,  0,  0,  4), XP(P_TRANS+1, -24)},
    {"SINE PAD",  {3,10, 64,  4,  90,  0,  0, 32}, {80, 90, 118, 90}, 0, 0, FX(0, 60, 20, 65)},
    /* superwave leads and pads (P_E8=SUPR, P_E9=SDTN, P_E11=DRFT, P_E12=FTYP) */
    {"SUPER SAW", {0,12,  0,  0,  95, 15,  0, 40}, {2,  80, 110, 55}, 10, 0, FX(0, 30, 40, 60), XP(P_E8+1, 6, P_E9+1, 50, P_E11+1, 10)},
    {"SUPER PAD", {0,20, 64,  0,  60, 20,  0, 30}, {90, 90, 115,100}, 6,  0, FX(0, 50, 30, 90), XP(P_E8+1, 5, P_E9+1, 70, P_E11+1, 30, P_E12+1, 1)},
    {"HP SHIMMER",{0,30, 64, 10,  70, 60,  0, 64}, {60, 90, 110,100}, 0,  0, FX(0, 60, 50,100), XP(P_E8+1, 4, P_E9+1, 80, P_E11+1, 20, P_E12+1, 3)},
    /* drones: held (P_AHOLD), slow LFO, superwave + filter type from P_E8/P_E12 */
    {"DRONE SAW", {0,40, 64, 20,  45, 35, 40, 20}, {120,90, 127,120}, 0,  0, FX(15,60, 35,110), ARP(7, 9, 1, 127), XP(P_E8+1, 6, P_E12+1, 1, P_AHOLD+1, 1, P_LRATE+1, 6)},
    {"LODO",      {4,14, 64,  8,  40,110, 30, 40}, {110,90, 127,110}, 0,  0, FX(15,40, 30,100), ARP(7, 9, 1, 127), XP(P_E8+1, 3, P_E12+1, 4, P_AHOLD+1, 1, P_LRATE+1, 7)},
    {"RABECA",    {0, 8, 64, 12,  70, 60, 30, 40}, {40, 90, 127, 90}, 0,  0, FX(10,30, 20, 80), ARP(7, 9, 1, 127), XP(P_E12+1, 2, P_AHOLD+1, 1, P_LRATE+1, 89, P_LD_PIT+1, 2)},
    {"INIT PATCH",{0, 0, 64,  0, 127,  0,  0, 64}, {0,  60, 127, 30}, 0,  0, FX(0,  0,  0,  0)},
};

static const engine_t ENG_ANALOG = {
    "ANALOG", {"OSC", "FLT"},
    {
        {"WAVE", F_ENUM,   0,   4,  0, N_ANALOG_WAVE, 0},
        {"DTN",  F_INT,    0, 127, 10, 0, "ct"},
        {"MIX",  F_PCT,    0, 127, 64, 0, 0},
        {"NOIS", F_PCT,    0, 127,  0, 0, 0},
        {"CUT",  F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES",  F_PCT,    0, 127, 30, 0, 0},
        {"DRV",  F_PCT,    0, 127,  0, 0, 0},
        {"KTR",  F_PCT,    0, 127, 64, 0, 0},
        {"SUPR", F_INT,    0,   6,  0, 0, 0},
        {"SDTN", F_PCT,    0, 127, 40, 0, 0},
        {"SUB",  F_PCT,    0, 127,  0, 0, 0},
        {"DRFT", F_PCT,    0, 127,  0, 0, 0},
        {"FTYP", F_ENUM,   0,   4,  0, N_ANALOG_FTYP, 0},
        {"SAT",  F_ENUM,   0,   3,  0, N_ANALOG_SAT,  0},
        {"SDRV", F_PCT,    0, 127, 64, 0, 0},
        {"SPOS", F_ENUM,   0,   1,  0, N_ANALOG_SPOS, 0},
    },
    ANALOG_PRESETS, sizeof(ANALOG_PRESETS) / sizeof(ANALOG_PRESETS[0]), 1, analog_note_on, analog_render,
    0xF986, {P_LEVEL, P_E4, P_E5, P_ED_FLT},
};
