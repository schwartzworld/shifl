/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Effects: per-track DIST insert, then sends into three shared buses (chorus, tempo delay, reverb).
 * Stereo dry mix; the chorus and the reverb come back in stereo, the delay in the middle. */
#define DLY_LEN 44100u           /* 1.00 s: 1/4 note accurate >= 60 BPM */
#define CHO_LEN 2048u
static int16_t dly_buf[DLY_LEN] __attribute__((section(".pool")));
static int16_t cho_buf[CHO_LEN] __attribute__((section(".pool")));
/* the reverb: two input diffusers, then four delay lines mixed by a Hadamard matrix (a feedback delay
 * network: every echo feeds all four, so it thickens instead of ringing like a comb), damped in the
 * loop, one line slowly modulated (no metallic tone on long tails); left and right take different lines */
#define REV_MOD 12               /* samples the modulated line moves (+-) */
static const uint16_t REV_LINE[4] = {1559, 1931, 2389, 2791};   /* 35..63 ms, coprime */
static const uint16_t REV_AP[2] = {556, 441};
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2];   /* (.bss: the pool is full) */
static int16_t rev_ap[556 + 441] __attribute__((section(".pool")));
static struct {
    uint32_t dly_w, cho_w, cho_ph, rev_ph;
    int32_t dly_lp;
    uint16_t line_i[4], ap_i[2];
    int32_t line_lp[4];
} fx;

/* DIST: low cut -> drive (1x..8x, exponential) -> asymmetric soft clip
 * (a little bias = even harmonics) -> tone low-pass that closes with drive ->
 * make-up gain. State per part (track_t dist_*). */
static void track_dist(track_t *t, int32_t *b, uint32_t n)
{
    int32_t d = t->p[P_DIST], i, g, k, mk, bias = 2400, b0;
    if (!d) {
        t->dist_on = 0;
        return;
    }
    if (!t->dist_on) {                                  /* coming on: no stale high-pass state (a thump) */
        t->dist_on = 1;
        t->dist_hp = b[0];
        t->dist_lp1 = t->dist_lp2 = 0;
    }
    g = 4096 + d * d * 2;                                /* Q12: 1x .. ~9x, gentle at first */
    k = 32000 - d * 95;                                  /* tone: transparent at low drive .. ~3 kHz, Q15 */
    mk = 30000 - d * 120;                                /* make-up */
    b0 = softclip(bias);
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = b[i], y;
        t->dist_hp += (x - t->dist_hp + 64) >> 7;           /* ~55 Hz low cut: keep the bass out of the clipper */
        x = clamp(x - t->dist_hp, -230000, 230000);         /* (x >> 2) * g fits 32 bits; the clip is flat out there */
        y = softclip((((x >> 2) * g) >> 10) + bias) - b0;   /* >> 2 first: no overflow for loud poly */
        t->dist_lp1 += mulq15(y - t->dist_lp1, k);         /* two poles: tames the fizz */
        t->dist_lp2 += mulq15(t->dist_lp1 - t->dist_lp2, k);
        b[i] = mulq15(t->dist_lp2, mk);
    }
}

/* master: peak limiter in front of the soft clipper, a 35 ms hold then ~46 ms release (2.6), threshold where tanh is still nearly linear,
 * so chords get quieter instead of crushed. SLOOP 2.6: it looks one block ahead (lim_block): the output is the mix
 * CTL samples late (0.73 ms) and the gain glides over each block to what the loudest sample of it and of the next one
 * needs, so a hot kick or bass is already turned down when it comes. Before, the gain caught up in ~4 samples and the
 * first ones went through the tanh knee at up to 4x: a crackle on the beats with the MASTER up (in headphones most) */
#define LIM_T 18000
typedef struct {
    int32_t dl[CTL], dr[CTL];           /* the mix of the last block (the delay) */
    uint32_t w;                         /* its oldest sample (the next out) */
    int32_t env;                        /* the peak the gain fits under LIM_T (LIM_T: none) */
    int32_t g;                          /* the gain at the end of the last block, Q15 (32768: 1) */
    uint32_t hold;                      /* blocks before the release starts (LIM_HOLD after a peak near env) */
} lim_t;
#define LIM_HOLD 48u
#define LIM_REL 11                      /* the release after the hold: ~46 ms */
static lim_t lim_dac __attribute__((section(".pool")));
static lim_t lim_usb __attribute__((section(".pool")));
/* l, r: n (<= CTL) samples in; out: the n samples of CTL before, limited (before the knee) */
static void lim_block(lim_t *s, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, w = s->w;
    int32_t pk = 0, a, g0 = s->g, g1, dg, e = s->env;
    for (i = 0; i < CTL; i++) {
        a = s->dl[i] < 0 ? -s->dl[i] : s->dl[i], pk = a > pk ? a : pk;
        a = s->dr[i] < 0 ? -s->dr[i] : s->dr[i], pk = a > pk ? a : pk;
    }
    for (i = 0; i < n; i++) {
        a = l[i] < 0 ? -l[i] : l[i], pk = a > pk ? a : pk;
        a = r[i] < 0 ? -r[i] : r[i], pk = a > pk ? a : pk;
    }
    if ((int64_t)pk * 17 >= (int64_t)e * 16)
        s->hold = LIM_HOLD;
    if (s->hold)
        s->hold--;
    else if (e > LIM_T)
        e -= (int32_t)((((int64_t)(e - LIM_T) * (int32_t)n) >> LIM_REL) + 1);
    if (e < LIM_T)
        e = LIM_T;
    if (pk > e)
        e = pk, s->hold = LIM_HOLD;
    s->env = e;
    g1 = e > LIM_T ? (int32_t)(((uint32_t)LIM_T << 15) / (uint32_t)e) : 32768;
    s->g = g1;
    dg = (g1 - g0) / (int32_t)(n ? n : 1u);
    for (i = 0; i < n; i++) {
        int32_t xl = s->dl[w], xr = s->dr[w], g = g0 + dg * (int32_t)(i + 1u);
        s->dl[w] = l[i], s->dr[w] = r[i];
        w = (w + 1u) & (CTL - 1u);
        if (i + 1u == n)
            g = g1;
        if (g < 32768) {
            xl = (int32_t)(((int64_t)xl * g) >> 15);
            xr = (int32_t)(((int64_t)xr * g) >> 15);
        }
        l[i] = xl, r[i] = xr;
    }
    s->w = w;
}
static volatile uint8_t fx_lowcut;     /* settings: 12 dB/oct ~110 Hz for the small speaker */
static int32_t lc_l1, lc_l2, lc_r1, lc_r2, dc_l, dc_r, dce_l, dce_r;

/* DC blocker (~2 Hz), always on: a leaky integrator of the input (Q6 state) subtracted from it.
 * The >> 12 step keeps its remainder (error feedback, 0..4095) and adds it to the next one, so no
 * part of the step is lost: the state follows the input exactly, down to 0 after the sound stops.
 * (It was rounded, and a rounded step of (x - dc) / 4096 stops moving at |x - dc| < 2048: a
 * constant offset of up to +-31 stayed at the output after silence.) */
static inline int32_t dc_block(int32_t x, int32_t *dc, int32_t *err)
{
    int32_t e = (x << 6) - *dc + *err, d = e >> 12;
    *err = e - (d << 12);
    *dc += d;
    return x - ((*dc + 32) >> 6);
}

static int32_t lce[4];
static inline int32_t lowcut1(int32_t x, int32_t *lc, int32_t *err)   /* x minus its one-pole low-pass */
{
    int32_t e = x - *lc + *err, d = e >> 6;
    *err = e - (d << 6);
    *lc += d;
    return x - *lc;
}

/* the output stage: linear up to KNEE (a clean low end: no tanh harmonics on a loud sine), above it
 * a tanh knee with the same slope at the joint, to full scale */
#define KNEE 16384
static inline int32_t knee(int32_t x)
{
    int32_t a = x < 0 ? -x : x;
    if (a <= KNEE)
        return x;
    a = KNEE + (softclip((a - KNEE) * 2) >> 1);
    return x < 0 ? -a : a;
}

/* the USB audio input at full level (menu USB AUDIO = FULL): the same output stage with its own state,
 * fed as if MASTER were all the way up, so the recording level does not follow the knob. The DAC path
 * (master_pre, lim_dac) is untouched. audio.c sets usb_full_now for a render while the computer records. */
static uint8_t usb_full;                 /* menu USB AUDIO: 0 MASTER (follows the knob), 1 FULL */
static uint8_t usb_full_now;             /* this render fills usb_out (audio.c) */
static int32_t usb_out[2u * CTL];
static struct { int32_t dc[2], dce[2], lc[4], lce[4]; } uo;
static void master_pre_usb(int32_t *l, int32_t *r)
{
    *l = dc_block(*l, &uo.dc[0], &uo.dce[0]);
    *r = dc_block(*r, &uo.dc[1], &uo.dce[1]);
    if (fx_lowcut) {
        *l = lowcut1(*l, &uo.lc[0], &uo.lce[0]);
        *l = lowcut1(*l, &uo.lc[1], &uo.lce[1]);
        *r = lowcut1(*r, &uo.lc[2], &uo.lce[2]);
        *r = lowcut1(*r, &uo.lc[3], &uo.lce[3]);
    }
}

static inline void master_pre(int32_t *l, int32_t *r)
{
    *l = dc_block(*l, &dc_l, &dce_l);
    *r = dc_block(*r, &dc_r, &dce_r);
    if (fx_lowcut) {                  /* two one-pole high-passes, error feedback as dc_block (the */
        *l = lowcut1(*l, &lc_l1, &lce[0]);          /* rounded step stopped at |x - lc| < 32: an offset) */
        *l = lowcut1(*l, &lc_l2, &lce[1]);
        *r = lowcut1(*r, &lc_r1, &lce[2]);
        *r = lowcut1(*r, &lc_r2, &lce[3]);
    }
}



static uint32_t delay_samples(void)
{
    uint32_t s = dly_samples((uint32_t)song.g[G_DTIME]);
    return s < 16u ? 16u : s >= DLY_LEN ? DLY_LEN - 1u : s;
}

/* process the three buses for one block; sends in, wet out (stereo). The LFOs (chorus, reverb line)
 * are computed per block and ramped: no sine per sample */
static void fx_buses(const int32_t *cho_in, const int32_t *dly_in, const int32_t *rev_in, int32_t *wet_l,
                     int32_t *wet_r, uint32_t n)
{
    uint32_t i, k, dl = delay_samples();
    int32_t fb = song.g[G_DFDBK] * 230, col = 2000 + song.g[G_DCOLOR] * 240;
    int32_t dmix = song.g[G_DMIX] * 258;
    int32_t g = 17000 + song.g[G_RSIZE] * 104, lpk = 32767 - song.g[G_RDAMP] * 200;   /* loop gain (RT60 ~0.4..4 s), damping */
    int32_t cdepth = song.g[G_CDEPTH] * 6;
    int32_t ca0, ca1, cb0, cb1, ma, mb, dca, dcb;
    const uint32_t L0 = REV_LINE[0] + REV_MOD + 2u, B1 = L0, B2 = B1 + REV_LINE[1], B3 = B2 + REV_LINE[2];
    {   /* the chorus' two read points (Q8 samples back) and line 0's extra length, at both ends of the block */
        int32_t s0 = osc_sine(fx.cho_ph), s1, m0 = osc_sine(fx.rev_ph), m1;
        fx.cho_ph += LFO_INC[song.g[G_CRATE] & 127];
        fx.rev_ph += LFO_INC[30];
        s1 = osc_sine(fx.cho_ph);
        m1 = osc_sine(fx.rev_ph);
        ca0 = (400 << 8) + ((s0 + 32768) * cdepth >> 8), ca1 = (400 << 8) + ((s1 + 32768) * cdepth >> 8);
        cb0 = (400 << 8) + ((32767 - s0) * cdepth >> 8), cb1 = (400 << 8) + ((32767 - s1) * cdepth >> 8);
        ma = (REV_MOD << 8) + ((m0 * REV_MOD) >> 7), mb = (REV_MOD << 8) + ((m1 * REV_MOD) >> 7);
        dca = (ca1 - ca0) >> CTL_LOG2, dcb = (cb1 - cb0) >> CTL_LOG2;
    }
    for (i = 0; i < n; i++) {
        int32_t yl, yr, x, a, o0, o1, o2, o3;
        /* chorus: two modulated short delays, 5..15 ms, the LFO half a turn apart: left and right move apart */
        cho_buf[fx.cho_w & (CHO_LEN - 1u)] = (int16_t)clamp(cho_in[i] >> 1, -32768, 32767);
        {
            int32_t r0 = ca0 + dca * (int32_t)i, r1 = cb0 + dcb * (int32_t)i;
            uint32_t i0 = (uint32_t)r0 >> 8, i1 = (uint32_t)r1 >> 8;
            int32_t c0 = cho_buf[(fx.cho_w - i0) & (CHO_LEN - 1u)], c1 = cho_buf[(fx.cho_w - i0 - 1u) & (CHO_LEN - 1u)];
            int32_t d0 = cho_buf[(fx.cho_w - i1) & (CHO_LEN - 1u)], d1 = cho_buf[(fx.cho_w - i1 - 1u) & (CHO_LEN - 1u)];
            yl = (c0 + (((c1 - c0) * (r0 & 255)) >> 8)) << 1;
            yr = (d0 + (((d1 - d0) * (r1 & 255)) >> 8)) << 1;
        }
        fx.cho_w++;
        /* delay with a low-passed feedback (in the middle) */
        x = dly_buf[(fx.dly_w - dl) & (DLY_LEN - 1u)];
        fx.dly_lp += mulq15(x - fx.dly_lp, col);
        dly_buf[fx.dly_w & (DLY_LEN - 1u)] =
            (int16_t)clamp((dly_in[i] >> 1) + mulq15(fx.dly_lp, fb), -32768, 32767);
        fx.dly_w++;
        x = mulq15(x << 1, dmix);
        yl += x;
        yr += x;
        /* reverb: two diffusers, then the four lines */
        a = mulq15(rev_in[i], 13000);
        {
            int16_t *c = rev_ap;
            for (k = 0; k < 2u; k++) {
                int32_t b = c[fx.ap_i[k]], v = a + (b >> 1);
                c[fx.ap_i[k]] = (int16_t)clamp(v, -32768, 32767);
                a = b - (v >> 1);
                if (++fx.ap_i[k] >= REV_AP[k])
                    fx.ap_i[k] = 0;
                c += REV_AP[k];
            }
        }
        {
            int16_t *c = rev_line;
            int32_t s0, s1, d0, d1, r = ma + (((mb - ma) * (int32_t)i) >> CTL_LOG2);   /* (between the two: never below 0) */
            uint32_t ri = fx.line_i[0] + ((uint32_t)r >> 8), rj;
            if (ri >= L0)                                   /* (the oldest sample is at line_i: reading */
                ri -= L0;                                   /* past it shortens line 0 by 0..2 REV_MOD) */
            rj = ri + 1u >= L0 ? 0u : ri + 1u;
            o0 = c[ri] + (((c[rj] - c[ri]) * (r & 255)) >> 8);
            o1 = c[B1 + fx.line_i[1]];
            o2 = c[B2 + fx.line_i[2]];
            o3 = c[B3 + fx.line_i[3]];
            s0 = o0 + o1, d0 = o0 - o1, s1 = o2 + o3, d1 = o2 - o3;   /* Hadamard / 2: each feeds all four */
            fx.line_lp[0] += mulq15(((s0 + s1) >> 1) - fx.line_lp[0], lpk);
            fx.line_lp[1] += mulq15(((d0 + d1) >> 1) - fx.line_lp[1], lpk);
            fx.line_lp[2] += mulq15(((s0 - s1) >> 1) - fx.line_lp[2], lpk);
            fx.line_lp[3] += mulq15(((d0 - d1) >> 1) - fx.line_lp[3], lpk);
            c[fx.line_i[0]] = (int16_t)clamp(mulq15(fx.line_lp[0], g) + a, -32768, 32767);
            c[B1 + fx.line_i[1]] = (int16_t)clamp(mulq15(fx.line_lp[1], g) - a, -32768, 32767);
            c[B2 + fx.line_i[2]] = (int16_t)clamp(mulq15(fx.line_lp[2], g) + a, -32768, 32767);
            c[B3 + fx.line_i[3]] = (int16_t)clamp(mulq15(fx.line_lp[3], g) - a, -32768, 32767);
            if (++fx.line_i[0] >= L0) fx.line_i[0] = 0;
            if (++fx.line_i[1] >= REV_LINE[1]) fx.line_i[1] = 0;
            if (++fx.line_i[2] >= REV_LINE[2]) fx.line_i[2] = 0;
            if (++fx.line_i[3] >= REV_LINE[3]) fx.line_i[3] = 0;
        }
        wet_l[i] = yl + o0 + o2;
        wet_r[i] = yr + o1 - o3;
    }
}

/* one block of the whole mix (shared with tests/hostsim.c): events -> each part
 * -> dist -> SLICER -> level / pan / sends -> drums (-> SLICER) -> buses -> master; out: stereo Q15 */
static void events_block(uint32_t n);                    /* seq.c */
static int32_t send_c[CTL], send_d[CTL], send_r[CTL], wet_l[CTL], wet_r[CTL], mix_l[CTL], mix_r[CTL], part_buf[CTL];

/* ---- the DJ filter: v < 0 a low-pass closing, > 0 a high-pass opening, 0 off. The cutoff glides
 * to the knob (no zipper); at 0 it opens fully, then the filter is bypassed. On the master (G_FILT)
 * and, SLOOP 2.4, on each track (P_TFLT: a synth part's mono signal, the drum track's left, right and
 * reverb send) */
typedef struct {
    int32_t cut;                                        /* now, 0..127 << 8 (CUTOFF_HZ index) */
    int8_t mode;                                        /* -1 LP, 1 HP, 0 off */
    uint8_t seed;                                       /* bit per channel: seed the LP state from the first sample */
    int32_t z[4][2];                                    /* SVF states; 4 channels: L, R, reverb send, delay send */
} djf_t;
static djf_t djf, tflt[NTRK];

/* the knob v -> this block's coefficients in *c; 0 = bypassed (nothing to do) */
static int djf_block(djf_t *f, int32_t v, tsvf_t *c)
{
    int32_t to;
    if (v < 0 && f->mode >= 0) {                        /* (switching side: from open) */
        f->mode = -1;
        f->cut = 127 << 8;
        memset(f->z, 0, sizeof f->z);
        f->seed = 15;
    } else if (v > 0 && f->mode <= 0) {
        f->mode = 1;
        f->cut = 0;
        memset(f->z, 0, sizeof f->z);
    }
    if (!f->mode)
        return 0;
    to = f->mode < 0 ? (v < 0 ? (127 << 8) + v * 90 * 4 : 127 << 8) : (v > 0 ? v * 90 * 4 : 0);
    f->cut += clamp(to - f->cut, -384, 384);            /* ~1.5 index a block */
    if (!v && f->cut == to) {
        f->mode = 0;                                    /* fully open again: off */
        return 0;
    }
    tsvf_coef(c, f->cut, 40);
    return 1;
}
/* n samples of channel ch through the filter (x within +-140000) */
static void djf_run(djf_t *f, const tsvf_t *c, int32_t *b, uint32_t n, uint32_t ch)
{
    uint32_t i;
    int32_t *z = f->z[ch];
    if ((f->seed >> ch) & 1u) {
        f->seed &= (uint8_t)~(1u << ch);
        if (f->mode < 0 && n) {
            z[0] = 0;
            z[1] = clamp(b[0], -140000, 140000);
        }
    }
    for (i = 0; i < n; i++) {
        int32_t x = clamp(b[i], -140000, 140000), y = tsvf_lp(c, x, &z[0], &z[1]);
        b[i] = f->mode < 0 ? y : x - y;
    }
}
/* a track's filter on a signal at 4x its level (b >> 2 into the filter's range, back << 2) */
static void tflt_run(djf_t *f, const tsvf_t *c, int32_t *b, uint32_t n, uint32_t ch)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] >>= 2;
    djf_run(f, c, b, n, ch);
    for (i = 0; i < n; i++)
        b[i] <<= 2;
}

/* ---- mute / solo: a track that goes silent fades out over ~6 ms (and back in) */
#define MUTE_STEP 4096                                  /* Q15 per block: 8 blocks */
static int32_t gain_next(track_t *t)                    /* the track's mute gain at the end of this block */
{
    int32_t to = trk_silent(t) ? 32767 : 0, a = t->att;
    t->att = a < to ? (to - a > MUTE_STEP ? a + MUTE_STEP : to) : (a - to > MUTE_STEP ? a - MUTE_STEP : to);
    return 32767 - t->att;
}

/* ---- DUCK: every kick (the drum track's KICK / KICK 2) dips the synth parts, which come back over an
 * eighth note: depth G_DUCK, the curve (1 - t / T)^2. drum_on sets drums.kick. */
static struct {
    uint32_t t;                                         /* units since the kick */
    int32_t g0, g1;                                     /* the parts' gain at the block start, end (Q15) */
} duck = {0xFFFFFFFFu, 32767, 32767};

static void duck_block(uint32_t adv)
{
    int32_t depth = song.g[G_DUCK] * 258, x;
    uint32_t len = BEAT_U / 2u;
    duck.g0 = duck.g1;
    if (drums.kick) {
        drums.kick = 0;
        duck.t = 0;
    }
    if (!depth || duck.t >= len) {
        duck.g1 = 32767;
        return;
    }
    x = 32767 - (int32_t)((duck.t << 10) / (len >> 5));      /* 1 - t / T, Q15 (t < len < 2^21) */
    if (x < 0)
        x = 0;
    duck.g1 = 32767 - mulq15(depth, mulq15(x, x));
    duck.t = duck.t + adv < duck.t ? 0xFFFFFFFFu : duck.t + adv;
}

/* one synth part into the dry mix and the sends; a part with no voice sounding costs
 * the LFO tick and a cleared buffer only (after the DIST tail has run out) */
static void mix_part(track_t *t, uint32_t n)
{
    int32_t *b = part_buf;
    uint32_t i;
    int32_t g0 = 32767 - t->att, g1 = gain_next(t);
    if (track_render(t, b, n))
        t->tail = 16;                                   /* blocks of DIST state to run out after the last voice */
    else if ((!t->tail || !t->p[P_DIST] || !--t->tail) && !slicer_busy(t)) {
        slicer_track(t, 0, n);                          /* (the SLICER's step clock runs on) */
        return;
    }
    if (!g0 && !g1) {                                   /* silent (MUTE / SOLO): the voices run, nothing is heard */
        slicer_track(t, 0, n);
        return;
    }
    {
        int32_t lvl = LEVEL_Q12[t->p[P_LEVEL] ? clamp(t->p[P_LEVEL] + t->p[P_ED_FX], 1, 127) : 0], pan = t->p[P_PAN];   /* (+ the sound's trim: 1/2 dB steps, as LEVEL's) */
        int32_t gl = 4096 - (pan > 0 ? pan * 64 : 0), gr = 4096 + (pan < 0 ? pan * 64 : 0);
        int32_t c = t->p[P_CHOR] * 258, d = t->p[P_DLY] * 258, r = t->p[P_REV] * 258, pk = t->peak;
        int32_t xmax = c > d ? c : d;
        int32_t ga = mulq15(g0, duck.g0), gb = mulq15(g1, duck.g1);   /* mute x duck, ramped over the block */
        xmax = 0x7FFFFFFF / ((xmax > r ? xmax : r) | 1);   /* sends: loud chords at a high LEVEL */
        track_dist(t, b, n);
        slicer_track(t, b, n);                          /* slicer.c: before the level, pan and sends */
        {
            tsvf_t fc;                                  /* the track's FILTER (P_TFLT), after the SLICER */
            djf_t *f = &tflt[(uint32_t)(t - trk) % NTRK];
            if (djf_block(f, t->p[P_TFLT], &fc))
                tflt_run(f, &fc, b, n, 0);
        }
        int32_t lvl0 = t->lvl ? t->lvl : lvl, dl = (lvl - lvl0) >> CTL_LOG2;   /* a new sound's trim: ramped */
        t->lvl = lvl;
        for (i = 0; i < n; i++) {
            int32_t x = ((b[i] >> 2) * (lvl0 + dl * (int32_t)i)) >> 10, a;   /* pre-shift: 8 loud voices */
            int32_t xs, g = ga + (((gb - ga) * (int32_t)i) >> CTL_LOG2);
            if (g < 32767)
                x = (x >> 4) * (g >> 3) >> 8;           /* (Q15 in two halves: no 32-bit overflow) */
            a = x < 0 ? -x : x;
            xs = clamp(x, -xmax, xmax);                 /* sends: mulq15 would overflow */
            if (a > pk)
                pk = a;
            if (c)
                send_c[i] += mulq15(xs, c);
            if (d)
                send_d[i] += mulq15(xs, d);
            if (r)
                send_r[i] += mulq15(xs, r);
            mix_l[i] += ((x >> 4) * gl) >> 8;           /* (x may pass 2^19: >> 4 first) */
            mix_r[i] += ((x >> 4) * gr) >> 8;
        }
        t->peak = pk;
    }
}

/* ---- DUST: the master through an old sampler and a record. G_DUST 0..127 turns up together: drive
 * into a soft clip, a lower sample rate (held samples, 44.1 -> 11 kHz), fewer bits (15 -> 8), a
 * one-pole low-pass (open -> ~3 kHz), a little hiss and crackle. The hiss and the crackle are the
 * record turning: they fade in with PLAY and out (~0.1 s) at STOP, so a stopped SHIFL is silent.
 * Stereo, ~25 ops a sample. */
static struct {
    int32_t hl, hr, hn;                                 /* held samples, samples left to hold */
    int32_t ll, lr;                                     /* low-pass states */
    int32_t rnd, click;                                 /* noise state, a crackle decaying */
    int32_t bed;                                        /* hiss / crackle level, Q15: 0 stopped, 32767 playing */
} dust = {0, 0, 0, 0, 0, 0x2545F491, 0, 0};

static int32_t crush_bits(int32_t v, int32_t shift)    /* fewer bits, rounded toward 0: no DC from tails */
{
    return v >= 0 ? (v >> shift) << shift : -((-v >> shift) << shift);
}

static void dust_process(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t d = song.g[G_DUST], hold, shift, a, drive, hiss, i, bed0, bed1;
    uint32_t pc;
    if (!d) {
        dust.bed = 0;
        return;
    }
    hold = 1 + d * 3 / 127;
    shift = d / 18;
    a = 32767 - d * 165;                                /* one-pole coefficient, Q15 */
    drive = 4096 + d * 24;                              /* Q12: 1x .. 1.75x */
    hiss = d * 2;
    pc = (uint32_t)d * 7u;                              /* crackle: chance per sample, x 2^-22 */
    bed0 = dust.bed;                                    /* ~0.1 s from 0 to full (238 a block of 32) */
    bed1 = dust.bed = clamp(dust.bed + (song.playing ? 238 : -238), 0, 32767);
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = l[i], y = r[i];
        int32_t bed = bed0 + (((bed1 - bed0) * i) >> CTL_LOG2);
        uint32_t nz = noise32(&dust.rnd);
        if (--dust.hn <= 0) {                           /* sample and hold, then the bits */
            dust.hn = hold;
            dust.hl = softclip(((x >> 2) * drive) >> 10);
            dust.hr = softclip(((y >> 2) * drive) >> 10);
            if (shift) {
                dust.hl = crush_bits(dust.hl, shift);
                dust.hr = crush_bits(dust.hr, shift);
            }
        }
        dust.ll += mulq15(dust.hl - dust.ll, a);
        dust.lr += mulq15(dust.hr - dust.lr, a);
        if ((nz >> 10) < pc)                            /* a speck of dust */
            dust.click = mulq15(((int32_t)(nz & 0x3FFu) - 512) * d / 8, bed);
        x = dust.ll + dust.click + mulq15(((int32_t)(nz >> 16) - 32768) * hiss >> 15, bed);
        y = dust.lr + dust.click + mulq15(((int32_t)(nz & 0xFFFFu) - 32768) * hiss >> 15, bed);
        dust.click -= dust.click >> 2;
        l[i] = x;
        r[i] = y;
    }
}

static void djf_process(int32_t *l, int32_t *r, uint32_t n)
{
    tsvf_t c;
    if (!djf_block(&djf, song.g[G_FILT], &c))
        return;
    djf_run(&djf, &c, l, n, 0);
    djf_run(&djf, &c, r, n, 1);
}

#include "punch.c"            /* PUNCH-IN FX on the whole mix (FX held + a white key) */
static int32_t master_cur = -1;                        /* the volume knob, ramped per sample (no zipper) */
static void mix_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t m0, m1;
    for (i = 0; i < n; i++)
        send_c[i] = send_d[i] = send_r[i] = mix_l[i] = mix_r[i] = 0;
    events_block(n);
    duck_block(n * (uint32_t)song.g[G_BPM]);
    analog_nv = (uint8_t)voices_busy();             /* superwave voice count for CPU scaling (eng_analog.c) */
    for (i = 0; i < NPART; i++)
        mix_part(&trk[i], n);
    drums.a0 = TDRUM->att;                              /* the drum track's mute / solo fade */
    drums.a1 = 32767 - gain_next(TDRUM);
    {
        static int32_t dl[CTL], dr[CTL], dv[CTL], dd[CTL];
        tsvf_t fc;
        djf_t *f = &tflt[TRK_DRUM];
        if (n <= CTL && djf_block(f, TDRUM->p[P_TFLT], &fc)) {   /* the drum track's FILTER: left, right, reverb */
            for (i = 0; i < n; i++)
                dl[i] = dr[i] = dv[i] = dd[i] = 0;
            slicer_drums(dl, dr, dv, dd, n);
            tflt_run(f, &fc, dl, n, 0);
            tflt_run(f, &fc, dr, n, 1);
            tflt_run(f, &fc, dv, n, 2);
            for (i = 0; i < n; i++) {
                mix_l[i] += dl[i];
                mix_r[i] += dr[i];
                send_r[i] += dv[i];
                send_d[i] += dd[i];
            }
        } else {
            slicer_drums(mix_l, mix_r, send_r, send_d, n);      /* drums_render, through the SLICER when on */
        }
    }
    fx_buses(send_c, send_d, send_r, wet_l, wet_r, n);
    for (i = 0; i < n; i++) {
        mix_l[i] += wet_l[i];
        mix_r[i] += wet_r[i];
    }
    dust_process(mix_l, mix_r, n);
    punch_process(mix_l, mix_r, n);
    djf_process(mix_l, mix_r, n);
    m1 = (int32_t)song.master_q12;
    m0 = master_cur < 0 ? m1 : master_cur;
    master_cur = m1;
    if (n > CTL)
        n = CTL;
    {   /* send_c/send_d/send_r/wet_l are scratch: free after fx_buses */
        static uint8_t usb_was;
        for (i = 0; i < n; i++) {
            int32_t m = m0 + (((m1 - m0) * (int32_t)i) >> CTL_LOG2);
            send_c[i] = ((mix_l[i] >> 2) * m) >> 10;
            send_d[i] = ((mix_r[i] >> 2) * m) >> 10;
            master_pre(&send_c[i], &send_d[i]);
            if (usb_full_now) {
                send_r[i] = (mix_l[i] >> 2) << 2, wet_l[i] = (mix_r[i] >> 2) << 2;
                master_pre_usb(&send_r[i], &wet_l[i]);
            }
        }
        lim_block(&lim_dac, send_c, send_d, n);
        for (i = 0; i < n; i++) {
            out[2u * i] = knee(send_c[i]);
            out[2u * i + 1u] = knee(send_d[i]);
        }
        if (usb_full_now && !usb_was) {
            memset(&lim_usb, 0, sizeof lim_usb);
            lim_usb.env = LIM_T, lim_usb.g = 32768, lim_usb.hold = 0;
        }
        usb_was = usb_full_now;
        if (usb_full_now) {
            lim_block(&lim_usb, send_r, wet_l, n);
            for (i = 0; i < n; i++) {
                usb_out[2u * i] = knee(send_r[i]);
                usb_out[2u * i + 1u] = knee(wet_l[i]);
            }
        }
    }
}
