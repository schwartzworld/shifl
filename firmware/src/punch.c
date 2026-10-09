/* SPDX-License-Identifier: GPL-3.0-only */
/* PUNCH-IN FX, pocket-operator style: hold FX, press a white key (16 of them, F3..G5) and the
 * whole mix goes through that effect while the key is held; release it and the mix comes
 * back. Beat-synced to the tempo; loops start on the grid of the running transport.
 * A mono ring of the mix (PUNCH_N samples, 0.74 s) feeds the loops, oct-up, tape stop,
 * and echo tails; the filters, crush, alias and gate run in stereo. Every change
 * crossfades over 64 samples. Runs in the audio ISR (mix_block, fx.c). */
#define PUNCH_N 32768u                    /* power of two */
/* the transport clock (core.h clk_pos) places the loops and the gate on the grid */
static uint32_t clk_samples(void) { return clk_pos / (uint32_t)song.g[G_BPM]; }   /* samples into the beat */
#define PUNCH_NFX 32u
/* Page 1 (physical keys 0-15): 0-3 loops | 4-7 pitch/stop/slap | 8-10 delay tails | 11-13 character | 14-15 gate */
/* Page 2 (MIDI CC 118-133):   16-19 LPF  | 20-23 retrigger     | 24-27 pitch mangle | 28-31 glitch/texture */
enum { PX_LOOP4, PX_LOOP8, PX_LOOP16, PX_LOOP32, PX_OCT, PX_STPF, PX_STPS, PX_SLAP,
       PX_ECH4, PX_ECH2, PX_ECHO, PX_TEL, PX_CRUSH, PX_DOWN, PX_GATE, PX_GATE2,
       PX_LPFS, PX_LPFF, PX_LPFD, PX_LPFU,
       PX_RETRIG, PX_RETRIF, PX_STUTL, PX_STULL,
       PX_SHUF, PX_OCTDN, PX_SCRATCH, PX_VIBRATO,
       PX_GLITCH, PX_FEEDBK, PX_DISTORT, PX_BLINDS };
static const char *const PUNCH_NAME[PUNCH_NFX] = {
    "LOOP 4",  "LOOP 8",  "LOOP 16", "LOOP 32", "OCT UP",  "STOP F",  "STOP S",  "SLAP",
    "ECHO L",  "ECHO 2",  "ECHO",    "PHONE",   "CRUSH",   "ALIAS",   "GATE",    "GATE 2",
    "LPF SLW", "LPF FST", "LPF DWN", "LPF UP",  "RETRIG",  "RETRIG F","STUT L",  "STUT LL",
    "SHUF",    "OCT DN",  "SCRATCH", "VIBRATO", "GLITCH",  "FEEDBK",  "DISTORT", "BLINDS"};
static int16_t punch_ring[PUNCH_N] __attribute__((section(".pool")));
static struct {
    volatile int8_t req;          /* effect asked for by the keys (-1 none), ISR keyboard_block */
    volatile uint8_t hold;        /* FX button held (UI main loop) */
    volatile uint8_t cc_hold;     /* FX active via MIDI CC (independent of physical button) */
    int8_t song;                  /* punch FX queued by the active arrangement entry (-1 none) */
    uint32_t keybit;              /* the key that started it */
    int8_t cur;                   /* effect playing (fading out when req differs) */
    int32_t g;                    /* wet gain Q15 */
    uint32_t w;                   /* ring write index */
    uint32_t start, t, len;       /* loop / reverse: start index, samples since, length */
    uint32_t pre;                 /* loops: samples of dry still to go to the next division */
    uint32_t k, sub;              /* position in the loop; HALF: odd / even sample */
    uint32_t dspd;                /* tape stop: speed step per sample */
    uint32_t gp;                  /* gate: samples into the 1/16 step */
    uint32_t rp;                  /* tape stop / wobble: read position Q16 */
    uint32_t spd;                 /* tape stop speed Q16 */
    uint32_t lfo;                 /* wobble phase */
    int32_t held_l, held_r, hn;   /* downsample */
    int32_t f1l, f2l, f1r, f2r, f3l, f4l, f3r, f4r;   /* filter states */
    int32_t cut;                  /* sweep, 0..127 << 8 */
    int8_t latch;                 /* latched effect index (-1 = none) */
    uint8_t page;                 /* 0 = effects 0-15 on keys, 1 = effects 16-31 */
} punch = {.req = -1, .cur = -1, .song = -1, .latch = -1};

static uint32_t beat_samples(void) { return (uint32_t)FS * 60u / (uint32_t)song.g[G_BPM]; }

static int32_t ring_raw(uint32_t i) { return punch_ring[i & (PUNCH_N - 1u)]; }   /* the stored 16 bits (mix / 8) */
static int32_t ring_at(uint32_t i) { return ring_raw(i) << 3; }
static int32_t ring_q16(uint32_t p)                 /* read at Q16 position, linear (on the stored values: no overflow) */
{
    int32_t a = ring_raw(p >> 16), b = ring_raw((p >> 16) + 1u);
    return (a + (((b - a) * (int32_t)((p >> 1) & 0x7FFFu)) >> 15)) << 3;
}

static void punch_start(int32_t fx)
{
    uint32_t beat = beat_samples(), len, ph = clk_samples();
    static const uint8_t DIV[8] = {1, 2, 4, 8, 1, 1, 1, 1};   /* loops/oct/stop/slap: beat / DIV */
    punch.cur = (int8_t)fx;
    punch.t = 0;
    len = fx <= PX_SLAP ? beat / DIV[fx] : beat;
    if      (fx == PX_RETRIG)  len = beat >> 3;         /* 1/8 beat retrigger */
    else if (fx == PX_RETRIF)  len = beat >> 4;         /* 1/16 beat */
    else if (fx == PX_STUTL)   len = beat >> 2;         /* 1/4 beat */
    else if (fx == PX_STULL)   len = beat >> 1;         /* 1/2 beat */
    else if (fx == PX_OCTDN)   len = beat >> 1;         /* half-speed reads half-beat at 1x tempo */
    else if (fx == PX_SHUF)    len = beat >> 4;         /* 1/16 note slots */
    else if (fx == PX_GLITCH)  len = 512u;
    if (len > PUNCH_N - 1024u) len = PUNCH_N - 1024u;
    if (len < 64u)             len = 64u;
    punch.len = len;
    punch.pre = 0;
    punch.start = punch.w;
    /* retrigger, shuf, glitch: start reading from the most recent captured chunk */
    if ((fx >= PX_RETRIG && fx <= PX_STULL) || fx == PX_GLITCH)
        punch.start = punch.w > punch.len ? punch.w - punch.len : 0u;
    else if (fx == PX_SHUF) {
        uint32_t slot = (punch.w * 1664525u + 1013904223u) % 16u;
        punch.start = punch.w > punch.len * 16u ? punch.w - punch.len * 16u + slot * punch.len : 0u;
    }
    if (fx <= PX_OCT && song.playing) {                 /* on the grid: */
        uint32_t back = ph % len;
        if (len - back <= 1536u) {                      /* just before a division: from that one */
            punch.pre = len - back;
        } else {                                        /* else from the last division start */
            punch.start = punch.w - back;
            punch.t = back;
        }
    }
    punch.rp = punch.w << 16;
    punch.spd = 65536u;
    if (fx == PX_STPF)
        punch.dspd = 65536u / len + 1u;           /* stops in ~1 beat */
    else if (fx == PX_STPS)
        punch.dspd = 65536u / (len * 4u) + 1u;    /* stops in ~4 beats */
    else
        punch.dspd = 65536u / (len * 2u) + 1u;
    punch.sub = 0;
    punch.k = punch.t % len;
    punch.lfo = 0;
    punch.hn = 0;
    if      (fx == PX_LPFS)  punch.cut = 100 << 8;   /* slow sweep from bright */
    else if (fx == PX_LPFF)  punch.cut = 100 << 8;   /* fast sweep from bright */
    else if (fx == PX_LPFD)  punch.cut = 100 << 8;   /* sweep from bright */
    else if (fx == PX_LPFU)  punch.cut = 20 << 8;    /* sweep from dark */
    else                     punch.cut = 127 << 8;
    punch.f1l = punch.f2l = punch.f1r = punch.f2r = punch.f3l = punch.f4l = punch.f3r = punch.f4r = 0;
}

/* the wet sample for the mono ring effects (after the ring got this sample); no divide per
 * sample: k is the position in the loop, counted up and wrapped */
static int32_t punch_ring_fx(int32_t fx)
{
    uint32_t len = punch.len, k = punch.k;
    int32_t y;
    switch (fx) {
    case PX_OCT: {
        /* octave up at original tempo: read first half of captured beat at 2x, looped twice */
        uint32_t half = len >> 1;
        uint32_t kk = k >= half ? k - half : k;
        y = ring_at(punch.start + kk * 2u);
        punch.k = k + 1u >= len ? 0u : k + 1u;
        int32_t env = k < 64 ? (int32_t)k : len - k < 64 ? (int32_t)(len - k) :
                      kk < 64 ? (int32_t)kk : half - kk < 64 ? (int32_t)(half - kk) : 64;
        return (y * env) >> 6;
    }
    case PX_STPF:
    case PX_STPS:
        y = ring_q16(punch.rp);
        punch.rp += punch.spd;
        punch.spd = punch.spd > punch.dspd ? punch.spd - punch.dspd : 0u;
        return ((y >> 3) * (int32_t)(punch.spd >> 4)) >> 9;
    case PX_RETRIG:
    case PX_RETRIF:
    case PX_STUTL:
    case PX_STULL:
        /* re-capture fresh material at each loop boundary */
        if (punch.k >= punch.len) {
            punch.k = 0; k = 0;
            punch.start = punch.w > punch.len ? punch.w - punch.len : 0u;
        }
        y = ring_at(punch.start + k);
        punch.k = k + 1u;
        return k < 64u ? (y * (int32_t)k) >> 6 : punch.len - k < 64u ? (y * (int32_t)(punch.len - k)) >> 6 : y;
    case PX_SHUF:
        /* each 1/16 slot picks a random slot from the last beat */
        if (k >= len) {
            uint32_t slot = (punch.w * 1664525u + 1013904223u) % 16u;
            punch.start = punch.w > len * 16u ? punch.w - len * 16u + slot * len : 0u;
            punch.k = 0; k = 0;
        }
        y = ring_at(punch.start + k);
        punch.k = k + 1u;
        return k < 64u ? (y * (int32_t)k) >> 6 : len - k < 64u ? (y * (int32_t)(len - k)) >> 6 : y;
    case PX_OCTDN:
        /* half-speed: play each sample twice → octave down at same tempo */
        y = ring_at(punch.start + k);
        punch.k = k;
        punch.sub ^= 1u;
        if (!punch.sub) punch.k = k + 1u >= len ? 0u : k + 1u;
        return k < 64u ? (y * (int32_t)k) >> 6 : len - k < 64u ? (y * (int32_t)(len - k)) >> 6 : y;
    case PX_SCRATCH:
        punch.lfo += 2u * (0xFFFFFFFFu / FS);
        punch.rp = (uint32_t)((int32_t)punch.rp + (sine_i(punch.lfo) * 4));
        return ring_q16(punch.rp);
    case PX_VIBRATO: {
        int32_t d = 300 + (sine_i(punch.lfo) * 80 >> 15);
        punch.lfo += 6u * (0xFFFFFFFFu / FS);           /* 6 Hz */
        return ring_q16((punch.w << 16) - ((uint32_t)d << 16));
    }
    case PX_GLITCH:
        if (punch.k >= punch.len) {
            punch.len = 256u + ((punch.w * 1664525u + 1013904223u) & 0x7FFu);
            punch.start = punch.w > punch.len ? punch.w - punch.len : 0u;
            punch.k = 0;
        }
        y = ring_at(punch.start + punch.k);
        punch.k++;
        return y;
    default:                                        /* loops */
        y = ring_at(punch.start + k);
        break;
    }
    punch.k = k + 1u >= len ? 0u : k + 1u;
    /* declick at the seams */
    return k < 64u ? (y * (int32_t)k) >> 6 : len - k < 64u ? (y * (int32_t)(len - k)) >> 6 : y;
}

/* the loops, STUTTER, HALF and REVERSE replay ring samples [start, start + len) (REVERSE: up to start,
 * backwards). The ring keeps recording the mix, and after PUNCH_N samples (0.74 s) the writer comes
 * round into that span: it skips those slots once the loop is captured, so a held loop plays for as
 * long as the key is held. The rest of the ring keeps recording (the next effect has its past). */
static int punch_owns(int32_t fx, uint32_t w)
{
    uint32_t d = w - punch.start, len = punch.len;
    if (fx < 0 || fx > PX_OCT || punch.pre)
        return 0;
    return d >= len && (d & (PUNCH_N - 1u)) < len;
}

/* l, r: the mix before the master (fx.c mix_block), n samples, in place */
static void punch_process(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    int32_t want = punch.latch >= 0 ? punch.latch :
                   (punch.hold || punch.cc_hold) ? punch.req : punch.song;
    uint32_t beat, step, echo_d;
    tsvf_t c1, c2;
    if (punch.cur < 0 && want < 0) {                   /* idle: only the ring */
        for (i = 0; i < n; i++)
            punch_ring[punch.w++ & (PUNCH_N - 1u)] = (int16_t)clamp((l[i] + r[i]) >> 4, -32768, 32767);
        return;
    }
    if (punch.cur < 0 && want >= 0)
        punch_start(want);
    beat = beat_samples();                              /* per block: no divide per sample */
    step = beat / 4u ? beat / 4u : 1u;
    echo_d = beat * 3u / 4u;
    if (echo_d > PUNCH_N - 64u) echo_d = PUNCH_N - 64u;
    if (punch.cur == PX_GATE || punch.cur == PX_GATE2)
        punch.gp = (song.playing ? clk_samples() : punch.t) % step;
    /* filter coefficients: per block */
    if (punch.cur == PX_TEL) {
        tsvf_coef(&c1, 96 << 8, 90);                  /* PHONE: bandpass < 2.5 kHz */
        tsvf_coef(&c2, 58 << 8, 40);                  /* PHONE: low cut ~500 Hz */
    } else if (punch.cur >= PX_LPFS && punch.cur <= PX_LPFU) {
        int32_t b = (int32_t)beat;
        if (punch.cur == PX_LPFS)
            punch.cut = punch.cut > (20 << 8) ? punch.cut - (int32_t)n * (80 << 8) / (b * 3) : (20 << 8);
        else if (punch.cur == PX_LPFF)
            punch.cut = punch.cut > (20 << 8) ? punch.cut - (int32_t)n * (80 << 8) / (b / 2 > 1 ? b / 2 : 1) : (20 << 8);
        else if (punch.cur == PX_LPFD)
            punch.cut = punch.cut > (20 << 8) ? punch.cut - (int32_t)n * (80 << 8) / b : (20 << 8);
        else if (punch.cur == PX_LPFU)
            punch.cut = punch.cut < (100 << 8) ? punch.cut + (int32_t)n * (80 << 8) / b : (100 << 8);
        tsvf_coef(&c1, punch.cut, 20);
    }
    for (i = 0; i < n; i++) {
        int32_t x = l[i], y = r[i], wl = x, wr = y, m = (x + y) >> 1;
        int32_t fx = punch.cur;
        uint32_t target = want == fx ? 32767u : 0u;
        if (!punch_owns(fx, punch.w))                   /* (a held loop keeps its material) */
            punch_ring[punch.w & (PUNCH_N - 1u)] = (int16_t)clamp(m >> 3, -32768, 32767);
        switch (fx) {
        case PX_ECH4: {
            uint32_t ech4_d = beat * 3u / 2u;
            if (ech4_d > PUNCH_N - 64u) ech4_d = PUNCH_N - 64u;
            if (ech4_d < 64u) ech4_d = 64u;
            int32_t e4 = (ring_raw(punch.w - ech4_d) * 16000) >> 12;
            punch_ring[punch.w & (PUNCH_N - 1u)] = (int16_t)clamp((m + e4) >> 3, -32768, 32767);
            wl = x + e4;
            wr = y + e4;
            break;
        }
        case PX_ECH2: {
            uint32_t echo2_d = beat >> 1;
            if (echo2_d > PUNCH_N - 64u) echo2_d = PUNCH_N - 64u;
            if (echo2_d < 64u) echo2_d = 64u;
            int32_t e2 = (ring_raw(punch.w - echo2_d) * 18000) >> 12;
            punch_ring[punch.w & (PUNCH_N - 1u)] = (int16_t)clamp((m + e2) >> 3, -32768, 32767);
            wl = x + e2;
            wr = y + e2;
            break;
        }
        case PX_TEL: {
            int32_t b = tsvf_lp(&c1, m >> 1, &punch.f1l, &punch.f2l);        /* < 2.5 kHz */
            b -= tsvf_lp(&c2, b, &punch.f3l, &punch.f4l);                   /* > 500 Hz */
            wl = wr = softclip(b << 2) >> 1;
            break;
        }
        case PX_CRUSH:
            wl = x >= 0 ? x & ~0x7FF : -((-x) & ~0x7FF);   /* towards zero: no offset */
            wr = y >= 0 ? y & ~0x7FF : -((-y) & ~0x7FF);
            break;
        case PX_DOWN:
            if (--punch.hn <= 0) {
                punch.hn = 8;
                punch.held_l = x;
                punch.held_r = y;
            }
            wl = punch.held_l;
            wr = punch.held_r;
            break;
        case PX_GATE: {
            uint32_t p = punch.gp;
            int32_t gg = p < step / 2u ? 32767 : 0;
            punch.gp = p + 1u >= step ? 0u : p + 1u;
            if (p < 64u) gg = (int32_t)p * 512;
            else if (p >= step / 2u && p < step / 2u + 64u) gg = 32767 - (int32_t)(p - step / 2u) * 512;
            wl = ((x >> 3) * gg) >> 12;
            wr = ((y >> 3) * gg) >> 12;
            break;
        }
        case PX_GATE2: {                            /* staccato: on for 1/4 of step */
            uint32_t p = punch.gp;
            uint32_t on = step / 4u;
            int32_t gg = p < on ? 32767 : 0;
            punch.gp = p + 1u >= step ? 0u : p + 1u;
            if (p < 64u) gg = (int32_t)p * 512;
            else if (p >= on && p < on + 64u) gg = 32767 - (int32_t)(p - on) * 512;
            wl = ((x >> 3) * gg) >> 12;
            wr = ((y >> 3) * gg) >> 12;
            break;
        }
        case PX_SLAP: {
            uint32_t slap_d = beat >> 2;
            if (slap_d > PUNCH_N - 64u) slap_d = PUNCH_N - 64u;
            if (slap_d < 64u) slap_d = 64u;
            int32_t se = (ring_raw(punch.w - slap_d) * 12000) >> 12;
            wl = x + se;
            wr = y + se;
            break;
        }
        case PX_ECHO: {
            uint32_t d = echo_d;
            int32_t e;
            e = (ring_raw(punch.w - d) * 18000) >> 12;  /* (x 8 after: no overflow on a hot mix) */
            punch_ring[punch.w & (PUNCH_N - 1u)] = (int16_t)clamp((m + e) >> 3, -32768, 32767);   /* feedback */
            wl = x + e;
            wr = y + e;
            break;
        }
        case PX_LPFS:
        case PX_LPFF:
        case PX_LPFD:
        case PX_LPFU:
            wl = tsvf_lp(&c1, x, &punch.f1l, &punch.f2l);
            wr = tsvf_lp(&c1, y, &punch.f1r, &punch.f2r);
            break;
        case PX_FEEDBK: {
            int32_t ef = (ring_raw(punch.w - echo_d) * 24000) >> 12;   /* gain >1: builds up */
            punch_ring[punch.w & (PUNCH_N - 1u)] = (int16_t)clamp((m + ef) >> 3, -32768, 32767);
            wl = softclip(x + ef);
            wr = softclip(y + ef);
            break;
        }
        case PX_DISTORT:
            wl = softclip(x * 3) >> 1;
            wr = softclip(y * 3) >> 1;
            break;
        case PX_BLINDS: {
            int32_t amp = 16384 + (sine_i(punch.lfo) >> 1);            /* 0..32768 */
            punch.lfo += 8u * (0xFFFFFFFFu / FS);
            wl = ((x >> 3) * amp) >> 12;
            wr = ((y >> 3) * amp) >> 12;
            break;
        }
        default:
            if (fx < 0)
                break;
            if (punch.pre) {                            /* waiting for the division */
                if (!--punch.pre) {
                    punch.start = punch.w + 1u;
                    punch.k = 0;
                }
                break;
            }
            wl = wr = punch_ring_fx(fx);
            break;
        }
        if ((uint32_t)punch.g < target)
            punch.g = punch.g + 512 > 32767 ? 32767 : punch.g + 512;
        else if ((uint32_t)punch.g > target)
            punch.g = punch.g < 512 ? 0 : punch.g - 512;
        l[i] = x + ((((wl - x) >> 3) * (punch.g >> 3)) >> 9);   /* (no 32-bit overflow) */
        r[i] = y + ((((wr - y) >> 3) * (punch.g >> 3)) >> 9);
        punch.w++;
        punch.t++;
        if (punch.g == 0 && want != fx) {                /* faded out: the next one, or none */
            if (want >= 0)
                punch_start(want);
            else
                punch.cur = -1;
        }
    }
}

/* the keyboard (seq.c keyboard_block) while FX is held: white key index 0..15, -1 = black */
static int32_t punch_key(uint32_t k)
{
    static const int8_t W[12] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6};   /* from F */
    int32_t i = W[k % 12u];
    return i < 0 ? -1 : (int32_t)(k / 12u) * 7 + i + (int32_t)punch.page * 16;
}
