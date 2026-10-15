/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Keyboard and its layers, scale and chords, arpeggiator, note repeat, sequencer, recording and
 * transport. Runs in the audio ISR, once per CTL-sample block (events_block), and ends in
 * trk_note_on / trk_note_off / drum_on: engines never see where a note came from.
 *
 * Time: the transport clock of fx.c (clk_beat, clk_pos, in units of a sample at 1 BPM). Every track
 * reads its step from it (trk_grid): the step is the clock divided by the track's DIV, swung, modulo
 * its LEN. So the tracks, the click, the arp, the rolls and the song arranger never drift apart, a
 * tempo or DIV change plays at most one step a block, and a polymeter (any LEN) stays in phase.
 *
 * Four tracks: tracks 1..3 are synth parts (steps of up to 4 notes), track 4 the drum track (steps
 * of 16 lanes, one per white key). Each step note / lane has a level (ghost .. hard) and a ratchet
 * (x1..x4 hits in its step). The keys play the selected track; MIDI channels 1..3 play parts 1..3,
 * the DRUMS channel (GLO > DRUMS, default 10) the drum track, any other channel the selected track.
 *
 * Layers: a function button held turns the keys into something else (TE style: hold + touch):
 *   FX   punch-in effects (punch.c)       EDIT  erase that note / sound (while held, as it plays)
 *   ARP  note repeat (roll) at G_ROLL     SEQ   steps 1..16 (the UI: ui_layers.c)
 *   SCL  the key of the song (the UI)     GLO   mute / solo / tap tempo (the UI)
 * On the drum track OCT- / OCT+ held play (and record) ghost / hard hits. */
#define SEQ_NONE 0xFFFFFFFFu


#define KB_SILENT 255u
static uint32_t kb_prev;
/* per key: what its press started, so its release ends the same (whatever the layer or track is now) */
enum { KS_NONE, KS_NOTE, KS_DRUM, KS_ERASE, KS_FX, KS_UI, KS_MOD };   /* KS_MOD: a chord modifier (2.4) */
static uint8_t kb_kind[27], kb_trk[27], kb_n[27], kb_nt[27][4], kb_root[27];   /* kb_root: a chord key's note (CHORD+) */
static uint8_t last_note = 60;
static uint8_t pen_n = 1, pen_note[4] = {60};   /* the last chord / note played: the SEQ layer writes it */
static uint8_t pen_lane;                       /* the last drum lane played: the SEQ layer's lane */
static volatile uint8_t transport_req;   /* 1 start, 2 stop (from the UI) */
static volatile uint8_t panic_req;       /* bit per track: release every sounding note (preset / engine change) */

static uint32_t trk_index(const track_t *t) { return (uint32_t)(t - trk); }

static uint32_t trk_midi_ch(uint32_t i)    /* MIDI channel 0..15 of track i (keys -> MIDI out) */
{
    if (i < NPART)
        return i;
    return song.g[G_DRCH] ? (uint32_t)song.g[G_DRCH] - 1u : 9u;
}

static uint32_t scale_mask(const track_t *t)
{
    const micro_scale_t *s = micro_scale(t);
    if (s) {    /* approximate pitch classes for LED highlighting; not used for audio when micro_active */
        uint32_t mask = 0, i;
        for (i = 0; i < (uint32_t)s->count; i++)
            mask |= 1u << ((uint32_t)((s->pitch[i] + 5000) / 10000) % 12u);
        return mask;
    }
    return SCALE_MASK[clamp(t->p[P_SCALE], 0, (int32_t)SCALE_LEGACY - 1)];
}

/* ---------------------------------------------------------- layers --- */
enum { LY_PLAY, LY_FX, LY_ERASE, LY_ROLL, LY_STEP, LY_SCALE, LY_MIX, LY_SONG, LY_COUNT };
static uint32_t ly_bit[LY_COUNT];        /* the button (fm1_in.buttons bit) of each layer: the UI sets them */
static uint32_t dyn_bit[2];              /* OCT- / OCT+: ghost / hard on the drum track */
/* a layer locked open (its button held + HOME tapped: ui_input.c), LY_PLAY = none: the keys and knobs
 * stay in it with the button let go, as if it were held */
static volatile uint8_t ly_lock = LY_PLAY;
static volatile uint8_t punch_page_on;   /* set by ui_input when PUNCH FX page is current */
static volatile uint32_t punch_page_on_t; /* fm1_ms when punch_page_on last went true */
static volatile uint8_t punch_showing;   /* punch_page_on && dwell >= SHOW_MS (no flash on quick scroll) */
static uint32_t layer_buttons(void) { return fm1_in.buttons | (ly_lock != LY_PLAY ? ly_bit[ly_lock % LY_COUNT] : 0u); }
/* the layer the keys are in: the held function button (FX, EDIT, ARP, SEQ, SCL, GLO in that order),
 * else the locked one */
static uint32_t layer_now(void)
{
    uint32_t b = layer_buttons(), l;
    for (l = LY_FX; l < LY_COUNT; l++)
        if (b & ly_bit[l])
            return l;
    return LY_PLAY;
}
/* the DRUMS grid page shown (the UI sets it every frame): with no layer, the keys are its steps (KB_GRID
 * events of lk_q, ui_studio.c grid_key) and play nothing */
#define KB_GRID LY_COUNT
static volatile uint8_t kb_grid;
/* the keys of the layers the UI handles (steps, key, mix): key k down / up, in order */
#define LKQ 16u
static volatile uint16_t lk_q[LKQ];
static volatile uint32_t lk_w, lk_r;
static void lk_push(uint32_t layer, uint32_t k, uint32_t down)
{
    if (lk_w - lk_r < LKQ) {
        lk_q[lk_w % LKQ] = (uint16_t)(layer << 8 | down << 7 | k);
        RING_PUBLISH();
        lk_w++;
    }
}

/* ------------------------------------------------------------- keys --- */
/* key k -> note on a synth part (KB_SILENT: none). WHITE (and chord mode): the white keys walk the
 * scale from C4 = the root, the black keys are silent; SNAP: every key, rounded down into the scale */
static uint32_t kb_map(const track_t *t, uint32_t k)
{
    static const int8_t DEGREE[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int32_t n = 53 + (int32_t)k;
    if (is_drum(t))
        return LANE_NOTE[lane_of_key(k)];
#if FELUCCA_SLICE
    if (ENGINES[t->eng_req % NENGINES] == &ENG_SLICE)   /* SLICE: lowest key = slice 0 (C4 + ROOT), no scale */
        return (uint32_t)clamp(SLC_BASE + t->p[P_ROOT] + 12 * song.octave + (int32_t)k, 0, 127);
#endif
    if (t->p[P_QUANT] == 1 && !t->p[P_CHORD]) {  /* SNAP: every key, rounded down to the scale (the old ON) */
        if (micro_active(t)) {
            uint32_t note = micro_snap(t, n + t->p[P_TRANS]);
            return note == KB_SILENT ? note : micro_degree_map(t, (int32_t)note - 60, 12 * song.octave, 1);
        }
        uint32_t mask = scale_mask(t), guard = 12;
        n += 12 * song.octave + t->p[P_TRANS];
        while (guard-- && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u))
            n--;
        return (uint32_t)clamp(n, 0, 127);
    }
    if (t->p[P_QUANT] == 2 || t->p[P_CHORD]) {   /* WHITE: white keys walk the scale, black keys are silent */
        int32_t degree = DEGREE[n % 12], oct;
        if (degree < 0)
            return KB_SILENT;
        /* C4 is the root. Walk scale degrees on successive white keys. */
        degree += (n / 12 - 5) * 7;
        if (micro_active(t))
            return micro_degree_map(t, degree, 12 * song.octave, 0);
        uint32_t mask = t->p[P_CHORD] && !t->p[P_SCALE] ? SCALE_MASK[2] : scale_mask(t), i;
        int32_t count = 0;
        for (i = 0; i < 12u; i++)
            count += (mask >> i) & 1u;
        oct = degree / count;
        degree %= count;
        if (degree < 0) {
            degree += count;
            oct--;
        }
        for (i = 0; i < 12u; i++)
            if ((mask >> i) & 1u) {
                if (!degree)
                    break;
                degree--;
            }
        n = 60 + t->p[P_ROOT] + 12 * oct + (int32_t)i;
    }
    return (uint32_t)clamp(n + 12 * song.octave + t->p[P_TRANS], 0, 127);
}

/* chord mode (P_CHORD): the chord of the scale built on note n (in the scale; CHR: minor), into c[];
 * the notes it holds (<= 4, the most a step keeps) */
static const int8_t CHORD_DEG[6][4] = {
    {0, -1, -1, -1},                     /* OFF */
    {0, 2, 4, -1},                       /* TRIAD: 1 3 5 */
    {0, 2, 4, 6},                        /* 7TH: 1 3 5 7 */
    {0, 2, 6, 8},                        /* 9TH: 1 3 7 9 (the lo-fi / R&B voicing) */
    {0, 3, 4, -1},                       /* SUS4: 1 4 5 */
    {0, -1, -1, -1},                     /* POWER: 1 5 8 (semitones, below) */
};
static uint32_t chord_notes(const track_t *t, uint32_t n, uint8_t *c)
{
    uint32_t type = (uint32_t)clamp(t->p[P_CHORD], 0, 5);
    uint32_t k = 0, j;
    if (type == 5u) {
        static const uint8_t PW[3] = {0, 7, 12};
        for (j = 0; j < 3u; j++)
            if (n + PW[j] < 128u)
                c[k++] = (uint8_t)(n + PW[j]);
        return k;
    }
    for (j = 0; j < 4u && CHORD_DEG[type][j] >= 0; j++) {
        if (micro_active(t)) {
            uint32_t m = n + (uint32_t)CHORD_DEG[type][j];
            if (m < 128u) c[k++] = (uint8_t)m;
            continue;
        }
        uint32_t mask = t->p[P_SCALE] ? scale_mask(t) : SCALE_MASK[2];
        int32_t m = (int32_t)n, d = CHORD_DEG[type][j], guard = 48;
        while (d > 0 && guard--) {                       /* d scale degrees up */
            m++;
            if ((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u)
                d--;
        }
        if (m < 128)
            c[k++] = (uint8_t)m;
    }
    return k;
}

/* CHORD+ (2.4): in chord mode the black keys are modifiers. Held while a white key plays (or pressed
 * while it is held: the chord changes under the finger), they change its chord: F# flips its third
 * (major <-> minor), G# adds the 7th, A# makes it sus4, C# adds the 9th, D# inverts it (its lowest note an
 * octave up); several at once combine. P_VLEAD ON voices each chord nearest the last one played on the track. */
enum { CM_MINOR = 1, CM_SEVEN = 2, CM_SUS4 = 4, CM_NINE = 8, CM_INV = 16 };
static uint32_t chord_mod_of_key(uint32_t k)            /* key k's modifier (0: a white key) */
{
    switch ((53u + k) % 12u) {
    case 6: return CM_MINOR;                            /* F# */
    case 8: return CM_SEVEN;                            /* G# */
    case 10: return CM_SUS4;                            /* A# */
    case 1: return CM_NINE;                             /* C# */
    case 3: return CM_INV;                              /* D# */
    default: return 0;
    }
}
static uint8_t vl_prev[NPART][4], vl_n[NPART];          /* the last chord played on each part (voice leading) */
static uint32_t scale_up(const track_t *t, uint32_t n, uint32_t deg)   /* deg scale degrees above n */
{
    if (micro_active(t))
        return (uint32_t)clamp((int32_t)n + (int32_t)deg, 0, 127);
    uint32_t mask = t->p[P_SCALE] ? scale_mask(t) : SCALE_MASK[2];
    int32_t m = (int32_t)n, guard = 48;
    while (deg > 0 && guard--) {
        m++;
        if ((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u)
            deg--;
    }
    return (uint32_t)m;
}
static void sort_notes(uint8_t *c, uint32_t n)
{
    uint32_t a, b;
    for (a = 1; a < n; a++)
        for (b = a; b > 0 && c[b - 1u] > c[b]; b--) {
            uint8_t x = c[b]; c[b] = c[b - 1u]; c[b - 1u] = x;
        }
}
/* the chord of white key n with the modifiers held, voiced (VLEAD); its notes (<= 4) into c[] */
static uint32_t chord_play_notes(track_t *t, uint32_t n, uint32_t mods, uint8_t *c)
{
    uint32_t k = chord_notes(t, n, c), j, type = (uint32_t)clamp(t->p[P_CHORD], 0, 5), part = trk_index(t);
    if (type != 5u && k >= 2u && mods) {                /* (POWER: the inversion only) */
        uint32_t third = scale_up(t, n, 2u);
        for (j = 0; j < k; j++) {
            if (c[j] == third && (mods & CM_SUS4))
                c[j] = (uint8_t)scale_up(t, n, 3u);     /* the 4th instead of the 3rd */
            else if (c[j] == third && (mods & CM_MINOR))
                c[j] = (uint8_t)(c[j] - n == 4u ? c[j] - 1u : c[j] - n == 3u ? c[j] + 1u : c[j]);
        }
        if ((mods & CM_SEVEN) && k < 4u) {
            uint32_t s7 = scale_up(t, n, 6u);
            for (j = 0; j < k && c[j] != s7; j++)
                ;
            if (j == k && s7 < 128u)
                c[k++] = (uint8_t)s7;
        }
        if (mods & CM_NINE) {
            uint32_t s9 = scale_up(t, n, 8u), s5 = scale_up(t, n, 4u);
            for (j = 0; j < k && c[j] != s9; j++)
                ;
            if (j == k && s9 < 128u) {
                if (k < 4u) {
                    c[k++] = (uint8_t)s9;
                } else {                                /* four already: the 9th for the 5th */
                    for (j = 0; j < k && c[j] != s5; j++)
                        ;
                    c[j < k ? j : k - 1u] = (uint8_t)s9;
                }
            }
        }
    }
    sort_notes(c, k);
    if (t->p[P_VLEAD] && part < NPART && vl_n[part] && k) {   /* the inversion and octave nearest the last chord */
        uint8_t best[4], cand[4];
        int32_t bcost = 0x7FFFFFFF, inv, oct;
        for (inv = 0; inv < (int32_t)k; inv++)
            for (oct = -1; oct <= 1; oct++) {
                int32_t cost = 0, ok = 1;
                uint32_t jj;
                for (jj = 0; jj < k; jj++) {
                    int32_t v = c[jj] + 12 * oct + (jj < (uint32_t)inv ? 12 : 0);
                    if (v < 24 || v > 108)
                        ok = 0;
                    cand[jj] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
                }
                if (!ok)
                    continue;
                sort_notes(cand, k);
                for (jj = 0; jj < k; jj++) {
                    int32_t d = (int32_t)cand[jj] - vl_prev[part][jj < vl_n[part] ? jj : vl_n[part] - 1u];
                    cost += d < 0 ? -d : d;
                }
                if (cost < bcost) {
                    bcost = cost;
                    memcpy(best, cand, k);
                }
            }
        if (bcost != 0x7FFFFFFF)
            memcpy(c, best, k);
    }
    if ((mods & CM_INV) && k >= 2u && c[0] + 12u < 128u) {   /* an inversion: the lowest an octave up */
        c[0] = (uint8_t)(c[0] + 12u);
        sort_notes(c, k);
    }
    if (part < NPART) {
        memcpy(vl_prev[part], c, k);
        vl_n[part] = (uint8_t)k;
    }
    return k;
}

/* ------------------------------------------------------ chord sequencer --- */
/* CQ_* chord quality names match CQ_* enum in core.h */
static const int8_t CSEQ_SEMI[CQ_COUNT][4] = {
    {-1, -1, -1, -1},              /* CQ_NONE */
    { 0,  4,  7, -1},              /* CQ_MAJ  */
    { 0,  3,  7, -1},              /* CQ_MIN  */
    { 0,  3,  6, -1},              /* CQ_DIM  */
    { 0,  4,  8, -1},              /* CQ_AUG  */
    { 0,  4,  7, 11},              /* CQ_MAJ7 */
    { 0,  3,  7, 10},              /* CQ_MIN7 */
    { 0,  4,  7, 10},              /* CQ_DOM7 */
    { 0,  3,  6,  9},              /* CQ_DIM7 */
    { 0,  3,  6, 10},              /* CQ_M7B5 */
    { 0,  3,  7, 11},              /* CQ_MMAJ7 */
    { 0,  4, 11, 14},              /* CQ_MAJ9 (drop 5th) */
    { 0,  3, 10, 14},              /* CQ_MIN9 (drop 5th) */
    { 0,  4, 10, 14},              /* CQ_DOM9 (drop 5th) */
};
static const char *const N_CQ[CQ_COUNT] = {
    "none", "maj", "min", "dim", "aug",
    "maj7", "min7", "dom7", "dim7", "m7b5", "mMaj7",
    "maj9", "min9", "dom9"
};

static chord_step_t cstep[NCHORD_STEP];  /* 64 bytes */
static uint8_t clen = 8;                 /* sequence length, 1..NCHORD_STEP */
static uint8_t cseq_div = 7;             /* step timing: N_SDIV index (7 = 1 bar / 4 beats) */
static uint32_t cseq_abs = SEQ_NONE;     /* transport step of the last chord advance */
static uint32_t cseq_idx;               /* current chord step index */
static uint8_t chord_root;              /* active chord root MIDI note (0 = none) */
static uint8_t chord_type;             /* active CQ_* type */
static uint8_t chord_inv;              /* active inversion 0-3 */
static int8_t  cseq_arr_tr;            /* arrangement transpose offset added to chord_root in cfol_apply */

/* notes of a chord step into buf[]; returns note count (0 = empty) */
static uint32_t cseq_notes(uint8_t root, uint32_t type, uint32_t inv, uint8_t *buf)
{
    uint32_t k = 0, j;
    const int8_t *semi;
    if (!root || type == CQ_NONE || type >= CQ_COUNT)
        return 0;
    semi = CSEQ_SEMI[type];
    for (j = 0; j < 4u && semi[j] >= 0; j++) {
        uint32_t n = (uint32_t)((int32_t)root + semi[j]);
        if (n < 128u)
            buf[k++] = (uint8_t)n;
    }
    /* apply inversion: cycle lowest note up one octave, once per inversion */
    for (j = 0; j < inv && k >= 2u; j++) {
        if (buf[0] + 12u < 128u) {
            uint8_t tmp = buf[0];
            uint32_t i;
            for (i = 0; i < k - 1u; i++)
                buf[i] = buf[i + 1u];
            buf[k - 1u] = (uint8_t)(tmp + 12u);
        }
    }
    return k;
}

static uint32_t grid_at(uint32_t, uint32_t, uint32_t *, uint32_t *);  /* defined in grid section */

/* advance the chord sequence one step per cseq_div grid boundary (called from events_block) */
static void cseq_tick(void)
{
    uint32_t into, len, abs;
    if (!song.playing)
        return;
    abs = grid_at(cseq_div, 0, &into, &len);
    (void)into; (void)len;
    if (abs == cseq_abs)
        return;
    cseq_abs = abs;
    {
        uint32_t n = (uint32_t)clen, idx = abs % n;
        const chord_step_t *cs;
        cseq_idx = (uint8_t)idx;
        cs = &cstep[idx];
        if (cs->note) {
            uint32_t fire = 1, cond = cstep_cond(cs);
            if      (cond == 1u) fire = (rng() & 0xFFu) < 192u;  /* P75 */
            else if (cond == 2u) fire = (rng() & 0xFFu) < 128u;  /* P50 */
            else if (cond == 3u) fire = (rng() & 0xFFu) < 64u;   /* P25 */
            if (fire) {
                chord_root = cs->note;
                chord_type = (uint8_t)cstep_type(cs);
                chord_inv  = (uint8_t)cstep_inv(cs);
            }
        }
        /* REST (note==0): keep existing active chord */
    }
}

/* ------------------------------------------------------ chord follow --- */
/* snap note n to scale; also used by the arranger fragment transpose */
static uint32_t seq_snap_scale(uint32_t n, const track_t *t)
{
    uint32_t mask = scale_mask(t), d, pc;
    int32_t root = t->p[P_ROOT];
    if (micro_active(t) || mask == 0xFFFu) return n;
    pc = (uint32_t)((int32_t)n - root + 120) % 12u;
    if (mask & (1u << pc)) return n;
    for (d = 1; d <= 6u; d++) {
        if (mask & (1u << ((pc + d) % 12u))) return (uint32_t)clamp((int32_t)n + (int32_t)d, 0, 127);
        if (mask & (1u << ((pc - d + 12u) % 12u))) return (uint32_t)clamp((int32_t)n - (int32_t)d, 0, 127);
    }
    return n;
}

/* nearest octave of any pitch class in pcs[0..cnt-1] to reference note ref */
static uint32_t cfol_closest(uint32_t ref, const uint8_t *pcs, uint32_t cnt)
{
    uint32_t best = ref, i;
    int32_t bdist = 200;
    for (i = 0; i < cnt; i++) {
        uint32_t pc = (uint32_t)pcs[i] % 12u;
        int32_t up = ((int32_t)pc - (int32_t)(ref % 12u) + 12) % 12;
        int32_t dn = (up == 0) ? 0 : up - 12;
        int32_t delta = (up <= -dn) ? up : dn;
        uint32_t cand = (uint32_t)clamp((int32_t)ref + delta, 0, 127);
        int32_t dist = (int32_t)cand >= (int32_t)ref ? (int32_t)(cand - ref) : (int32_t)(ref - cand);
        if (dist < bdist) { bdist = dist; best = cand; }
    }
    return best;
}

/* transform n according to P_CFOL and the current chord (chord_root/type/inv from cseq_tick).
 * cseq_arr_tr is the arrangement transpose offset applied on top of chord_root. */
static uint32_t cfol_apply(const track_t *t, uint32_t n)
{
    uint32_t mode, cn;
    uint8_t cbuf[4];
    uint8_t eff_root;

    if (is_drum(t) || !chord_root) return n;
    mode = (uint32_t)clamp(t->p[P_CFOL], 0, CF_COUNT - 1);
    if (mode == CF_OFF) return n;

    eff_root = (uint8_t)clamp((int32_t)chord_root + (int32_t)cseq_arr_tr, 1, 127);
    cn = cseq_notes(eff_root, chord_type, chord_inv, cbuf);

    switch (mode) {
    case CF_ROOT: {
        return cfol_closest(n, &eff_root, 1u);
    }
    case CF_ROOT5: {
        uint8_t r[2];
        r[0] = eff_root;
        r[1] = (uint8_t)clamp((int32_t)eff_root + 7, 0, 127);
        return cfol_closest(n, r, 2u);
    }
    case CF_CHORD:
        if (!cn) return n;
        return cfol_closest(n, cbuf, cn);

    case CF_3RD5TH: {
        uint8_t r[2];
        uint32_t rc = 0;
        if (cn > 1u) r[rc++] = cbuf[1];   /* 3rd */
        if (cn > 2u) r[rc++] = cbuf[2];   /* 5th */
        if (!rc) return n;
        return cfol_closest(n, r, rc);
    }
    case CF_3RD5THX:
        if (cn <= 1u) return n;
        return cfol_closest(n, cbuf + 1, cn - 1u);  /* 3rd, 5th, 7th, 9th */

    case CF_TRANSPOSE: {
        int32_t key = (int32_t)t->p[P_ROOT];
        int32_t delta = (int32_t)(eff_root % 12u) - key;
        if (delta > 6)  delta -= 12;   /* prefer shorter path */
        if (delta < -6) delta += 12;
        n = (uint32_t)clamp((int32_t)n + delta, 0, 127);
        return seq_snap_scale(n, t);
    }
    default:
        return n;
    }
}

/* ------------------------------------------------------------- grid --- */
/* units an odd step starts late: the track's + the global SWING (MPC: 0 = 50 %, 100 = 75 %) */
static uint32_t swing_units(int32_t pct, uint32_t u)
{
    return (uint32_t)clamp(pct, 0, 100) * u / 200u;
}
/* odd sub-beat steps sw late: abs step, units into it, its length */
static uint32_t grid_swing(uint32_t abs, uint32_t frac, uint32_t u, uint32_t sw, uint32_t *into, uint32_t *len)
{
    if (abs & 1u) {                                         /* an odd step: sw late */
        if (frac < sw) {
            abs--;
            frac += u;
            *len = u + sw;
        } else {
            frac -= sw;
            *len = u - sw;
        }
    } else {
        *len = u + sw;
    }
    *into = frac;
    return abs;
}
/* the clock on a grid of den steps a beat (the rolls: ROLL_DEN) */
static uint32_t grid_den(uint32_t den, uint32_t sw, uint32_t *into, uint32_t *len)
{
    uint32_t u = BEAT_U / den;
    return grid_swing(clk_beat * den + clk_pos / u, clk_pos % u, u, sw, into, len);
}
/* the clock on the grid of a division (N_SDIV: steps inside a beat, or of 2, 4, 8 whole beats) */
static uint32_t grid_at(uint32_t div, uint32_t sw, uint32_t *into, uint32_t *len)
{
    uint32_t m;
    if (div < NDIV_SHORT)
        return grid_den(DIV_DEN[div], sw, into, len);
    m = DIV_BEATS[(div - NDIV_SHORT) % 3u];                /* whole beats: the step from the beat count */
    return grid_swing(clk_beat / m, (clk_beat % m) * BEAT_U + clk_pos, BEAT_U * m, sw, into, len);
}
/* swing is for the straight grids inside a beat: off on the triplet grids and on steps of whole beats */
static uint32_t swings(uint32_t div) { return div < 4u; }
static uint32_t trk_div(const track_t *t) { return (uint32_t)t->p[P_SDIV] % NDIV_STEP; }
static uint32_t trk_grid(const track_t *t, uint32_t *into, uint32_t *len)
{
    uint32_t div = trk_div(t);
    return grid_at(div, swings(div) ? swing_units(t->p[P_SSWING] + song.g[G_SWING], div_units(div)) : 0u, into, len);
}
static uint32_t trk_len(const track_t *t) { return t->p[P_SLEN] > 0 ? (uint32_t)t->p[P_SLEN] : 1u; }

/* ------------------------------------------------------------- undo --- */
/* One step back (and forward again) for the pattern of one track: what it was before the last
 * recording pass, erase, step edit, tool or clear (a session: one mark). EDIT + OCT- / OCT+. */
static struct {
    uint8_t valid, undone, trk;
    int16_t len;
    uint32_t sess;
    step_t st[NSTEP];
} undo;
static uint32_t undo_sess = 1;           /* UI sessions (seq.c: recording passes use the track's pass) */
static void undo_mark(const track_t *t, uint32_t sess)
{
    uint32_t i = trk_index(t);
    if (undo.valid && !undo.undone && undo.trk == i && undo.sess == sess)
        return;                                          /* (this session is marked already) */
    memcpy(undo.st, t->step, sizeof undo.st);
    undo.len = t->p[P_SLEN];
    undo.trk = (uint8_t)i;
    undo.sess = sess;
    undo.valid = 1;
    undo.undone = 0;
}
#define UNDO_REC(t) (((t)->pass << 2) | 1u)      /* a recording pass of track t */
static uint32_t undo_erase_sess;

static volatile uint16_t evl_req;        /* (track*4+subtype) bit: mutation requested by the EVOLVE interval (ISR) */

/* --------------------------------------------------------- generate --- */
/* Private xorshift32 for deterministic, seed-based pattern generation. */
static uint32_t gen_next(uint32_t *r)
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return *r;
}

/* Pick an in-scale MIDI note using local prng *r.
 * prev != 0: 90% chance to prefer notes within 5 semitones (stepwise melody).
 * When a big jump is taken the octave is chosen to keep it nearest to prev. */
static uint32_t gen_pick_note(const track_t *t, uint32_t *r, uint32_t prev)
{
    if (micro_active(t)) {
        uint32_t count = micro_scale(t)->count;
        return (uint32_t)clamp((int32_t)60 + t->p[P_ROOT] + (int32_t)(gen_next(r) % count), 0, 127);
    }
    uint32_t mask = scale_mask(t), deg[12], ndeg = 0, i, oct;
    uint32_t near[24], nnear = 0;
    for (i = 0u; i < 12u; i++)
        if ((mask >> i) & 1u)
            deg[ndeg++] = i;
    if (!ndeg) { ndeg = 1u; deg[0] = 0u; }
    if (prev && (gen_next(r) & 15u) != 0u) {     /* 93.75%: prefer close intervals */
        for (oct = 0u; oct < 2u; oct++) {
            for (i = 0u; i < ndeg; i++) {
                uint32_t n = (uint32_t)clamp((int32_t)60 + t->p[P_ROOT] + (int32_t)deg[i] + (int32_t)oct * 12, 0, 127);
                uint32_t d = n > prev ? n - prev : prev - n;
                if (d > 0u && d <= 5u)
                    near[nnear++] = n;
            }
        }
        if (nnear)
            return near[gen_next(r) % nnear];
    }
    /* fallback big jump: pick degree, then octave nearest prev */
    i = deg[gen_next(r) % ndeg];
    {
        uint32_t n0 = (uint32_t)clamp((int32_t)60 + t->p[P_ROOT] + (int32_t)i, 0, 127);
        uint32_t n1 = (uint32_t)clamp((int32_t)60 + t->p[P_ROOT] + (int32_t)i + 12, 0, 127);
        uint32_t d0 = n0 > prev ? n0 - prev : prev - n0;
        uint32_t d1 = n1 > prev ? n1 - prev : prev - n1;
        return (prev && d1 < d0) ? n1 : n0;
    }
}

/* Same but uses the global rng (for mutation, where reproducibility is not needed). */
static uint32_t mutate_pick_note(const track_t *t)
{
    if (micro_active(t)) {
        uint32_t count = micro_scale(t)->count;
        return (uint32_t)clamp((int32_t)60 + t->p[P_ROOT] + (int32_t)(rng() % count), 0, 127);
    }
    uint32_t mask = scale_mask(t), deg[12], ndeg = 0, i;
    for (i = 0; i < 12u; i++)
        if ((mask >> i) & 1u)
            deg[ndeg++] = i;
    if (!ndeg) { ndeg = 1; deg[0] = 0; }
    i = deg[rng() % ndeg];
    return (uint32_t)clamp((int32_t)60 + t->p[P_ROOT] + (int32_t)i + (int32_t)(rng() & 1u) * 12, 0, 127);
}

/* Full from-scratch generation of a synth part.
 * P_SPICE (0-127) controls density: 0 = ~34% notes, 127 = ~59% notes.
 * Step 0 always lands on the root. Beat-downbeats (i%4==0) favour the root at 60%.
 * Stepwise motion is preferred 90% of the time; big jumps stay within the same octave. */
static void seq_generate(track_t *t)
{
    uint32_t len = trk_len(t), i, r;
    uint32_t spice    = (uint32_t)clamp(t->p[P_SPICE], 0, 127);
    uint32_t note_thr = 44u + spice / 4u;   /* density threshold: 44..75 / 128 */
    uint32_t acc_thr  = 22u + spice / 8u;   /* accent probability */
    uint32_t prev     = 0u;                 /* melodic memory: 0 = none yet */
    uint32_t root     = (uint32_t)clamp((int32_t)60 + t->p[P_ROOT], 0, 127);
    step_t   tmp[NSTEP];
    int      any = 0;

    r = rng();
    memset(tmp, 0, sizeof tmp);
    for (i = 0; i < len; i++) {
        step_t *s = &tmp[i];
        if (i == 0u) {                                         /* step 0: always the root */
            s->note[0] = (uint8_t)root;
            s->n = 1; s->time = ST_NOTE;
            prev = root; any = 1;
            continue;
        }
        if ((gen_next(&r) & 127u) >= note_thr) { s->time = ST_REST; continue; }
        s->note[0] = (uint8_t)gen_pick_note(t, &r, prev);
        if (i % 4u == 0u && (gen_next(&r) & 127u) < 77u)     /* beat downbeat: 60% root */
            s->note[0] = (uint8_t)root;
        prev = s->note[0];
        s->n = 1; s->time = ST_NOTE;
        if ((gen_next(&r) & 127u) < acc_thr) s->flags  = SF_ACCENT;
        if ((gen_next(&r) & 127u) < 22u)     s->flags |= SF_SLIDE;
        any = 1;
    }
    if (!any) {                              /* guarantee at least one note */
        tmp[0].note[0] = (uint8_t)root;
        tmp[0].n = 1; tmp[0].time = ST_NOTE;
    }
    for (i = 0; i < len; i++) {             /* drop slides that lead into a rest */
        uint32_t nx = (i + 1u) % len;
        if (flags_is_slide(tmp[i].flags) && tmp[nx].time == ST_REST)
            tmp[i].flags &= ~SF_SLIDE;
    }
    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(t->step, tmp, len * sizeof(step_t));
    t->gen_seed = r;
    fm1_irq_on();
}

/* Full from-scratch generation of the drum part.
 * Foundation: kick on 1+3, snare on 2+4, 8th-note hi-hat.
 * P_SPICE controls how much variation is added on top of the foundation. */
static void drum_generate(track_t *t)
{
    uint32_t len = trk_len(t), i, r;
    uint32_t spice = (uint32_t)clamp(t->p[P_SPICE], 0, 127);
    uint32_t var   = spice / 2u;   /* 0..63 variation budget */
    dstep_t  tmp[NSTEP];

    r = rng();
    memset(tmp, 0, sizeof tmp);
    for (i = 0; i < len; i++) {
        dstep_t *d = &tmp[i];
        uint32_t p16 = (i * 16u) / len;   /* normalised 16th-note position 0..15 */

        /* Kick (lane 0): 1 and 3 backbone; syncopation at high spice */
        {
            uint32_t thr = (p16 == 0u)  ? 115u :                       /* beat 1: near-certain */
                           (p16 == 8u)  ?  95u :                        /* beat 3 */
                           (p16 == 4u || p16 == 12u) ?                  /* beats 2 and 4: low unless spicy */
                               (var > 32u ? var / 2u : 8u) :
                           (p16 % 2u == 0u && var > 40u) ? var / 4u :  /* even offbeats at high spice */
                                                            0u;
            if (thr && (gen_next(&r) & 127u) < thr)
                dstep_set(d, 0u, LV_NORM, 0u);
        }
        /* Snare (lane 2): 2 and 4 backbone; ghost at offbeats only with plenty of spice */
        {
            uint32_t thr = (p16 == 4u || p16 == 12u) ? 115u :          /* backbeat: near-certain */
                           (p16 != 0u && p16 != 8u && var > 48u) ?
                               var / 6u :                               /* ghost spice fills */
                               0u;
            if (thr && (gen_next(&r) & 127u) < thr)
                dstep_set(d, 2u, (p16 % 4u == 0u) ? LV_NORM : LV_GHOST, 0u);
        }
        /* Closed hi-hat (lane 4): 8th notes are the baseline; 16ths added with spice */
        {
            uint32_t is_8th = (p16 % 2u == 0u);
            uint32_t thr    = is_8th ? 85u :                            /* 8th notes: usually on */
                              (var > 24u ? var / 2u : 0u);              /* 16th notes: moderate+ spice */
            if (thr && (gen_next(&r) & 127u) < thr)
                dstep_set(d, 4u, LV_NORM, 0u);
        }
        /* Open hi-hat (lane 5): only when no closed hat, very sparse */
        if (!dstep_certain(d, 4u) && (gen_next(&r) & 127u) < (8u + var / 4u))
            dstep_set(d, 5u, LV_NORM, 0u);
        /* Clap (lane 3): light ghost on snare backbeats at moderate spice */
        if ((p16 == 4u || p16 == 12u) && var > 32u && (gen_next(&r) & 127u) < var / 3u)
            dstep_set(d, 3u, LV_GHOST, 0u);
        /* Toms (lanes 9-10): only at higher spice, never on core beats */
        if (var > 48u && p16 != 0u && p16 != 4u && p16 != 8u && p16 != 12u) {
            if ((gen_next(&r) & 127u) < var / 8u)
                dstep_set(d, 9u, LV_GHOST, 0u);
            if ((gen_next(&r) & 127u) < var / 8u)
                dstep_set(d, 10u, LV_GHOST, 0u);
        }
    }
    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(t->dstep, tmp, len * sizeof(dstep_t));
    t->gen_seed = r;
    fm1_irq_on();
}

/* Per-track per-subtype mutation strength set by the ROLL page knobs.
 * 0 = no mutation, 127 = chaos. Stored here; ui_layers.c reads/writes via the helpers. */
static uint8_t mut_str[NTRK][4];
static uint32_t mut_str_get(uint32_t ti, uint32_t st)
{
    return ti < (uint32_t)NTRK ? (uint32_t)mut_str[ti][st & 3u] : 64u;
}
static void mut_str_adj(uint32_t ti, uint32_t st, int32_t delta)
{
    if (ti < (uint32_t)NTRK)
        mut_str[ti][st & 3u] = (uint8_t)clamp((int32_t)mut_str[ti][st & 3u] + delta, 0, 127);
}
/* Linearly interpolate a mutation probability threshold: lo at str=0, hi at str=127.
 * Use inside (rng() & 127u) < MUTHR(lo, hi, str) guards. */
#define MUTHR(lo, hi, s) ((uint32_t)((int32_t)(lo) + (int32_t)((hi)-(lo)) * (int32_t)(s) / 127))

/* Bass note picker: root / fifth / octave weighted, placed in the octave nearest ref. */
static const int8_t BASS_IVLS[8] = {0, 7, 12, 0, 7, 0, 5, 12};
static uint32_t mutate_pick_bass_note(const track_t *t, uint32_t ref)
{
    int32_t root = 60 + t->p[P_ROOT];
    while (root + 6 < (int32_t)ref) root += 12;
    while (root > (int32_t)ref + 6) root -= 12;
    root = clamp(root, 24, 60);
    return (uint32_t)clamp(root + (int32_t)BASS_IVLS[rng() % 8u], 24, 72);
}

/* Partial stochastic mutation: ~50% of steps re-rolled (type, pitch, position, or ratchet).
 * Drum track: protects kick/snare on core beats; prefers hi-hat toggles.
 * Synth track: bass-range tracks prefer root/fifth/octave; adds swap and ratchet ops. */
static void seq_mutate(track_t *t)
{
    uint32_t len = trk_len(t), i;
    uint32_t spice    = (uint32_t)clamp(t->p[P_SPICE], 0, 127);
    uint32_t note_thr = 64u + spice / 4u;
    uint32_t acc_thr  = 28u + spice / 8u;
    uint32_t ref_note = 60u;
    int      is_bass  = 0;
    union { step_t s[NSTEP]; dstep_t d[NSTEP]; } tmp;

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(&tmp, t->step, len * (is_drum(t) ? sizeof(dstep_t) : sizeof(step_t)));
    fm1_irq_on();

    if (!is_drum(t)) {
        /* Compute average pitch of existing notes to calibrate bass detection and note picking */
        uint32_t note_sum = 0u, note_n = 0u;
        for (i = 0; i < len; i++) {
            const step_t *s = &tmp.s[i];
            if (step_fires(s->time) && s->n) { note_sum += s->note[0]; note_n++; }
        }
        if (note_n) ref_note = note_sum / note_n;
        is_bass = (t->p[P_VOICE] != V_POLY) && (ref_note < 60u);
    }

    for (i = 0; i < len; i++) {
        if (rng() & 1u) continue;                       /* 50% of steps untouched */
        if (is_drum(t)) {
            uint32_t p16 = (i * 16u) / len;
            int core = (p16 == 0u || p16 == 4u || p16 == 8u || p16 == 12u);
            if (rng() & 1u) {
                /* Toggle hi-hat: the most natural drum variation */
                dstep_set_hit(&tmp.d[i], 4u, dstep_hit(&tmp.d[i], 4u) == HIT_OFF ? HIT_ON : HIT_OFF);
            } else {
                /* Toggle one non-essential lane; on core beats protect kick and snare */
                uint32_t l = rng() % DRUM_LANES;
                if (core && (l == 0u || l == 2u || l == 4u))
                    l = 3u + (rng() % 13u);             /* redirect to lanes 3..15 */
                dstep_set_hit(&tmp.d[i], l, dstep_hit(&tmp.d[i], l) == HIT_OFF ? HIT_ON : HIT_OFF);
            }
        } else {
            step_t *s = &tmp.s[i];
            switch (rng() % 4u) {
            case 0:                                      /* re-roll step type */
                if ((rng() & 127u) >= note_thr) {
                    s->time = ST_REST; s->n = 0; s->rat = 0;
                } else {
                    s->note[0] = is_bass ? (uint8_t)mutate_pick_bass_note(t, ref_note)
                                         : (uint8_t)mutate_pick_note(t);
                    s->n = 1; s->time = ST_NOTE; s->flags = 0; s->rat = 0;
                    if ((rng() & 127u) < acc_thr) s->flags  = SF_ACCENT;
                    if ((rng() & 127u) < 22u)     s->flags |= SF_SLIDE;
                    if ((rng() & 127u) < 15u)     s->rat    = 1u;  /* x2 ratchet */
                }
                break;
            case 1:                                      /* re-pitch existing note */
                if (s->n)
                    s->note[0] = is_bass ? (uint8_t)mutate_pick_bass_note(t, ref_note)
                                         : (uint8_t)mutate_pick_note(t);
                break;
            case 2: {                                    /* swap with another step */
                uint32_t j = rng() % len;
                step_t swap = tmp.s[j];
                tmp.s[j] = *s;
                *s = swap;
                break;
            }
            default:                                     /* flip ratchet x1 <-> x2 */
                if (s->n && step_fires(s->time))
                    s->rat ^= 1u;
                else if ((rng() & 127u) >= note_thr) {
                    s->time = ST_REST; s->n = 0;
                } else {
                    s->note[0] = is_bass ? (uint8_t)mutate_pick_bass_note(t, ref_note)
                                         : (uint8_t)mutate_pick_note(t);
                    s->n = 1; s->time = ST_NOTE; s->flags = 0;
                }
                break;
            }
        }
    }
    if (!is_drum(t)) {
        for (i = 0; i < len; i++) {
            uint32_t nx = (i + 1u) % len;
            if (flags_is_slide(tmp.s[i].flags) && tmp.s[nx].time == ST_REST)
                tmp.s[i].flags &= ~SF_SLIDE;
        }
    }
    fm1_irq_off();
    memcpy(t->step, &tmp, len * (is_drum(t) ? sizeof(dstep_t) : sizeof(step_t)));
    fm1_irq_on();
}

/* Bass-specific mutation.
 * Rules:
 *   - Beat 1 (p16=0) and beat 3 (p16=8) are anchors — never touched.
 *   - Beat 2/4 (p16=4,12) are strong — re-pitch only (no add/remove), low probability.
 *   - The 16th just before each quarter note (p16=3,7,11,15) is the approach step —
 *     highest chance of change; may gain a slide into the next beat.
 *   - Everything else: moderate probability, can add/remove/repitch.
 *   - Pitch pool: root x4, fifth x3, root+octave x2, third x1.
 *   - Interval constraint: no tritone (6) or large seventh (10-11 semitones) between neighbours.
 */
static void mutate_bass(track_t *t, uint32_t str)
{
    static const uint8_t BASS_W[10] = {0, 0, 0, 0, 4, 4, 4, 99, 99, 2}; /* scale degrees; 99 = root+12 */
    uint32_t len      = trk_len(t), i;
    int32_t  prev     = -1;
    uint32_t bass_root;
    step_t   tmp[NSTEP];

    {
        uint32_t note_sum = 0u, note_n = 0u;
        for (i = 0; i < len; i++) {
            const step_t *s = &t->step[i];
            if (step_fires(s->time) && s->n) { note_sum += s->note[0]; note_n++; }
        }
        uint32_t ref = note_n ? (note_sum / note_n) : 48u;
        int32_t r = 60 + t->p[P_ROOT];
        while (r + 6 < (int32_t)ref) r += 12;
        while (r > (int32_t)ref + 6) r -= 12;
        bass_root = (uint32_t)clamp(r, 24, 60);
    }

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->step, len * sizeof(step_t));
    fm1_irq_on();

    for (i = 0; i < len; i++) {
        uint32_t p16       = (i * 16u) / len;
        step_t  *s         = &tmp[i];
        int      is_anchor  = (p16 == 0u || p16 == 8u);
        int      is_strong  = (p16 == 4u || p16 == 12u);
        int      is_approach= (p16 == 3u || p16 == 7u || p16 == 11u || p16 == 15u);

        if (s->time == ST_TIE) continue;                 /* always preserve ties */

        if (is_anchor) {
            if (step_fires(s->time) && s->n) prev = s->note[0];
            continue;
        }

        uint32_t mut_thr = is_strong   ? MUTHR(0, 22, str)
                         : is_approach ? MUTHR(0, 65, str)
                         :               MUTHR(0, 45, str);

        if ((rng() & 127u) >= mut_thr) {
            if (step_fires(s->time) && s->n) prev = s->note[0];
            continue;
        }

        if (is_strong) {
            /* Re-pitch only; preserve on/off state */
            if (step_fires(s->time) && s->n) {
                uint32_t deg  = BASS_W[rng() % 10u];
                uint32_t note = (deg == 99u) ? (uint32_t)clamp((int32_t)bass_root + 12, 24, 72)
                                             : (uint32_t)clamp((int32_t)scale_up(t, bass_root, deg), 24, 72);
                if (prev >= 0) {
                    uint32_t d = note > (uint32_t)prev ? note - (uint32_t)prev : (uint32_t)prev - note;
                    if (d == 6u || d == 10u || d == 11u) note = bass_root;
                }
                s->note[0] = (uint8_t)note;
                s->flags |= SF_ACCENT;
                prev = (int32_t)note;
            }
        } else {
            /* Add, remove or repitch */
            uint32_t note_thr = is_approach ? MUTHR(40, 90, str) : MUTHR(30, 75, str);
            if ((rng() & 127u) >= note_thr) {
                s->time = ST_REST; s->n = 0; s->flags = 0; s->rat = 0;
            } else {
                uint32_t deg  = BASS_W[rng() % 10u];
                uint32_t note = (deg == 99u) ? (uint32_t)clamp((int32_t)bass_root + 12, 24, 72)
                                             : (uint32_t)clamp((int32_t)scale_up(t, bass_root, deg), 24, 72);
                if (prev >= 0) {
                    uint32_t d = note > (uint32_t)prev ? note - (uint32_t)prev : (uint32_t)prev - note;
                    if (d == 6u || d == 10u || d == 11u) note = bass_root;
                }
                s->note[0] = (uint8_t)note;
                s->n = 1; s->time = ST_NOTE; s->rat = 0;
                s->flags = (is_approach && prev >= 0 && (rng() & 127u) < MUTHR(5, 50, str))
                           ? SF_SLIDE : 0u;
                prev = (int32_t)note;
            }
        }
    }

    for (i = 0; i < len; i++) {
        uint32_t nx = (i + 1u) % len;
        if (flags_is_slide(tmp[i].flags) && tmp[nx].time == ST_REST)
            tmp[i].flags &= ~SF_SLIDE;
    }

    fm1_irq_off();
    memcpy(t->step, tmp, len * sizeof(step_t));
    fm1_irq_on();
}

/* Keys-specific mutation.
 * Detects comp mode (multi-note chord hits) vs arp mode (single-note run) and
 * applies appropriate small variations.
 *
 * Comp mode: repitch chord voicings, add/remove hits on weak beats, thin if too dense.
 * Arp mode: swap neighbours, rest a note, octave-shift, or replace with a chord tone.
 *
 * Chord tones: root, third, fifth, (seventh at high spice) — scale_up degrees 0, 2, 4, 6.
 * Keys range: ~57-96 (well above bass, no more than 1.5 octaves total spread).
 * Anchors (p16=0,8) are never touched.
 */
static void mutate_keys(track_t *t, uint32_t str)
{
    static const uint8_t CDEG[4] = {0, 2, 4, 6};   /* scale degrees: root third fifth seventh */
    uint32_t len      = trk_len(t), i;
    uint32_t n_chord  = str < 32u ? 2u : str < 96u ? 3u : 4u;
    step_t   tmp[NSTEP];

    int32_t keys_root = 60 + t->p[P_ROOT];
    while (keys_root > 72) keys_root -= 12;
    while (keys_root < 60) keys_root += 12;

    /* Detect comp vs arp: comp if more multi-note steps than single-note steps */
    uint32_t multi_n = 0u, single_n = 0u;
    for (i = 0; i < len; i++) {
        if (!step_fires(t->step[i].time)) continue;
        if (t->step[i].n >= 2u) multi_n++; else single_n++;
    }
    int comp_mode = (multi_n >= single_n);

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->step, len * sizeof(step_t));
    fm1_irq_on();

    for (i = 0; i < len; i++) {
        uint32_t p16        = (i * 16u) / len;
        step_t  *s          = &tmp[i];
        int      is_anchor  = (p16 == 0u || p16 == 8u);
        int      is_beat    = (p16 == 4u || p16 == 12u);  /* 2 and 4 */
        int      is_weak16  = !is_anchor && !is_beat && (p16 % 2u != 0u);
        int      is_off8    = !is_anchor && !is_beat && (p16 % 4u == 2u);

        if (s->time == ST_TIE || is_anchor) continue;

        uint32_t mut_thr = is_beat              ? MUTHR(0, 18, str)
                         : (is_weak16||is_off8) ? MUTHR(0, 55, str)
                         :                        MUTHR(0, 38, str);

        if ((rng() & 127u) >= mut_thr) continue;

        if (comp_mode) {
            if (step_fires(s->time) && s->n >= 2u) {
                uint32_t op = rng() % 4u;
                if (op == 0u) {
                    /* Repitch: rebuild voicing at same root, possibly different inversion */
                    int32_t r = keys_root + (int32_t)((rng() % 2u) * 12);
                    r = clamp(r, 57, 84);
                    s->n = 0; s->lvl = 0; s->rat = 0; s->flags = 0;
                    uint32_t k;
                    for (k = 0; k < n_chord; k++) {
                        int32_t note = clamp((int32_t)scale_up(t, (uint32_t)r, CDEG[k]), 57, 96);
                        s->note[s->n++] = (uint8_t)note;
                    }
                } else if (op == 1u) {
                    /* Make conditional — 75% probability hit */
                    s->time = ST_P75;
                } else if (op == 2u) {
                    /* Remove — thin out */
                    s->time = ST_REST; s->n = 0; s->flags = 0; s->rat = 0;
                } else {
                    /* Add accent */
                    s->flags = SF_ACCENT;
                }
            } else if (s->time == ST_REST && (is_weak16 || is_off8)) {
                /* Add chord hit at weak position */
                if ((rng() & 127u) < MUTHR(0, 45, str)) {
                    s->n = 0; s->lvl = 0; s->rat = 0; s->flags = 0;
                    uint32_t k;
                    for (k = 0; k < n_chord; k++) {
                        int32_t note = clamp((int32_t)scale_up(t, (uint32_t)keys_root, CDEG[k]), 57, 96);
                        s->note[s->n++] = (uint8_t)note;
                    }
                    s->time = ST_NOTE;
                }
            }
        } else {
            /* Arp mode: single-note variations */
            if (step_fires(s->time) && s->n >= 1u) {
                uint32_t op = rng() % 4u;
                if (op == 0u) {
                    s->time = ST_REST; s->n = 0; s->flags = 0; s->rat = 0;
                } else if (op == 1u) {
                    /* Step up one scale degree */
                    s->note[0] = (uint8_t)clamp((int32_t)scale_up(t, s->note[0], 1u), 57, 96);
                } else if (op == 2u) {
                    /* Octave shift, stay in range */
                    int32_t shift = (rng() & 1u) ? 12 : -12;
                    int32_t note  = clamp((int32_t)s->note[0] + shift, 57, 96);
                    s->note[0] = (uint8_t)note;
                } else {
                    /* Replace with nearest chord tone */
                    uint32_t deg  = CDEG[rng() % n_chord];
                    int32_t  note = (int32_t)scale_up(t, (uint32_t)keys_root, deg);
                    while (note + 6 < (int32_t)s->note[0]) note += 12;
                    while (note > (int32_t)s->note[0] + 6) note -= 12;
                    s->note[0] = (uint8_t)clamp(note, 57, 96);
                }
            } else if (s->time == ST_REST && is_weak16) {
                /* Add passing chord tone */
                if ((rng() & 127u) < MUTHR(0, 38, str)) {
                    uint32_t deg  = CDEG[rng() % n_chord];
                    int32_t  note = clamp((int32_t)scale_up(t, (uint32_t)keys_root, deg), 57, 96);
                    s->note[0] = (uint8_t)note; s->n = 1;
                    s->time = ST_NOTE; s->flags = 0; s->rat = 0;
                }
            }
        }
    }

    for (i = 0; i < len; i++) {
        uint32_t nx = (i + 1u) % len;
        if (flags_is_slide(tmp[i].flags) && tmp[nx].time == ST_REST)
            tmp[i].flags &= ~SF_SLIDE;
    }

    fm1_irq_off();
    memcpy(t->step, tmp, len * sizeof(step_t));
    fm1_irq_on();
}

/* Pad-specific mutation.
 * Only modifies chord-start steps (ST_NOTE with n>=2); TIE steps are always preserved,
 * keeping note length intact. No accents, slides or ratchets are ever written.
 *
 * Operations (weighted 4:1:1:1:1 in favour of voice movement):
 *   Move one voice by one scale step (up or down)
 *   Add upper color tone (7th or 9th above the existing chord)
 *   Remove top color tone
 *   Suspension: raise or lower the third by one semitone (sus4 / sus2)
 *   Full open-voicing rebuild: root, fifth, upper-third, upper-seventh
 *
 * Range: 48-84 (root/fifth in lower register, thirds/sevenths above).
 */
static void mutate_pad(track_t *t, uint32_t str)
{
    uint32_t len   = trk_len(t), i;
    step_t   tmp[NSTEP];

    int32_t pad_root = 60 + t->p[P_ROOT];
    while (pad_root > 60) pad_root -= 12;
    while (pad_root < 48) pad_root += 12;

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->step, len * sizeof(step_t));
    fm1_irq_on();

    for (i = 0; i < len; i++) {
        step_t  *s = &tmp[i];
        uint32_t k, j;
        if (!step_fires(s->time) || s->n < 2u) continue;

        if ((rng() & 127u) >= MUTHR(0, 50, str)) continue;

        uint32_t op = rng() % 8u;

        if (op < 4u) {
            /* Move one voice up or down by one scale degree */
            uint32_t voice = rng() % s->n;
            int32_t  note;
            if (rng() & 1u) {
                note = (int32_t)scale_up(t, s->note[voice], 1u);
            } else {
                uint32_t mask = scale_mask(t);
                int32_t  m = (int32_t)s->note[voice] - 1;
                uint32_t guard = 12;
                while (guard-- && m >= 24 &&
                       !((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u))
                    m--;
                note = (m >= 24) ? m : (int32_t)s->note[voice];
            }
            note = clamp(note, 48, 84);
            int dup = 0;
            for (k = 0; k < s->n; k++) if (k != voice && s->note[k] == (uint8_t)note) { dup = 1; break; }
            if (!dup) s->note[voice] = (uint8_t)note;

        } else if (op == 4u && s->n < 4u) {
            /* Add color tone: 7th (degree 6) or 9th (degree 8), above the top note */
            uint32_t deg  = (str > 64u && (rng() & 1u)) ? 8u : 6u;
            int32_t  note = (int32_t)scale_up(t, (uint32_t)pad_root, deg);
            int32_t  top  = (int32_t)s->note[s->n - 1u];
            while (note <= top) note += 12;
            note = clamp(note, 48, 96);
            int dup = 0;
            for (k = 0; k < s->n; k++) if (s->note[k] == (uint8_t)note) { dup = 1; break; }
            if (!dup) { s->note[s->n] = (uint8_t)note; s->n++; }

        } else if (op == 5u && s->n > 2u) {
            /* Drop top color tone */
            s->n--;

        } else if (op == 6u) {
            /* Suspension: find the third (by pitch class) and nudge ±1 semitone */
            uint32_t third_pc = scale_up(t, (uint32_t)pad_root, 2u) % 12u;
            for (k = 0; k < s->n; k++) {
                if ((uint32_t)s->note[k] % 12u == third_pc) {
                    int32_t note = clamp((int32_t)s->note[k] + ((rng() & 1u) ? 1 : -1), 48, 84);
                    int dup = 0;
                    for (j = 0; j < s->n; j++) if (j != k && s->note[j] == (uint8_t)note) { dup = 1; break; }
                    if (!dup) s->note[k] = (uint8_t)note;
                    break;
                }
            }

        } else {
            /* Full open-voicing rebuild: root low, fifth low, third+seventh above */
            uint32_t fifth   = (uint32_t)clamp((int32_t)scale_up(t, (uint32_t)pad_root, 4u), 48, 84);
            uint32_t third_h = (uint32_t)clamp((int32_t)scale_up(t, (uint32_t)(pad_root + 12), 2u), 57, 96);
            uint32_t sev_h   = (uint32_t)clamp((int32_t)scale_up(t, (uint32_t)(pad_root + 12), 6u), 57, 96);
            s->note[0] = (uint8_t)pad_root;
            s->note[1] = (uint8_t)fifth;
            s->n = 2u;
            if (str >= 16u && third_h != fifth)  s->note[s->n++] = (uint8_t)third_h;
            if (str >= 64u && sev_h != third_h)  s->note[s->n++] = (uint8_t)sev_h;
        }

        s->flags = 0; s->rat = 0;    /* pads never have accents, slides or ratchets */
    }

    fm1_irq_off();
    memcpy(t->step, tmp, len * sizeof(step_t));
    fm1_irq_on();
}

/* Lead-specific mutation.
 * Small, melodically coherent edits: monophonic, range-bounded, no ratchets.
 *
 * Operations (5:2:1:1:1 weighting): nudge note one scale degree; drop to rest;
 * swap with next step; add passing note; make ornament optional (ST_P75).
 *
 * Post-process:
 *   - Fix runs of 3+ same-direction leaps (pull the third leap back one scale step).
 *   - Add occasional slides (SF_SLD75) where the next note is within a third.
 *   - Anchor steps (p16=0,8) are never touched.
 */
static void mutate_lead(track_t *t, uint32_t str)
{
    uint32_t len   = trk_len(t), i;
    step_t   tmp[NSTEP];

    /* Range: ±10 semitones around the existing melodic center */
    uint32_t note_sum = 0u, note_n = 0u;
    for (i = 0; i < len; i++) {
        const step_t *s = &t->step[i];
        if (step_fires(s->time) && s->n) { note_sum += s->note[0]; note_n++; }
    }
    int32_t center = note_n ? (int32_t)(note_sum / note_n) : 72;
    int32_t lo     = clamp(center - 10, 36, 96);
    int32_t hi     = clamp(center + 10, 36, 96);

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->step, len * sizeof(step_t));
    fm1_irq_on();

    for (i = 0; i < len; i++) {
        uint32_t p16       = (i * 16u) / len;
        step_t  *s         = &tmp[i];
        int      is_anchor = (p16 == 0u || p16 == 8u);
        int      is_strong = (p16 == 4u || p16 == 12u);
        int      is_weak   = !is_anchor && !is_strong && (p16 % 2u != 0u);

        if (s->time == ST_TIE) continue;
        s->rat = 0;                   /* leads never ratchet */
        if (s->n > 1u) s->n = 1u;    /* enforce monophonic */
        if (is_anchor) continue;

        uint32_t mut_thr = is_strong ? MUTHR(0, 18, str)
                         : is_weak   ? MUTHR(0, 60, str)
                         :             MUTHR(0, 40, str);
        if ((rng() & 127u) >= mut_thr) continue;

        uint32_t op = rng() % 10u;

        if (op < 5u && step_fires(s->time) && s->n == 1u) {
            /* Nudge one scale degree */
            int32_t note;
            if (rng() & 1u) {
                note = (int32_t)scale_up(t, s->note[0], 1u);
            } else {
                uint32_t mask = scale_mask(t);
                int32_t  m    = (int32_t)s->note[0] - 1;
                uint32_t guard = 12;
                while (guard-- && m >= lo &&
                       !((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u))
                    m--;
                note = (m >= lo) ? m : (int32_t)s->note[0];
            }
            s->note[0] = (uint8_t)clamp(note, lo, hi);

        } else if (op < 7u && step_fires(s->time) && !is_strong) {
            /* Drop to rest */
            s->time = ST_REST; s->n = 0; s->flags = 0;

        } else if (op < 8u && i + 1u < len && tmp[i + 1u].time != ST_TIE) {
            /* Swap with next step */
            step_t nx    = tmp[i + 1u];
            tmp[i + 1u] = *s;
            *s           = nx;

        } else if (op < 9u && s->time == ST_REST) {
            /* Add a passing note in-scale */
            uint32_t mask = scale_mask(t), deg[12], ndeg = 0, k;
            for (k = 0; k < 12u; k++)
                if ((mask >> k) & 1u) deg[ndeg++] = k;
            if (!ndeg) { ndeg = 1; deg[0] = 0; }
            int32_t note = (int32_t)(60u + (uint32_t)t->p[P_ROOT] + deg[rng() % ndeg]);
            while (note < lo) note += 12;
            while (note > hi) note -= 12;
            s->note[0] = (uint8_t)clamp(note, lo, hi);
            s->n = 1; s->time = ST_NOTE; s->flags = 0;

        } else if (step_fires(s->time) && s->n == 1u) {
            /* Make ornament optional */
            s->time = ST_P75;
        }
    }

    /* Post-process 1: fix 3+ same-direction leaps */
    {
        uint32_t nidx[NSTEP], np = 0u;
        for (i = 0; i < len; i++)
            if (step_fires(tmp[i].time) && tmp[i].n) nidx[np++] = i;
        if (np >= 3u) {
            uint32_t k;
            for (k = 0; k + 2u < np; k++) {
                int32_t p0 = (int32_t)tmp[nidx[k]].note[0];
                int32_t p1 = (int32_t)tmp[nidx[k+1u]].note[0];
                int32_t p2 = (int32_t)tmp[nidx[k+2u]].note[0];
                int32_t d01 = p1 - p0, d12 = p2 - p1;
                if ((d01 > 3 && d12 > 3) || (d01 < -3 && d12 < -3)) {
                    int32_t note;
                    if (d12 > 0) {
                        /* third leap up: step p2 back down toward p1 */
                        uint32_t mask = scale_mask(t);
                        int32_t  m    = p2 - 1;
                        uint32_t guard = 12;
                        while (guard-- && m >= lo &&
                               !((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u))
                            m--;
                        note = (m >= lo) ? m : p2;
                    } else {
                        /* third leap down: step p2 back up */
                        note = (int32_t)scale_up(t, (uint32_t)p2, 1u);
                    }
                    tmp[nidx[k+2u]].note[0] = (uint8_t)clamp(note, lo, hi);
                }
            }
        }
    }

    /* Post-process 2: occasional slides on close intervals, clean up stranded slides */
    for (i = 0; i < len; i++) {
        if (!step_fires(tmp[i].time) || !tmp[i].n) continue;
        uint32_t nx = (i + 1u) % len;
        if (step_fires(tmp[nx].time) && tmp[nx].n) {
            int32_t d = (int32_t)tmp[nx].note[0] - (int32_t)tmp[i].note[0];
            if (d < 0) d = -d;
            if (d > 0 && d <= 4 && (rng() & 127u) < MUTHR(3, 35, str))
                tmp[i].flags = SF_SLD75;
        }
        if (flags_is_slide(tmp[i].flags) && tmp[nx].time == ST_REST)
            tmp[i].flags &= ~SF_SLIDE;
    }

    fm1_irq_off();
    memcpy(t->step, tmp, len * sizeof(step_t));
    fm1_irq_on();
}

/* ---- Drum mutation: KICK ---------------------------------------------------
 * Anchor: beat 1 (p16=0) always loud certain. Semi-anchor: beat 3 (p16=8) often present.
 * Backbeats (p16=4,12): always cleared (snare territory).
 * Syncopated positions (p16=3,6,9,10,11,14): optional ghost/pickup kicks; adjacency-limited.
 * All other positions: remove stray kicks at moderate probability. */
static void mutate_drum_kick(track_t *t, uint32_t str)
{
    uint32_t len   = trk_len(t), i;
    dstep_t  tmp[NSTEP];

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->dstep, len * sizeof(dstep_t));
    fm1_irq_on();

    for (i = 0u; i < len; i++) {
        uint32_t p16  = (i * 16u) / len;
        dstep_t *d    = &tmp[i];
        int has_k     = dstep_has(d, 0u) || dstep_has(d, 1u);
        uint32_t lane = dstep_has(d, 0u) ? 0u : 1u;

        if (p16 == 0u) {
            dstep_set(d, 0u, LV_HARD, 0u);
            continue;
        }
        if (p16 == 4u || p16 == 12u) {
            dstep_clr(d, 0u); dstep_clr(d, 1u);
            continue;
        }
        if (p16 == 8u) {
            if (has_k) {
                if ((rng() & 127u) < MUTHR(0, 22, str))
                    dstep_set_prb(d, lane, 1u);
            } else if ((rng() & 127u) < MUTHR(0, 40, str)) {
                dstep_set(d, 0u, LV_NORM, 0u);
            }
            continue;
        }
        if (p16 == 3u  || p16 == 6u  || p16 == 9u  ||
            p16 == 10u || p16 == 11u || p16 == 14u) {
            if (has_k) {
                if ((rng() & 127u) < MUTHR(0, 32, str)) {
                    switch (rng() % 3u) {
                        case 0u: dstep_clr(d, lane);          break;
                        case 1u: dstep_set_prb(d, lane, 1u);  break;
                        default: dstep_set_prb(d, lane, 2u);  break;
                    }
                }
            } else if ((rng() & 127u) < MUTHR(0, 42, str)) {
                int l1 = (i > 0u)     && (dstep_has(&tmp[i-1u], 0u) || dstep_has(&tmp[i-1u], 1u));
                int l2 = (i > 1u)     && (dstep_has(&tmp[i-2u], 0u) || dstep_has(&tmp[i-2u], 1u));
                int r1 = (i+1u < len) && (dstep_has(&tmp[i+1u], 0u) || dstep_has(&tmp[i+1u], 1u));
                int r2 = (i+2u < len) && (dstep_has(&tmp[i+2u], 0u) || dstep_has(&tmp[i+2u], 1u));
                if (!(l1 && l2) && !(r1 && r2) && !(l1 && r1))
                    dstep_set_prb(d, 0u, 1u);
            }
            continue;
        }
        if (has_k && (rng() & 127u) < MUTHR(0, 30, str)) {
            dstep_clr(d, 0u); dstep_clr(d, 1u);
        }
    }

    fm1_irq_off();
    memcpy(t->dstep, tmp, len * sizeof(dstep_t));
    fm1_irq_on();
}

/* ---- Drum mutation: CYM ----------------------------------------------------
 * Low str: accent-velocity tweaks, conditional 16ths, open-hat placement.
 * High str: full hat pattern repaint (8th / 16th / off-beat / Charleston / sparse);
 *           ride substitution on beats; wider open-hat placement. */
static void mutate_drum_cym(track_t *t, uint32_t str)
{
    uint32_t len   = trk_len(t), i;
    dstep_t  tmp[NSTEP];
    uint32_t hat_n = 0u;

    for (i = 0u; i < len; i++)
        if (dstep_has(&t->dstep[i], 4u)) hat_n++;
    int was16 = (hat_n * 2u >= len);

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->dstep, len * sizeof(dstep_t));
    fm1_irq_on();

    /* High-strength: full repaint with a new hat pattern style */
    if (str > 64u && (rng() & 127u) < (str - 64u) * 2u) {
        for (i = 0u; i < len; i++) { dstep_clr(&tmp[i], 4u); dstep_clr(&tmp[i], 5u); }
        uint32_t style = rng() % 5u;
        for (i = 0u; i < len; i++) {
            uint32_t p16 = (i * 16u) / len;
            int is_beat  = (p16 == 0u || p16 == 4u || p16 == 8u || p16 == 12u);
            int is_and   = (p16 == 2u || p16 == 6u || p16 == 10u || p16 == 14u);
            int on;
            switch (style) {
                case 0:  on = (p16 % 2u == 0u); break;   /* 8ths */
                case 1:  on = 1;                 break;   /* 16ths */
                case 2:  on = (p16 % 2u != 0u); break;   /* off-beats */
                case 3:  on = (p16 == 0u || p16 == 6u || p16 == 8u || p16 == 14u); break; /* Charleston */
                default: on = (p16 % 4u == 0u); break;   /* quarter-note hats */
            }
            if (!on) continue;
            uint32_t lvl = is_beat ? LV_HARD : (is_and ? LV_NORM : LV_SOFT);
            if (!is_beat && !is_and && (rng() & 3u))
                dstep_set_prb(&tmp[i], 4u, 1u + (rng() & 1u));
            else
                dstep_set(&tmp[i], 4u, lvl, 0u);
        }
    } else {
        /* Individual hit pass */
        for (i = 0u; i < len; i++) {
            uint32_t p16 = (i * 16u) / len;
            dstep_t *d   = &tmp[i];
            int is_beat  = (p16 == 0u || p16 == 4u || p16 == 8u || p16 == 12u);
            int is_and   = (p16 == 2u || p16 == 6u || p16 == 10u || p16 == 14u);

            uint32_t thr = is_beat ? MUTHR(2, 20, str)
                         : is_and  ? MUTHR(5, 55, str)
                         :           MUTHR(10, 88, str);
            if ((rng() & 127u) >= thr) continue;

            if (dstep_has(d, 4u)) {
                if (!is_beat && !is_and) {
                    if (str > 50u && (rng() & 1u))
                        dstep_clr(d, 4u);
                    else
                        dstep_set_prb(d, 4u, 1u + (rng() & 1u));
                } else {
                    dstep_set(d, 4u, is_beat ? LV_HARD : LV_NORM, dstep_rat(d, 4u));
                }
            } else if (is_beat || is_and) {
                dstep_set(d, 4u, is_beat ? LV_HARD : LV_NORM, 0u);
            } else if (was16 || str > 40u) {
                if ((rng() & 127u) < MUTHR(2, 30, str))
                    dstep_set(d, 4u, LV_SOFT, 0u);
            }
        }
    }

    /* Open hat: best at and-of-2/4 (p16=6,7,14,15); wider at higher str */
    for (i = 0u; i < len; i++) {
        uint32_t p16 = (i * 16u) / len;
        dstep_t *d   = &tmp[i];
        int is_good  = (p16 == 6u || p16 == 7u || p16 == 14u || p16 == 15u);
        int is_ok    = (str > 50u) && (p16 == 2u || p16 == 10u);

        if (is_good || is_ok) {
            if (dstep_has(d, 5u)) {
                if ((rng() & 127u) < MUTHR(5, 25, str)) dstep_clr(d, 5u);
            } else if (!dstep_has(d, 4u)) {
                int prev = (i > 0u)     && dstep_has(&tmp[i-1u], 5u);
                int next = (i+1u < len) && dstep_has(&tmp[i+1u], 5u);
                if (!prev && !next && (rng() & 127u) < MUTHR(3, 48, str))
                    dstep_set(d, 5u, LV_NORM, 0u);
            }
        } else if (dstep_has(d, 5u)) {
            dstep_clr(d, 5u);
        }
        if (dstep_has(d, 4u) && dstep_has(d, 5u)) dstep_clr(d, 5u);
    }

    /* High-str: swap some beat hats to ride (lane 12) for variety */
    if (str > 70u) {
        for (i = 0u; i < len; i++) {
            uint32_t p16 = (i * 16u) / len;
            int is_beat  = (p16 == 0u || p16 == 4u || p16 == 8u || p16 == 12u);
            if (is_beat && dstep_has(&tmp[i], 4u) &&
                (rng() & 127u) < (str - 70u) / 2u) {
                dstep_clr(&tmp[i], 4u);
                dstep_set(&tmp[i], 12u, LV_NORM, 0u);
            }
        }
    }

    fm1_irq_off();
    memcpy(t->dstep, tmp, len * sizeof(dstep_t));
    fm1_irq_on();
}

/* ---- Drum mutation: SN -----------------------------------------------------
 * Backbeat snare (lane 2, p16=4,12): locked as loud certain hits.
 * Ghost snare: very soft certain hits at off-16ths; prefer kick-free steps; no
 * adjacent ghosts. Click/clap (lane 3): sparse ghost colour on weak 16ths. */
static void mutate_drum_sn(track_t *t, uint32_t str)
{
    uint32_t len   = trk_len(t), i;
    dstep_t  tmp[NSTEP];

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->dstep, len * sizeof(dstep_t));
    fm1_irq_on();

    for (i = 0u; i < len; i++) {
        uint32_t p16 = (i * 16u) / len;
        if (p16 == 4u || p16 == 12u)
            dstep_set(&tmp[i], 2u, LV_HARD, 0u);
    }

    for (i = 0u; i < len; i++) {
        uint32_t p16     = (i * 16u) / len;
        dstep_t *d       = &tmp[i];
        int is_back      = (p16 == 4u || p16 == 12u);
        int is_kick      = dstep_has(d, 0u) || dstep_has(d, 1u);
        int is_ghost_pos = (p16 == 3u  || p16 == 5u  || p16 == 6u  || p16 == 7u  ||
                            p16 == 9u  || p16 == 10u || p16 == 11u ||
                            p16 == 13u || p16 == 14u || p16 == 15u);

        if (is_back) continue;

        uint32_t mut_thr = is_ghost_pos ? MUTHR(0, 55, str) : MUTHR(0, 20, str);
        if ((rng() & 127u) >= mut_thr) continue;

        if (is_ghost_pos && !dstep_certain(d, 2u)) {
            if (dstep_has(d, 2u)) {
                if ((rng() & 3u) == 0u) dstep_clr(d, 2u);
            } else {
                int prev_sn = (i > 0u)     && dstep_has(&tmp[i-1u], 2u);
                int next_sn = (i+1u < len) && dstep_has(&tmp[i+1u], 2u);
                if (!prev_sn && !next_sn) {
                    uint32_t add_thr = is_kick ? MUTHR(0, 18, str) : MUTHR(0, 45, str);
                    if ((rng() & 127u) < add_thr)
                        dstep_set(d, 2u, LV_GHOST, 0u);
                }
            }
        } else if (!is_ghost_pos && !is_back && dstep_has(d, 2u) && !dstep_certain(d, 2u)) {
            if ((rng() & 127u) < 30u) dstep_clr(d, 2u);
        }

        if (is_ghost_pos && !is_kick && !dstep_has(d, 2u) &&
            (rng() & 127u) < MUTHR(0, 22, str)) {
            if (dstep_has(d, 3u))
                dstep_clr(d, 3u);
            else
                dstep_set(d, 3u, LV_GHOST, 0u);
        }
    }

    fm1_irq_off();
    memcpy(t->dstep, tmp, len * sizeof(dstep_t));
    fm1_irq_on();
}

/* ---- Drum mutation: ACC ----------------------------------------------------
 * Percussion: shaker(13), conga(14), cowbell(15) for repeating patterns.
 * Toms (9,10): fills reserved for last quarter (p16>=12).
 * Never on snare backbeats; max one accent lane per step; most hits probabilistic. */
static void mutate_drum_acc(track_t *t, uint32_t str)
{
    static const uint8_t PERC[3] = {13u, 14u, 15u};
    uint32_t len   = trk_len(t), i, li;
    dstep_t  tmp[NSTEP];

    fm1_irq_off();
    undo_mark(t, undo_sess++);
    memcpy(tmp, t->dstep, len * sizeof(dstep_t));
    fm1_irq_on();

    for (i = 0u; i < len; i++) {
        uint32_t p16    = (i * 16u) / len;
        dstep_t *d      = &tmp[i];
        int is_sn_back  = (p16 == 4u || p16 == 12u);
        int is_fill_bar = (p16 >= 12u);

        if (is_fill_bar) {
            uint32_t tom = (p16 <= 13u) ? 10u : 9u;
            if ((rng() & 127u) < MUTHR(0, 32, str)) {
                if (dstep_has(d, tom)) dstep_clr(d, tom);
                else                   dstep_set(d, tom, LV_NORM, 0u);
            }
            continue;
        }

        if (is_sn_back) continue;

        for (li = 0u; li < 3u; li++) {
            uint32_t lane     = PERC[li];
            uint32_t base_thr = (li == 0u) ? MUTHR(0, 40, str)   /* shaker: most frequent */
                              : (li == 1u) ? MUTHR(0, 28, str)   /* conga */
                              :              MUTHR(0, 18, str);  /* cowbell: sparsest */
            if ((rng() & 127u) >= base_thr) continue;

            if (dstep_has(d, lane)) {
                switch (rng() % 3u) {
                    case 0u: dstep_clr(d, lane);         break;
                    case 1u: dstep_set_prb(d, lane, 2u); break;
                    default: break;
                }
            } else {
                int any_acc = 0;
                uint32_t al;
                for (al = 0u; al < 3u; al++)
                    if (dstep_has(d, PERC[al])) { any_acc = 1; break; }
                if (!any_acc) {
                    if ((rng() & 3u) == 0u)
                        dstep_set(d, lane, LV_NORM, 0u);
                    else
                        dstep_set_prb(d, lane, 1u);
                }
            }
        }
    }

    fm1_irq_off();
    memcpy(t->dstep, tmp, len * sizeof(dstep_t));
    fm1_irq_on();
}

/* Typed mutation — each type will get its own algorithm.
 * Melodic types: 0=LEAD 1=BASS 2=KEYS 3=PAD.  Drum types: 0=KICK 1=SN 2=CYM 3=ACC. */
static void seq_mutate_type(track_t *t, uint8_t type)
{
    uint32_t ti  = (uint32_t)(t - trk);
    uint32_t str = mut_str_get(ti, (uint32_t)type);
    if (!is_drum(t) && type == 0u) { mutate_lead(t, str); return; }
    if (!is_drum(t) && type == 1u) { mutate_bass(t, str); return; }
    if (!is_drum(t) && type == 2u) { mutate_keys(t, str); return; }
    if (!is_drum(t) && type == 3u) { mutate_pad(t, str);  return; }
    if ( is_drum(t) && type == 0u) { mutate_drum_kick(t, str); return; }
    if ( is_drum(t) && type == 1u) { mutate_drum_sn(t, str);   return; }
    if ( is_drum(t) && type == 2u) { mutate_drum_cym(t, str);  return; }
    if ( is_drum(t) && type == 3u) { mutate_drum_acc(t, str);  return; }
    seq_mutate(t);
}

/* ---------------------------------------------------------- evolve --- */
/* Call once per UI frame (main context): runs seq_mutate_type on any (track, subtype) whose EVOLVE interval fired.
 * P_EVOLVE packs four 2-bit subtype rates: bits[2k+1:2k] = subtype k rate (0=off 1=4bar 2=8bar 3=16bar). */
static void seq_evl_process(void)
{
    uint32_t i, st, req;
    fm1_irq_off();
    req = (uint32_t)evl_req;
    evl_req = 0;
    fm1_irq_on();
    for (i = 0; i < NTRK; i++)
        for (st = 0; st < 4u; st++)
            if ((req >> (i * 4u + st)) & 1u)
                seq_mutate_type(&trk[i], (uint8_t)st);
}

/* ------------------------------------------------------- recording --- */
/* key to ear, in samples: the key's debounce (~3 ms) and the audio out buffer (HALF_FRAMES to
 * 2 x HALF_FRAMES, ~9 ms on average). A note played in time with what the player hears reaches
 * the sequencer this much later than the sound it was played to: recording takes it back. */
#define REC_LAT 512u

/* the step a note played now goes into (as heard: REC_LAT earlier): the one playing, or the next one
 * when it is past the middle of the playing one; *later: it has not played yet (it must not sound twice) */
static uint32_t rec_target(const track_t *t, uint32_t *later)
{
    uint32_t into, slen, abs = trk_grid(t, &into, &slen), half = slen / 2u, lat = REC_LAT * (uint32_t)song.g[G_BPM];
    if (into > half + (lat < half ? lat : half))
        abs++;
    *later = abs != t->seq_abs;
    return abs;
}

/* note into synth step idx (overdub: a step that holds notes gets this one added, a chord of up to 4;
 * when full, the last note is replaced; MONO / LEGATO / UNISON parts keep one note per step) */
static void step_add(track_t *t, uint32_t idx, uint32_t note, uint32_t vel, uint32_t lvl, uint32_t rat)
{
    step_t *s = &t->step[idx];
    uint32_t k;
    if (s->time != ST_NOTE || !s->n || t->p[P_VOICE] != V_POLY) {
        s->n = 0;                                   /* a fresh step */
        s->flags = 0;
        s->vel = 0;
        s->lvl = 0;
        s->rat = 0;
    }
    for (k = 0; k < s->n && s->note[k] != note; k++)
        ;
    if (k == s->n) {
        if (s->n < 4u)
            s->n++;
        k = s->n - 1u;
        s->note[k] = (uint8_t)note;
    }
    s->lvl = (uint8_t)((s->lvl & ~(3u << (2u * k))) | (lvl & 3u) << (2u * k));
    s->rat = (uint8_t)((s->rat & ~(3u << (2u * k))) | (rat & 3u) << (2u * k));
    s->time = ST_NOTE;
    if (vel > s->vel)
        s->vel = (uint8_t)vel;
}

/* live recording into a synth part: the nearest step (rec_target). Held on: each further step the
 * sequencer enters while the note is held becomes a TIE (rec_hold), up to the pattern length and
 * never over a step with notes (overdub keeps them); a release before the middle of the last one
 * puts that step back (rec_release), so a short note stays one step. A note recorded into another
 * step ends the hold before (the step model ties the notes of one step only). */
static void rec_note(track_t *t, uint32_t note, uint32_t vel, uint32_t rat, int hold)
{
    uint32_t len = trk_len(t), later, abs = rec_target(t, &later), idx = abs % len, k;
    undo_mark(t, UNDO_REC(t));
    step_add(t, idx, note, vel, vel_lvl(vel), rat);
    t->seq_active = 1;
    if (later) {                                    /* it sounds now: the step must not trigger it again */
        if (t->rskip_abs != abs)
            t->rskip_n = 0;
        t->rskip_abs = abs;
        if (t->rskip_n < 4u)
            t->rskip[t->rskip_n++] = (uint8_t)note;
    }
    if (!hold)
        return;
    if (!t->rh_n || t->rh_start != idx) {           /* a new hold (one in another step ends) */
        t->rh_n = 0;
        t->rh_start = (uint8_t)idx;
        t->rh_ties = 0;
    }
    for (k = 0; k < t->rh_n && t->rh_note[k] != note; k++)
        ;
    if (k == t->rh_n && t->rh_n < 4u)
        t->rh_note[t->rh_n++] = (uint8_t)note;      /* a chord: held until its last key is up */
}

/* live recording into the drum track: lane, level, ratchet */
static void rec_hit(track_t *t, uint32_t lane, uint32_t lvl, uint32_t rat)
{
    uint32_t later, abs = rec_target(t, &later);
    undo_mark(t, UNDO_REC(t));
    dstep_set(&t->dstep[abs % trk_len(t)], lane, lvl, rat);
    t->seq_active = 1;
    if (later) {
        if (t->rskip_abs != abs)
            t->rskip_lanes = 0;
        t->rskip_abs = abs;
        t->rskip_lanes |= (uint16_t)(1u << lane);
    }
}

/* the sequencer enters step idx (before playing it): a recorded note still held ties into it */
static void rec_hold(track_t *t, uint32_t idx, uint32_t len, uint32_t abs)
{
    step_t *s;
    if (!t->rh_n)
        return;
    if (!((song.rec >> trk_index(t)) & 1u) || t->rh_ties + 1u >= len) {
        t->rh_n = 0;                                /* disarmed, or the whole pattern is this note */
        return;
    }
    if (idx == t->rh_start)
        return;                                     /* (recorded ahead into the step now starting) */
    s = &t->step[idx];
    if (step_fires(s->time) && s->n) {
        t->rh_n = 0;                                /* a step with notes: the hold ends before it */
        return;
    }
    t->rh_bak = *s;
    t->rh_last = (uint8_t)idx;
    t->rh_last_abs = abs;
    t->rh_ties++;
    memset(s, 0, sizeof *s);
    s->time = ST_TIE;
}

/* a key of a recorded note is up: the hold ends with the last one */
static void rec_release(track_t *t, uint32_t note)
{
    uint32_t i, k = 0, into, slen, half, lat;
    for (i = 0; i < t->rh_n; i++)
        if (t->rh_note[i] != note)
            t->rh_note[k++] = t->rh_note[i];
    if (k == t->rh_n || (t->rh_n = (uint8_t)k))
        return;                                     /* not one of them, or others still held */
    if (!t->rh_ties) {
        t->rh_n = 0;
        return;
    }
    if (trk_grid(t, &into, &slen) != t->rh_last_abs)
        return;
    half = slen / 2u;
    lat = REC_LAT * (uint32_t)song.g[G_BPM];
    if (into <= half + (lat < half ? lat : half) && t->step[t->rh_last].time == ST_TIE)
        t->step[t->rh_last] = t->rh_bak;            /* released early in it: not held into this step */
}

/* LIVE recording, no click needed (REC: ui_input.c rec_toggle).
 *  playing: REC records the selected track at once (song.rec), quantised, overdub.
 *  stopped: REC arms (rec_wait). The first note played on the selected track:
 *   - a project with notes: starts the transport, that note is step 1, recording on.
 *     PLAY while armed starts the transport and the recording together.
 *   - an empty project: a FREE TAKE (ft_on). Play freely, as long as you like: no tempo, no
 *     grid. REC on the next downbeat closes the loop: its length sets the tempo (1, 2 or 4
 *     bars, the nearest the current tempo; within 3 % of it the tempo is kept), the notes are
 *     quantised to 1/16 into it with their lengths, and the loop plays on. PLAY drops the take.
 *  The REC screen (ui_studio.c) sets how (settings of the FM-1, panel.c lights_word):
 *   KNOB 1 MODE (an empty project): FREE (the free take above) or TEMPO (record at the tempo set,
 *          as in a project with notes);
 *   KNOB 3 START (TEMPO, or a project with notes): NOTE (the first note starts the loop, as above)
 *          or COUNT (PLAY clicks one bar, 4 beats, then the loop and the recording start; notes
 *          played meanwhile only sound). */
static volatile uint8_t rec_wait;             /* 1: armed, waits for a note */
static uint8_t rec_tempo;                     /* REC screen MODE: 0 FREE, 1 TEMPO (an empty project) */
static uint8_t rec_count;                     /* REC screen START: 0 NOTE, 1 COUNT (one bar of clicks) */
static volatile uint8_t ci_on;                /* the count-in runs (armed, COUNT, PLAY) */
static volatile uint8_t ci_beat;              /* its beats clicked so far - 1 (the UI shows 4 - ci_beat) */
static uint32_t ci_u;                         /* clock units since it started */
static volatile uint8_t rec_go;               /* recording just started (the UI says so) */
static void seq_start(void);
static void rec_begin(void)
{
    song.rec = (uint8_t)(1u << (song.sel % NTRK));
    rec_wait = 0;
    rec_go = 1;
}

#define FT_MAX 192u
#define FT_BLOCKS (24u * FS / CTL)            /* 24 s: 4 bars at 40 BPM, the longest loop */
#define FT_OPEN 0xFFFFu                       /* the key is still down */
typedef struct { uint16_t t, d; uint8_t note, vel; } ft_ev_t;   /* in blocks from the first note; drums: lane, level */
static ft_ev_t ft_ev[FT_MAX];
static uint32_t ft_n;
static volatile uint8_t ft_on;                /* a free take runs */
static uint8_t ft_trk;
static volatile uint32_t ft_t;                /* blocks since its first note */
static volatile uint32_t ft_btn_mask;         /* the REC button (the UI sets it): closes the take */
static volatile uint32_t ft_drop_mask;        /* the PLAY button: drops it */
static uint32_t ft_btn_prev;
static volatile uint8_t ft_bars;              /* the loop just closed: bars (the UI says so), 0xFF dropped */
static volatile uint32_t ft_close_ms;         /* when (the UI drops the press that closed it) */
static volatile uint8_t ft_closed;            /* (ft_close_ms is set) */

static int track_empty(const track_t *t)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++)
        if (is_drum(t) ? dstep_full_mask(&t->dstep[k]) != 0u : step_fires(t->step[k].time) && t->step[k].n)
            return 0;
    return 1;
}
static int project_empty(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (!track_empty(&trk[i]))
            return 0;
    return 1;
}
static void locks_clear(track_t *t);           /* forward: defined after the lock data structures */
static void steps_clear(track_t *t)           /* an empty pattern (synth: REST steps, drums: no lane); no lock, no nudge, no condition */
{
    uint32_t k;
    memset(t->step, 0, sizeof t->step);
    memset(t->fill, 0, sizeof t->fill);
    locks_clear(t);
    if (!is_drum(t))
        for (k = 0; k < NSTEP; k++)
            t->step[k].time = ST_REST;
}

/* tempo x 10 of a loop of T blocks holding n bars of 4/4 */
static uint32_t ft_bpm10(uint32_t T, uint32_t n)
{
    uint32_t den = T * CTL;                       /* (n * 2400 * FS < 2^32 up to n = 4) */
    return den ? (n * 2400u * FS + den / 2u) / den : 0u;
}

/* the bars a loop of T blocks holds (1, 2 or 4: the tempo the nearest the current one, in
 * 40..240), 0 none; *bpm its tempo */
static uint32_t ft_fit(uint32_t T, uint32_t *bpm)
{
    static const uint8_t BARS[3] = {1, 2, 4};
    uint32_t i, best = 0, cur = (uint32_t)song.g[G_BPM] * 10u, err = 0xFFFFFFFFu;
    for (i = 0; i < 3u; i++) {
        uint32_t b = ft_bpm10(T, BARS[i]), e;
        if (b < 400u || b > 2400u)
            continue;
        e = b > cur ? b * 1000u / cur : cur * 1000u / b;   /* the ratio, x 1000 */
        if (e < err) {
            err = e;
            best = BARS[i];
            *bpm = (b + 5u) / 10u;
            if (e <= 1030u)
                *bpm = cur / 10u;                           /* played to the tempo set: keep it */
        }
    }
    return best;
}

static void ft_start(track_t *t)
{
    ft_on = 1;
    ft_trk = (uint8_t)trk_index(t);
    ft_t = 0;
    ft_n = 0;
    rec_wait = 0;
}

static void ft_note_on(uint32_t note, uint32_t vel)
{
    if (ft_n < FT_MAX && ft_t < FT_BLOCKS) {
        ft_ev[ft_n].t = (uint16_t)ft_t;
        ft_ev[ft_n].d = FT_OPEN;
        ft_ev[ft_n].note = (uint8_t)note;
        ft_ev[ft_n].vel = (uint8_t)vel;
        ft_n++;
    }
}

static void ft_note_off(uint32_t note)
{
    uint32_t i = ft_n;
    while (i--)
        if (ft_ev[i].note == note && ft_ev[i].d == FT_OPEN) {
            ft_ev[i].d = (uint16_t)(ft_t - ft_ev[i].t);
            return;
        }
}

/* REC on the downbeat: the loop is the time from the first note to now */
static void ft_close(void)
{
    track_t *t = &trk[ft_trk % NTRK];
    uint32_t T = ft_t, bars, bpm = 0, len, i, k;
    ft_on = 0;
    ft_close_ms = fm1_ms;
    ft_closed = 1;
    bars = ft_n && T >= FS / CTL / 2u ? ft_fit(T, &bpm) : 0u;
    if (!bars) {
        ft_bars = 0xFF;                               /* nothing played, or no tempo fits */
        return;
    }
    len = 16u * bars;
    undo_mark(t, (undo_sess += 4u) | 3u);             /* (undo: back to the empty project) */
    steps_clear(t);                                   /* (no tie left over from an old pattern) */
    for (i = 0; i < NTRK; i++) {                      /* the project is empty: one loop length */
        trk[i].p[P_SLEN] = (int16_t)len;
        trk[i].p[P_SDIV] = 2;                         /* 1/16 */
    }
    for (i = 0; i < ft_n; i++) {                      /* the notes, to the nearest step */
        uint32_t idx = (ft_ev[i].t * len * 2u + T) / (2u * T) % len;
        if (is_drum(t))
            dstep_set(&t->dstep[idx], ft_ev[i].note & 15u, ft_ev[i].vel & 3u, 0);
        else
            step_add(t, idx, ft_ev[i].note, ft_ev[i].vel, vel_lvl(ft_ev[i].vel), 0);
    }
    if (!is_drum(t))
        for (i = 0; i < ft_n; i++) {                  /* their lengths: TIE steps, up to the next note */
            uint32_t d = ft_ev[i].d == FT_OPEN || ft_ev[i].t + ft_ev[i].d > T ? T - ft_ev[i].t : ft_ev[i].d;
            uint32_t idx = (ft_ev[i].t * len * 2u + T) / (2u * T) % len;
            uint32_t steps = (d * len * 2u + T) / (2u * T);
            for (k = 1; k < steps && k < len; k++) {
                step_t *s = &t->step[(idx + k) % len];
                if (step_fires(s->time) && s->n)
                    break;
                memset(s, 0, sizeof *s);
                s->time = ST_TIE;
            }
        }
    t->seq_active = 1;
    song.g[G_BPM] = (int16_t)bpm;
    ft_bars = (uint8_t)bars;
#if FELUCCA_ARRANGER
    arrangement_enabled = 0;
#endif
    seq_start();                                      /* this is the downbeat: the loop plays */
}

/* the UI: a REC / PLAY press is this one's (it runs the take, or just closed it): not the UI's */
static int ft_owns_press(void)
{
    return ft_on || (ft_closed && (uint32_t)(fm1_ms - ft_close_ms) < 300u);
}

/* a free take, once per block: REC closes it, PLAY drops it (their press, timed here, not by the UI) */
static void ft_block(void)
{
    uint32_t b = fm1_in.buttons & (ft_btn_mask | ft_drop_mask), press = b & ~ft_btn_prev;
    ft_btn_prev = b;
    if (!ft_on)
        return;
    ft_t++;
    if (press & ft_drop_mask) {
        ft_on = 0;                                    /* PLAY: dropped, nothing changes */
        ft_bars = 0xFF;
        ft_close_ms = fm1_ms;
        ft_closed = 1;
    } else if ((press & ft_btn_mask) || ft_t >= FT_BLOCKS) {
        ft_close();
    }
}

/* ------------------------------------------------------------ erase --- */
/* EDIT + key: that note / sound goes from the selected pattern. Playing: from the step playing and
 * every step the sequencer enters while the key is held (MPC style); stopped: from every step. */
static uint8_t er_trk;
static uint16_t er_lanes;                     /* drums: lanes held */
static uint32_t er_notes[4];                  /* synth: notes held (bit per MIDI note) */
static volatile uint8_t er_flash;             /* something was erased (the UI flashes) */
/* live mutes: EDIT + a black key enters mute mode; a white key mutes/unmutes
 * its sound (the sequencer skips it). Performance state: not saved, not undone */
static volatile uint16_t drum_mute;
static volatile uint8_t er_mute;

static int er_has(uint32_t note) { return (er_notes[(note >> 5) & 3u] >> (note & 31u)) & 1u; }
static void erase_step(track_t *t, uint32_t idx)
{
    if (is_drum(t)) {
        dstep_t *s = &t->dstep[idx];
        uint32_t l, m = dstep_full_mask(s) & er_lanes;
        for (l = 0; m; l++, m >>= 1)
            if (m & 1u) {
                dstep_clr(s, l);
                er_flash = 1;
            }
    } else {
        step_t *s = &t->step[idx];
        uint32_t i, k = 0, lv = 0, rt = 0;
        if (!step_fires(s->time))
            return;
        for (i = 0; i < s->n; i++)
            if (!er_has(s->note[i])) {
                lv |= ((s->lvl >> (2u * i)) & 3u) << (2u * k);
                rt |= ((s->rat >> (2u * i)) & 3u) << (2u * k);
                s->note[k++] = s->note[i];
            }
        if (k == s->n)
            return;
        er_flash = 1;
        s->n = (uint8_t)k;
        s->lvl = (uint8_t)lv;
        s->rat = (uint8_t)rt;
        if (!k) {
            s->time = ST_REST;
            s->flags = 0;
            s->vel = 0;
        }
    }
}
static void erase_now(track_t *t)             /* a key just went down */
{
    uint32_t i, len = trk_len(t);
    undo_mark(t, undo_erase_sess);
    if (song.playing && t->seq_abs != SEQ_NONE) {
        erase_step(t, t->seq_idx % len);
    } else {
        for (i = 0; i < len; i++)
            erase_step(t, i);
    }
}
static int erasing(const track_t *t) { return trk_index(t) == er_trk && (er_lanes || er_notes[0] || er_notes[1] || er_notes[2] || er_notes[3]); }

/* MIDI OUT of what the sequencer plays (GLO > SYSTEM > MIDI = SEQ; the keys always
 * go out: key_down / key_up). Notes from a computer or the jack are never echoed (no MIDI loop). A set per
 * track of the notes sent on, so a note is ended once, and STOP or MIDI = KEYS end them all */
static uint32_t mo_set[NTRK][4];
static void seq_out_off(const track_t *t, uint32_t note)
{
    uint32_t i = trk_index(t) % NTRK;
    if (note > 127u || !(mo_set[i][note >> 5] & (1u << (note & 31u))))
        return;
    mo_set[i][note >> 5] &= ~(1u << (note & 31u));
    midi_out_event(0x08u | (0x80u | trk_midi_ch(i)) << 8 | note << 16);
}
static uint8_t mo_any;
static void seq_out_on(const track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t i = trk_index(t) % NTRK;
    if (!song.g[G_MIDI] || note > 127u)
        return;
    seq_out_off(t, note);
    mo_set[i][note >> 5] |= 1u << (note & 31u);
    mo_any = 1;
    midi_out_event(0x09u | (0x90u | trk_midi_ch(i)) << 8 | note << 16 | (vel ? vel & 127u : 1u) << 24);
}
static void seq_out_track_off(const track_t *t)
{
    uint32_t i = trk_index(t) % NTRK, w, b;
    for (w = 0; w < 4u; w++)
        for (b = 0; mo_set[i][w]; b++)
            if (mo_set[i][w] & (1u << b)) {
                mo_set[i][w] &= ~(1u << b);
                midi_out_event(0x08u | (0x80u | trk_midi_ch(i)) << 8 | (w * 32u + b) << 16);
            }
}
static void seq_out_all_off(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        seq_out_track_off(&trk[i]);
}


/* ------------------------------------------------------- note input --- */
/* armed and stopped, a note on the selected track: an empty project starts a free take, else the
 * note is the downbeat (the transport starts, recording on) */
static void arm_start(track_t *t)
{
    if (!rec_wait || t != TSEL || song.playing || ci_on)
        return;
    if (project_empty() && !rec_tempo) {
        ft_start(t);                              /* the first take sets the loop and the tempo */
    } else if (rec_count) {
        return;                                   /* COUNT: PLAY counts in; a note only sounds */
    } else {
#if FELUCCA_ARRANGER
        arrangement_enabled = 0;
#endif
        seq_start();                              /* the note is the downbeat */
        if (song.playing)
            rec_begin();
    }
}

static void drum_input(uint32_t lane, uint32_t lvl, uint32_t rat, int rec);
static const uint8_t *in_chord;                    /* input_on's note is note in_chord_i of in_chord (CHORD+) */
static uint32_t in_chord_n, in_chord_i;
static void input_on(track_t *t, uint32_t note, uint32_t vel)
{
    if (is_drum(t)) {                             /* (a GM note on the drum track: its lane) */
        drum_input(lane_of_note(note), vel_lvl(vel), 0, 1);
        return;
    }
    last_note = (uint8_t)note;
    arm_start(t);
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_on(note, vel);
    if (((song.rec >> trk_index(t)) & 1u) && song.playing)
        rec_note(t, note, vel, 0, 1);
    if (in_chord)                                 /* a chord from the keys: maybe strummed (voice.c) */
        trk_note_chord(t, in_chord, in_chord_n, in_chord_i, vel);
    else
        trk_note_on(t, note, vel);
}

static void input_off(track_t *t, uint32_t note)
{
    if (is_drum(t)) {
        if (ft_on && ft_trk == TRK_DRUM)
            ft_note_off(lane_of_note(note));
        return;
    }
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_off(note);
    rec_release(t, note);
    trk_note_off(t, note);
}

/* a drum hit from a key, MIDI or a roll: lane, level; rat: its ratchet when recorded (rolls); rec: it
 * may be recorded (a roll records one hit a step) */
static void drum_input(uint32_t lane, uint32_t lvl, uint32_t rat, int rec)
{
    track_t *t = TDRUM;
    lane &= 15u;
    pen_lane = (uint8_t)lane;
    arm_start(t);
    if (ft_on && ft_trk == TRK_DRUM)
        ft_note_on(lane, lvl);
    if (rec && ((song.rec >> TRK_DRUM) & 1u) && song.playing)
        rec_hit(t, lane, lvl, rat);
    trk_note_on(t, LANE_NOTE[lane], lvl_vel(lvl, 100));
}


/* ---------------------------------------------------------- keyboard --- */
/* the level of a key on the drum track: OCT- held ghost, OCT+ held hard */
static uint32_t key_lvl(void)
{
    uint32_t b = fm1_in.buttons;
    return (b & dyn_bit[0]) ? LV_GHOST : (b & dyn_bit[1]) ? LV_HARD : LV_NORM;
}

/* CHORD+: the modifiers held (the black keys down in chord mode) */
static uint32_t chord_mods(void)
{
    uint32_t k, m = 0;
    for (k = 0; k < 27u; k++)
        if (kb_kind[k] == KS_MOD)
            m |= chord_mod_of_key(k);
    return m;
}
/* a modifier went down or up: every chord held on part sel changes under the finger (the notes it loses end,
 * the ones it gains start; the ones it keeps ring on) */
static void chord_revoice(uint32_t sel)
{
    uint32_t k, i, j, mods = chord_mods(), mc = trk_midi_ch(sel);
    track_t *t = &trk[sel % NTRK];
    for (k = 0; k < 27u; k++) {
        uint8_t nw[4];
        uint32_t nn;
        if (kb_kind[k] != KS_NOTE || kb_trk[k] != sel || !t->p[P_CHORD] || is_drum(t))
            continue;
        nn = chord_play_notes(t, kb_root[k], mods, nw);
        for (i = 0; i < kb_n[k]; i++) {             /* the notes it loses */
            for (j = 0; j < nn && nw[j] != kb_nt[k][i]; j++)
                ;
            if (j == nn) {
                input_off(t, kb_nt[k][i]);
                midi_out_event(0x08u | (0x80u | mc) << 8 | (uint32_t)kb_nt[k][i] << 16);
            }
        }
        for (j = 0; j < nn; j++) {                  /* the notes it gains */
            for (i = 0; i < kb_n[k] && kb_nt[k][i] != nw[j]; i++)
                ;
            if (i == kb_n[k]) {
                input_on(t, nw[j], 100);
                midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)nw[j] << 16 | 100u << 24);
            }
        }
        memcpy(kb_nt[k], nw, nn);
        kb_n[k] = (uint8_t)nn;
    }
}

static void key_down(uint32_t k)
{
    uint32_t layer = layer_now(), sel = song.sel % NTRK, i, mc;
    track_t *t = &trk[sel];
    kb_kind[k] = KS_NONE;
    kb_trk[k] = (uint8_t)sel;
    kb_n[k] = 0;
#if FELUCCA_ARRANGER
    if (live_mode && live_select && layer == LY_PLAY) {
        int32_t w = punch_key(k);
        if (w >= 0 && w < (int32_t)ARR_STEPS) {
            if (song.playing)
                live_mode_req = (int8_t)w;
            else {
                live_mode_scene = (int8_t)w;
                arrangement_apply((uint32_t)w);
            }
        }
        return;
    }
#endif
    if (layer == LY_PLAY && punch_page_on) {
        int32_t fx = punch_key(k) + (int32_t)punch.page * 16;
        kb_kind[k] = KS_FX;
        if (fx >= 0 && fx < (int32_t)PUNCH_NFX && PUNCH_NAME[fx][0]) {
            if (punch.latch_mode)
                punch.latch = (punch.latch == (int8_t)fx) ? (int8_t)-1 : (int8_t)fx;
            punch.req = (int8_t)fx;
            punch.keybit = 1u << k;
        }
        return;
    }
    switch (layer) {
    case LY_FX: {                                     /* FX held: the white keys pick a punch-in effect */
        int32_t fx = punch_key(k) + (int32_t)punch.page * 16;
        kb_kind[k] = KS_FX;
        if (fx >= 0 && fx < (int32_t)PUNCH_NFX) {
            if (punch.latch_mode)
                punch.latch = (punch.latch == (int8_t)fx) ? (int8_t)-1 : (int8_t)fx;
            punch.req = (int8_t)fx;
            punch.keybit = 1u << k;
        }
        return;
    }
    case LY_STEP:
    case LY_SCALE:
    case LY_MIX:
    case LY_SONG:
    case LY_ROLL:                                     /* the UI's: steps, the key, the mix, the sections, gen/mut */
        kb_kind[k] = KS_UI;
        kb_nt[k][0] = (uint8_t)layer;                 /* (its key-up goes to the same layer) */
        lk_push(layer, k, 1);
        return;
    case LY_ERASE:
        kb_kind[k] = KS_ERASE;
        if (is_drum(t) && punch_key(k) < 0) {        /* a black key: erase <-> mute */
            er_mute = (uint8_t)!er_mute;
            return;
        }
        if (is_drum(t) && er_mute) {                  /* mute mode: toggle the lane */
            drum_mute ^= (uint16_t)(1u << lane_of_key(k));
            return;
        }
        if (!erasing(t) || er_trk != sel)
            undo_erase_sess = (undo_sess += 4u) | 2u;  /* a new erase: one undo */
        if (er_trk != sel) {
            er_lanes = 0;
            er_notes[0] = er_notes[1] = er_notes[2] = er_notes[3] = 0;
            er_trk = (uint8_t)sel;
        }
        if (is_drum(t)) {
            kb_nt[k][0] = (uint8_t)lane_of_key(k);
            kb_n[k] = 1;
            er_lanes |= (uint16_t)(1u << kb_nt[k][0]);
        } else {
            uint32_t n = kb_map(t, k);
            if (n == KB_SILENT)
                return;
            kb_n[k] = (uint8_t)(t->p[P_CHORD] ? chord_notes(t, n, kb_nt[k]) : 1u);
            if (!t->p[P_CHORD])
                kb_nt[k][0] = (uint8_t)n;
            for (i = 0; i < kb_n[k]; i++)
                er_notes[(kb_nt[k][i] >> 5) & 3u] |= 1u << (kb_nt[k][i] & 31u);
        }
        erase_now(t);
        return;
    default:
        break;
    }
    if (is_drum(t) && layer == LY_PLAY && kb_grid) {   /* the DRUMS grid page: a step key */
        kb_kind[k] = KS_UI;
        kb_nt[k][0] = (uint8_t)KB_GRID;
        lk_push(KB_GRID, k, 1);
        return;
    }
    if (is_drum(t)) {                                 /* the drum track: the key's lane */
        uint32_t lane = lane_of_key(k), lvl = key_lvl();
        kb_nt[k][0] = (uint8_t)lane;
        kb_n[k] = 1;
        kb_kind[k] = KS_DRUM;
        drum_input(lane, lvl, 0, 1);
        mc = trk_midi_ch(sel);
        midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)LANE_NOTE[lane] << 16 | lvl_vel(lvl, 100) << 24);
        return;
    }
    {
        uint32_t n = kb_map(t, k);
        if (n == KB_SILENT) {
            if (t->p[P_CHORD] && layer == LY_PLAY && chord_mod_of_key(k)) {   /* CHORD+: a modifier key */
                kb_kind[k] = KS_MOD;
                chord_revoice(sel);
            }
            return;
        }
        kb_kind[k] = KS_NOTE;
        kb_root[k] = (uint8_t)n;
        if (t->p[P_CHORD]) {
            kb_n[k] = (uint8_t)chord_play_notes(t, n, chord_mods(), kb_nt[k]);   /* (the modifiers held, voiced) */
        } else {
            kb_nt[k][0] = (uint8_t)cfol_apply(t, n);   /* chord follow on live notes (CF_OFF: unchanged) */
            kb_n[k] = 1;
        }
        mc = trk_midi_ch(sel);
        for (i = 0; i < kb_n[k]; i++) {
            in_chord = kb_n[k] > 1u ? kb_nt[k] : 0;
            in_chord_n = kb_n[k];
            in_chord_i = i;
            input_on(t, kb_nt[k][i], 100);
            in_chord = 0;
            midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)kb_nt[k][i] << 16 | 100u << 24);
        }
        /* the pen of the SEQ layer: the keys down now (a chord), else this note */
        if (!(kb_prev & ~(1u << k)) || pen_n >= 4u)
            pen_n = 0;
        for (i = 0; i < kb_n[k] && pen_n < 4u; i++)
            pen_note[pen_n++] = kb_nt[k][i];
    }
}

static void key_up(uint32_t k)
{
    uint32_t i, mc, kind = kb_kind[k];
    track_t *t = &trk[kb_trk[k] % NTRK];
    kb_kind[k] = KS_NONE;
    switch (kind) {
    case KS_FX:
        if (punch.keybit == 1u << k) {                /* its key is up: the mix comes back */
            punch.keybit = 0;
            punch.req = -1;
        }
        return;
    case KS_UI:
        lk_push(kb_nt[k][0], k, 0);
        return;
    case KS_MOD:                                    /* a CHORD+ modifier let go: the chords held change back */
        chord_revoice(kb_trk[k]);
        return;
    case KS_ERASE:
        if (is_drum(t))
            er_lanes &= (uint16_t)~(1u << kb_nt[k][0]);
        else
            for (i = 0; i < kb_n[k]; i++)
                er_notes[(kb_nt[k][i] >> 5) & 3u] &= ~(1u << (kb_nt[k][i] & 31u));
        return;
    case KS_NOTE:
        mc = trk_midi_ch(kb_trk[k] % NTRK);
        for (i = 0; i < kb_n[k]; i++) {
            input_off(t, kb_nt[k][i]);
            midi_out_event(0x08u | (0x80u | mc) << 8 | (uint32_t)kb_nt[k][i] << 16);
        }
        return;
    case KS_DRUM:
        if (ft_on && ft_trk == TRK_DRUM)
            ft_note_off(kb_nt[k][0]);
        mc = trk_midi_ch(TRK_DRUM);
        midi_out_event(0x08u | (0x80u | mc) << 8 | (uint32_t)LANE_NOTE[kb_nt[k][0] & 15u] << 16);
        return;
    default:
        return;
    }
}

/* the UI asks to hear drum sounds (a sound or a step picked with a knob, a step set from a key):
 * aud_lanes the lanes, each at its level aud_lvl (2 bits a lane), played here, in the audio context */
static volatile uint32_t aud_lanes, aud_lvl;
static void audition_req(uint32_t lanes, uint32_t lvls)
{
    fm1_irq_off();
    aud_lvl = lvls;
    aud_lanes = lanes & 0xFFFFu;
    fm1_irq_on();
}
static void audition_lane(uint32_t lane) { audition_req(1u << (lane & 15u), 0u); }   /* (LV_NORM = 0) */
static void audition_step(const dstep_t *s)            /* every sound of a drum step, at its level */
{
    uint32_t m = dstep_mask(s), lv = 0, l;
    for (l = 0; l < 16u; l++)
        if ((m >> l) & 1u)
            lv |= dstep_lvl(s, l) << (2u * l);
    audition_req(m, lv);
}
static void audition_block(void)
{
    uint32_t m = aud_lanes, lv = aud_lvl, l;
    if (!m)
        return;
    aud_lanes = 0;
    for (l = 0; m; l++, m >>= 1)
        if (m & 1u)
            trk_note_on(TDRUM, LANE_NOTE[l], lvl_vel((lv >> (2u * l)) & 3u, 100));
}

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k;
    audition_block();
    if (er_mute && layer_now() != LY_ERASE)
        er_mute = 0;                              /* EDIT let go: back to erase mode */
    if (!ch)
        return;
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u)
            key_down(k);
        else
            key_up(k);
        kb_prev ^= 1u << k;                           /* (key_down sees the keys down before it) */
    }
}

/* LIVE: one record arm at most, on the selected track; selecting another track moves it there
 * (an arm follows too: it waits for a note on the track selected; a free take stays on its track) */
static void rec_follow(uint32_t sel)
{
    if (song.rec)
        song.rec = (uint8_t)(1u << (sel % NTRK));
}

/* -------------------------------------------------------- metronome --- */
/* A wood block on every beat, louder on the first of the bar, from the drum kit's voices
 * (not recorded, not muted with the drum track; GLO > DRUMS LVL sets its level).
 * GLO > GLOBAL CLICK: OFF (0, default: LIVE needs none), REC = while a track records,
 * ON = while playing. */
#define CLICK_REC 1
#define CLICK_ON 2
static uint32_t click_last = SEQ_NONE;          /* the beat it last played */
static void click_tick(void)
{
    uint32_t mode = (uint32_t)song.g[G_CLOCK];
    if (!song.playing || clk_beat == click_last)
        return;
    click_last = clk_beat;
    if (mode == CLICK_ON || (mode == CLICK_REC && song.rec))
        drum_on(clk_beat % 4u ? 76u : 77u, clk_beat % 4u ? 72u : 120u);
}

/* -------------------------------------------------------- sequencer --- */
#if FELUCCA_ARRANGER
/* ---- LIVE SECTIONS (SAVE held + key, ui_layers.c): a section asked for while playing starts on the
 * next bar, every track from its step 0 (as the song does). SONG REC writes the order you play into the
 * song chain (arrangement): each section with the bars it played, from the bar after the arm. */
static volatile int8_t live_req = -1;              /* section asked for (UI), applied on the next bar */
static volatile int8_t live_sec = -1;              /* the section playing: last jumped to, loaded or stored */
static uint32_t live_bar = 0xFFFFFFFFu;            /* clk_beat / 4 of the last bar seen (= bars the section played) */
/* QUICK CHAIN (SAVE held, two or more section keys tapped): the sections in order, looped, each for the bars its
 * longest pattern takes (section_bars); the UI writes it whole with the IRQ off, chain_n last (0 = none) */
#define CHAIN_MAX 8u
static volatile uint8_t chain_sec[CHAIN_MAX];
static volatile uint8_t chain_n, chain_i;          /* entries; the one playing (or asked for) */
static volatile uint8_t chain_bars;                /* the bars it plays */
static uint32_t section_bars(uint32_t s);          /* project.c (arranger_scene.c) */
/* LIVE-MODE CHAIN (ARP held, two or more entry keys tapped): same looping logic, but for live mode
 * entries; each plays for arrangement.entry[s].bars bars; the UI writes it whole with the IRQ off */
static volatile uint8_t lm_chain[CHAIN_MAX];
static volatile uint8_t lm_chain_n, lm_chain_i;   /* entries; the one playing */
static volatile uint8_t lm_chain_bars;             /* the bars this entry plays */
static volatile uint8_t srec;                      /* SONG REC: 0 off, 1 armed (from the next bar), 2 recording */
static arr_entry_t srec_e[ARR_STEPS];
static volatile uint8_t srec_n;                    /* entries so far (the last one still growing) */
static volatile uint8_t srec_done;                 /* the chain was written: n parts (0xFF: nothing played) */

static void srec_finish(void)                      /* (audio ISR, or the UI with the IRQ off) */
{
    uint32_t i, n = 0;
    for (i = 0; i < srec_n; i++)
        if (srec_e[i].bars)
            srec_e[n++] = srec_e[i];
    if (n) {
        arrangement.count = (uint8_t)n;
        arrangement.loop = 0;
        for (i = 0; i < n; i++)
            arrangement.entry[i] = srec_e[i];
    }
    srec_done = (uint8_t)(n ? n : 0xFFu);
    srec = 0;
    srec_n = 0;
}
static void srec_add(uint32_t s)
{
    uint32_t k;
    if (srec_n >= ARR_STEPS) {
        srec_finish();                              /* the chain is full: what was played so far */
        return;
    }
    for (k = 0; k < ARR_TRACKS; k++)
        srec_e[srec_n].track[k] = (uint8_t)s;
    srec_e[srec_n].bars = 0;
    srec_n++;
}
/* STOP (or SONG REC pressed again): the bar playing counts if it had begun */
static void srec_stop(void)
{
    if (srec == 2u && srec_n && srec_e[srec_n - 1u].bars <= 126u &&
        ((!(clk_beat & 3u) && (clk_beat >> 2) != live_bar) || (clk_beat & 3u)))
        srec_e[srec_n - 1u].bars += 2;
    if (srec == 2u)
        srec_finish();
    srec = 0;
}
static void seq_reset_tracks(uint32_t pos);
static void live_block(void)                       /* once a block while playing a loop (not the song) */
{
    if ((clk_beat & 3u) || (clk_beat >> 2) == live_bar)
        return;
    live_bar = clk_beat >> 2;                       /* a new bar */
    if (srec == 2u && srec_n) {
        arr_entry_t *e = &srec_e[srec_n - 1u];
        if (e->bars <= 126u) {
            e->bars += 2;
        } else {                                    /* (64 bars of one section: it goes on in the next entry) */
            srec_add(e->track[0]);   /* live sections are whole-scene; track[0] is representative */
            if (srec == 2u)
                srec_e[srec_n - 1u].bars = 2;
        }
    }
    if (chain_n && live_req < 0 && live_bar >= chain_bars) {   /* the chain: this entry has played its bars */
        chain_i = (uint8_t)((chain_i + 1u) % (chain_n < CHAIN_MAX ? chain_n : CHAIN_MAX));
        live_req = (int8_t)(chain_sec[chain_i] & 3u);
    }
    if (live_req >= 0) {
        uint32_t s = (uint32_t)live_req;
        live_req = -1;
        {                                           /* (the UI asked for a section it checked: no hash here) */
            arrangement_apply_scene(s);
            song.rec = 0;                           /* (a take does not run on into another section) */
            live_sec = (int8_t)s;
            seq_reset_tracks(clk_pos);              /* on the bar: every track from its step 0 */
            live_bar = 0;
            if (chain_n)
                chain_bars = (uint8_t)section_bars(chain_sec[chain_i % CHAIN_MAX]);
            if (srec == 2u)
                srec_add(s);
        }
    }
    if (srec == 1u && live_sec >= 0) {
        srec = 2;
        srec_n = 0;
        srec_add((uint32_t)live_sec);
    }
    if (live_mode && lm_chain_n && live_mode_req < 0 && live_bar >= lm_chain_bars) {
        lm_chain_i = (uint8_t)((lm_chain_i + 1u) % lm_chain_n);
        live_mode_req = (int8_t)lm_chain[lm_chain_i];
    }
    if (live_mode && live_mode_req >= 0) {
        uint32_t s = (uint32_t)live_mode_req;
        live_mode_req = -1;
        arrangement_apply(s);
        song.rec = 0;
        live_mode_scene = (int8_t)s;
        seq_reset_tracks(clk_pos);
        live_bar = 0;
        if (lm_chain_n)
            lm_chain_bars = arrangement.entry[lm_chain[lm_chain_i]].bars;
    }
}
#endif

/* every track from its step 0, together, at pos units into beat 0 (a song section: the arranger's
 * remainder, so the new section starts exactly on its bar) */
static void seq_reset_tracks(uint32_t pos)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        t->seq_abs = SEQ_NONE;
        t->seq_idx = 0;
        t->rskip_n = 0;
        t->rskip_lanes = 0;
        t->rskip_abs = SEQ_NONE;
        t->rh_n = 0;
        t->pass = 0;
    }
    clk_beat = 0;
    clk_pos = pos;
#if FELUCCA_ARRANGER
    live_bar = 0xFFFFFFFFu;                        /* (bar 0 is a new bar: SONG REC can start on it) */
#endif
    click_last = SEQ_NONE;
    cseq_abs = SEQ_NONE;
    cseq_idx = 0;
    song.tick = 0;
    song.playing = 1;
    slicer_start(pos);                             /* slicer.c: its step 0 with the sequencer's */
}


static void song_backup(void);                     /* project.c: song mode keeps the loop you made */
static void song_restore(void);
static void locks_restore(track_t *t);             /* Task 14: restore locked params on stop */
static void locks_clear(track_t *t);               /* Task 14: clear all locks (empty pattern) */

/* fill state (Task 13): updated each block in events_block, read by seq_tick */
static volatile uint8_t fill_held;
static volatile uint8_t fill_arm;
static uint8_t fill_bar_on;
static uint8_t fill_now;
static uint32_t fill_last_bar = 0xFFFFFFFFu;

/* drum fill state: ROLL + white key 2..7 on a drum track triggers a hardcoded fill pattern
 * that replaces the drum steps from the current position to the end of the pattern loop.
 * dstep_t bytes: {on[2], lvl[4], rat[4]}. on bits: lane L = on[L/8] bit (L%8).
 * Lanes used: 0=kick, 2=snare, 4=hi-hat-closed, 9=tom-lo, 10=tom-hi, 11=crash.
 * lvl[L/4] bits 2*(L%4): level (0=norm). rat[L/4] bits 2*(L%4): ratchet (0=x1,1=x2,3=x4).
 * Snare x2: rat[0] |= 1<<4 = 0x10. Snare x4: rat[0] |= 3<<4 = 0x30. */
#define DS_E      {{0x00,0x00},{0,0,0,0},{0,0,0,0}}  /* empty */
#define DS_K      {{0x01,0x00},{0,0,0,0},{0,0,0,0}}  /* kick */
#define DS_S      {{0x04,0x00},{0,0,0,0},{0,0,0,0}}  /* snare */
#define DS_SX2    {{0x04,0x00},{0,0,0,0},{0x10,0,0,0}} /* snare x2 */
#define DS_SX4    {{0x04,0x00},{0,0,0,0},{0x30,0,0,0}} /* snare x4 */
#define DS_KCR    {{0x01,0x08},{0,0,0,0},{0,0,0,0}}  /* kick + crash */
#define DS_HH     {{0x10,0x00},{0,0,0,0},{0,0,0,0}}  /* closed hi-hat */
#define DS_HH_K   {{0x11,0x00},{0,0,0,0},{0,0,0,0}}  /* hat + kick */
#define DS_HH_KS  {{0x15,0x00},{0,0,0,0},{0,0,0,0}}  /* hat + kick + snare */
#define DS_HH_CR  {{0x10,0x08},{0,0,0,0},{0,0,0,0}}  /* hat + crash */
#define DS_TH     {{0x00,0x04},{0,0,0,0},{0,0,0,0}}  /* hi tom */
#define DS_TL     {{0x00,0x02},{0,0,0,0},{0,0,0,0}}  /* lo tom */
#define DS_THTL   {{0x00,0x06},{0,0,0,0},{0,0,0,0}}  /* hi + lo tom */
#define DRUM_NFILLS 6
static const dstep_t drum_fill_pats[DRUM_NFILLS][16] = {
    { /* Fill 0: Classic snare buildup → crash */
        DS_E,  DS_E,  DS_E,  DS_E,
        DS_S,  DS_E,  DS_E,  DS_E,
        DS_S,  DS_E,  DS_S,  DS_E,
        DS_S,  DS_S,  DS_SX2, DS_KCR,
    },
    { /* Fill 1: 16th hi-hat run → crash */
        DS_HH, DS_HH, DS_HH, DS_HH,
        DS_HH, DS_HH, DS_HH, DS_HH,
        DS_HH, DS_HH, DS_HH, DS_HH,
        DS_HH, DS_HH, DS_HH, DS_KCR,
    },
    { /* Fill 2: Tom roll descending (hi → lo) → crash */
        DS_E,    DS_E,    DS_E,    DS_E,
        DS_TH,   DS_TH,   DS_TH,   DS_TH,
        DS_TL,   DS_TL,   DS_TL,   DS_TL,
        DS_THTL, DS_THTL, DS_THTL, DS_KCR,
    },
    { /* Fill 3: Snare doublets → crash */
        DS_E,  DS_E,  DS_E,  DS_E,
        DS_E,  DS_E,  DS_E,  DS_E,
        DS_S,  DS_S,  DS_S,  DS_S,
        DS_S,  DS_S,  DS_SX2, DS_KCR,
    },
    { /* Fill 4: Kick-snare alternating → snare roll → crash */
        DS_K,  DS_E,  DS_S,  DS_E,
        DS_K,  DS_E,  DS_S,  DS_E,
        DS_K,  DS_E,  DS_S,  DS_E,
        DS_K,  DS_E,  DS_SX4, DS_KCR,
    },
    { /* Fill 5: Full driver — hat throughout, kick/snare accents, crash at end */
        DS_HH_K, DS_HH, DS_HH, DS_HH,
        DS_HH_KS, DS_HH, DS_HH, DS_HH,
        DS_HH_K, DS_HH, DS_HH, DS_HH,
        DS_HH_KS, DS_HH, DS_HH, DS_HH_CR,
    },
};
#undef DS_E
#undef DS_K
#undef DS_S
#undef DS_SX2
#undef DS_SX4
#undef DS_KCR
#undef DS_HH
#undef DS_HH_K
#undef DS_HH_KS
#undef DS_HH_CR
#undef DS_TH
#undef DS_TL
#undef DS_THTL

static volatile int8_t drum_fill_req = -1;
static int8_t drum_fill_cur = -1;
static uint32_t drum_fill_start_abs;
static uint32_t drum_fill_end_abs;

static void drum_fill_trigger(uint32_t n)
{
    drum_fill_req = (int8_t)(n < DRUM_NFILLS ? (int8_t)n : 0);
}

static void seq_start(void)
{
#if FELUCCA_ARRANGER
    if (arrangement_enabled && !song.playing) {
        rec_wait = 0;                              /* song mode plays, it does not record */
        song_backup();
        chain_n = 0;                               /* (the song, not a quick chain) */
    }
    if (live_mode && !song.playing) {
        rec_wait = 0;
        chain_n = 0;
        if (live_mode_scene < 0)
            live_mode_scene = 0;
        arrangement_apply((uint32_t)(uint8_t)live_mode_scene);
        song.rec = 0;
    }
    if (!arrangement_start()) return;
#endif
    fill_last_bar = 0xFFFFFFFFu;
    seq_reset_tracks(0);
}

static void seq_release(track_t *t)
{
    uint32_t i;
    for (i = 0; i < t->seq_n; i++) {
        trk_note_off(t, t->seq_notes[i]);
        seq_out_off(t, t->seq_notes[i]);
    }
    if (is_drum(t))
        seq_out_track_off(t);                       /* the drum hits of the step: ended with it */
    t->seq_n = 0;
    t->seq_hold = 0;
    t->slide_glide = 0;                             /* live MONO / LEG keys must not glide after it */
}

static void seq_stop(void)
{
    uint32_t i;
#if FELUCCA_ARRANGER
    if (song.playing)
        srec_stop();                                /* SONG REC: the order played so far is the song */
    live_req = -1;
    chain_n = 0;
    if (live_mode) {
        live_select = 0;
        live_mode_req = -1;
        lm_chain_n = 0;
    }
#endif
    song.playing = 0;
    fill_held = 0;
    fill_arm = 0;
    fill_bar_on = 0;
    drum_fill_cur = -1;
    drum_fill_req = -1;
    for (i = 0; i < NTRK; i++) {
        seq_release(&trk[i]);
        locks_restore(&trk[i]);
        trk[i].rh_n = 0;                           /* a recorded note held over the stop: as far as it got */
    }
    seq_out_all_off();
#if FELUCCA_ARRANGER
    if (arrangement_clock.running) {
        arrangement_clock.running = 0;
        punch.song = -1;
        arr_song_transpose = 0;
        arr_song_trans_inkey = 0;
        song_restore();                            /* back to the loop you were making */
    }
#endif
}

/* conditional hit resolution: idx is offset from ST_COND_FIRST / HIT_COND_FIRST.
 * pass is 1-indexed (incremented at the start of each loop).
 * Stubs (idx 4-6) use mod=1/tgt=1 which never fires (pass%1 is always 0). */
static const uint8_t COND_MOD[8] = {2, 2, 4, 4, 1, 1, 1, 8};
static const uint8_t COND_TGT[8] = {1, 0, 1, 0, 1, 1, 1, 0};
static int cond_fires_idx(uint32_t idx, uint32_t pass)
{
    return (pass % COND_MOD[idx]) == COND_TGT[idx];
}

/* the velocity of note i of synth step s; accented: resolved at seq_step (may be probabilistic) */
static uint32_t step_vel(const step_t *s, uint32_t i, uint32_t accented)
{
    uint32_t base = accented ? 127u : (s->vel ? s->vel : 96u);
    return lvl_vel((s->lvl >> (2u * i)) & 3u, base);
}

/* Apply per-fragment transpose from song mode to a sequencer note (synth parts only). */
#if FELUCCA_ARRANGER
static uint32_t seq_note_trans(const track_t *t, uint32_t n)
{
    n = cfol_apply(t, n);
    if (is_drum(t) || !arr_song_transpose) return n;
    n = (uint32_t)clamp((int32_t)n + arr_song_transpose, 0, 127);
    if (arr_song_trans_inkey) n = seq_snap_scale(n, t);
    return n;
}
#else
static uint32_t seq_note_trans(const track_t *t, uint32_t n) { return cfol_apply(t, n); }
#endif

/* play one synth step: TIE extends, REST releases, NOTE (re)triggers; a SLIDE on the previous
 * step makes this one legato with a glide (acid style). skip: bit k = note k already sounds
 * from live recording (not triggered, not released here). len: the step's length (units). */
static void seq_step(track_t *t, const step_t *s, uint32_t slen, uint32_t skip)
{
    uint32_t i, j, gate = slen * (uint32_t)t->p[P_SGATE] / 128u;
    uint32_t slide_in = t->seq_hold && t->seq_n;
    uint32_t next_tie = t->step[(t->seq_idx + 1u) % trk_len(t)].time == ST_TIE;
    uint32_t step_time = s->time, do_accent, do_slide;
    uint32_t acc_t, sld_t;

    /* resolve conditional or probability for time */
    if (step_time >= ST_COND_FIRST) {
        step_time = cond_fires_idx(step_time - ST_COND_FIRST, t->pass) ? ST_NOTE : ST_REST;
    } else if (step_time >= ST_P75) {
        uint32_t thresh = step_time == ST_P75 ? 96u : step_time == ST_P50 ? 64u : 32u;
        step_time = ((rng() & 127u) < thresh) ? ST_NOTE : ST_REST;
    }
    /* resolve flag probabilities (128 threshold = always fires) */
    acc_t = flags_acc_thresh(s->flags);
    sld_t = flags_sld_thresh(s->flags);
    do_accent = flags_is_accent(s->flags) && (acc_t >= 128u || (rng() & 127u) < acc_t);
    do_slide  = flags_is_slide(s->flags)  && (sld_t >= 128u || (rng() & 127u) < sld_t);
    t->seq_accent = (uint8_t)do_accent;

    if (step_time == ST_TIE) {
        if (t->seq_n) {
            t->seq_off = gate + slen / 2u;
            t->seq_hold = (uint8_t)(do_slide || next_tie);   /* chains hold at any GATE / swing */
        }
        return;
    }
    if (step_time == ST_REST || !s->n) {
        seq_release(t);
        return;
    }
    if (s->rat) {                                   /* ratchets: each hit its share of the step */
        uint32_t hits = 1u + ((s->rat >> 0) & 3u);
        for (i = 1; i < s->n; i++)
            if (1u + ((s->rat >> (2u * i)) & 3u) > hits)
                hits = 1u + ((s->rat >> (2u * i)) & 3u);
        gate /= hits;
    }
    t->slide_glide = (uint8_t)slide_in;
    if (!slide_in)
        seq_release(t);
    for (i = 0; i < s->n; i++)
        if (!((skip >> i) & 1u)) {
            uint32_t tn = seq_note_trans(t, s->note[i]);
            uint32_t sv = step_vel(s, i, do_accent);
            trk_note_on(t, tn, sv);
            if (!slide_in || !(mo_set[trk_index(t) % NTRK][tn >> 5] & (1u << (tn & 31u))))
                seq_out_on(t, tn, sv);
        }
    if (slide_in)                                   /* release what is not held over */
        for (i = 0; i < t->seq_n; i++) {
            for (j = 0; j < s->n && seq_note_trans(t, s->note[j]) != t->seq_notes[i]; j++)
                ;
            if (j == s->n) {
                trk_note_off(t, t->seq_notes[i]);
                seq_out_off(t, t->seq_notes[i]);
            }
        }
    t->seq_n = 0;
    for (i = 0; i < s->n; i++)
        if (!((skip >> i) & 1u))
            t->seq_notes[t->seq_n++] = (uint8_t)seq_note_trans(t, s->note[i]);
    t->seq_off = gate;
    t->seq_hold = (uint8_t)(!s->rat && (do_slide || next_tie));   /* next step a TIE: keep the notes to it */
}

/* play one drum step: each lane a hit (skip: lanes already played by live recording, or rolling) */
static void drum_step(track_t *t, const dstep_t *s, uint32_t skip)
{
    static const uint8_t PRB_THRESH[3] = {96, 64, 32};
    uint32_t l;
    t->drum_fired = 0;
    seq_out_track_off(t);
    for (l = 0; l < DRUM_LANES; l++) {
        uint32_t h;
        if ((skip >> l) & 1u) continue;
        h = dstep_hit(s, l);
        if (h == HIT_OFF) continue;
        if (h <= HIT_X4) {
            t->drum_fired |= 1u << l;
            trk_note_on(t, LANE_NOTE[l], lvl_vel(dstep_lvl(s, l), 100));
            seq_out_on(t, LANE_NOTE[l], lvl_vel(dstep_lvl(s, l), 100));
        } else if (h <= HIT_P25) {
            if ((rng() & 127u) < PRB_THRESH[h - HIT_P75]) {
                t->drum_fired |= 1u << l;
                trk_note_on(t, LANE_NOTE[l], 100);
                seq_out_on(t, LANE_NOTE[l], 100);
            }
        } else {
            if (cond_fires_idx(h - HIT_COND_FIRST, t->pass)) {
                t->drum_fired |= 1u << l;
                trk_note_on(t, LANE_NOTE[l], 100);
                seq_out_on(t, LANE_NOTE[l], 100);
            }
        }
    }
}

/* ---- fills (Task 13) ---- */
static uint32_t step_fill(const track_t *t, uint32_t idx)
{
    idx %= NSTEP;
    return (uint32_t)(t->fill[idx / 4u] >> (2u * (idx % 4u))) & 3u;
}
static void step_fill_set(track_t *t, uint32_t idx, uint32_t v)
{
    uint32_t sh;
    idx %= NSTEP;
    sh = 2u * (idx % 4u);
    t->fill[idx / 4u] = (uint8_t)((t->fill[idx / 4u] & ~(3u << sh)) | (v & 3u) << sh);
}

static uint32_t step_plays(const track_t *t, uint32_t idx)
{
    uint32_t c = step_fill(t, idx);
    return c == FC_FILL ? fill_now : c == FC_NOFILL ? !fill_now : 1u;
}

/* ---- parameter locks (Task 14) ---- */
/* A lock: on its step the track's p[param] takes its value; the parameter goes back to what it was
 * at the next step without a lock on it (Elektron style). Only sound parameters lock (p_lockable). */
static int p_lockable(uint32_t id)
{
    return id <= P_LD_AMP || id == P_SGATE || (id >= P_DIST && id <= P_REV) || id == P_GLIDE || id == P_PAN ||
           id == P_DETUNE || (id >= P_SLCR && id <= P_SLDEPTH) || (id >= P_E0 && id <= P_E7) || id == P_TFLT;
}
static const param_desc_t *lock_desc(const track_t *t, uint32_t id)
{
    if (is_drum(t) && id == P_E0)
        return &DRUM_KIT_DESC;
    if (id >= P_E0 && id <= P_E7)
        return &ENGINES[t->engine % NENGINES]->edit[id - P_E0];
    return &TP[id % P_COUNT];
}
static void lock_write(track_t *t, uint32_t id, int32_t v)
{
    const param_desc_t *d = lock_desc(t, id);
    t->p[id % P_COUNT] = (int16_t)clamp(v, d->min, d->max);
}
static void locks_restore(track_t *t)
{
    uint32_t i;
    for (i = 0; i < t->lk_n && i < NLOCK; i++)
        if (t->p[t->lk_param[i] % P_COUNT] == t->lk_set[i])
            lock_write(t, t->lk_param[i], t->lk_base[i]);
    t->lk_n = 0;
}
static void locks_clear(track_t *t)
{
    uint32_t i;
    locks_restore(t);
    memset(t->micro, 0, sizeof t->micro);
    for (i = 0; i < NLOCK; i++) {
        t->lock[i].step = LOCK_FREE;
        t->lock[i].param = 0;
        t->lock[i].val = 0;
    }
}
static void lock_step(track_t *t, uint32_t idx)
{
    uint32_t i, k, n = 0;
    for (i = 0; i < t->lk_n && i < NLOCK; i++) {
        uint32_t p = t->lk_param[i] % P_COUNT, has = 0;
        for (k = 0; k < NLOCK; k++)
            if (t->lock[k].step == idx && t->lock[k].param == p)
                has = 1;
        if (has) {
            t->lk_param[n] = (uint8_t)p;
            t->lk_base[n] = t->lk_base[i];
            t->lk_set[n] = t->lk_set[i];
            n++;
        } else if (t->p[p] == t->lk_set[i]) {
            lock_write(t, p, t->lk_base[i]);
        }
    }
    t->lk_n = (uint8_t)n;
    for (k = 0; k < NLOCK; k++) {
        const plock_t *l = &t->lock[k];
        uint32_t p = l->param;
        if (l->step != idx || p >= P_COUNT || !p_lockable(p))
            continue;
        for (i = 0; i < t->lk_n && t->lk_param[i] != p; i++)
            ;
        if (i == t->lk_n) {
            if (i >= NLOCK)
                continue;
            t->lk_param[i] = (uint8_t)p;
            t->lk_base[i] = t->p[p];
            t->lk_n++;
        } else if (t->p[p] != t->lk_set[i]) {
            t->lk_base[i] = t->p[p];
        }
        lock_write(t, p, l->val);
        t->lk_set[i] = t->p[p];
    }
}
static int lock_find(const track_t *t, uint32_t step, uint32_t param, int make)
{
    uint32_t k;
    int fr = -1;
    for (k = 0; k < NLOCK; k++) {
        if (t->lock[k].step == step && t->lock[k].param == param)
            return (int)k;
        if (fr < 0 && t->lock[k].step == LOCK_FREE)
            fr = (int)k;
    }
    return make ? fr : -1;
}
static int lock_set(track_t *t, uint32_t step, uint32_t param, int32_t v)
{
    int k;
    const param_desc_t *d;
    if (step >= NSTEP || param >= P_COUNT || !p_lockable(param) || (k = lock_find(t, step, param, 1)) < 0)
        return 0;
    d = lock_desc(t, param);
    t->lock[k].step = (uint8_t)step;
    t->lock[k].param = (uint8_t)param;
    t->lock[k].val = (int16_t)clamp(v, d->min, d->max);
    return 1;
}
static void lock_del(track_t *t, uint32_t step, uint32_t param)
{
    uint32_t k;
    for (k = 0; k < NLOCK; k++)
        if (t->lock[k].step == step && (param >= P_COUNT || t->lock[k].param == param))
            t->lock[k].step = LOCK_FREE;
}
static int step_locked(const track_t *t, uint32_t step)
{
    uint32_t k;
    if (step < NSTEP && t->micro[step])
        return 1;
    for (k = 0; k < NLOCK; k++)
        if (t->lock[k].step == step)
            return 1;
    return 0;
}
/* ratchets: the further hits of the playing step's notes / lanes, each at its share of the step */
static void seq_ratchets(track_t *t, uint32_t into, uint32_t slen)
{
    uint32_t i;
    if (t->seq_skip)
        return;                                     /* (its condition failed: no hit at all) */
    if (is_drum(t)) {
        const dstep_t *s = &t->dstep[t->seq_idx % NSTEP];
        uint32_t m = t->drum_fired & dstep_mask(s);
        for (i = 0; m; i++, m >>= 1) {
            uint32_t hits = 1u + dstep_rat(s, i), h, done;
            if (!(m & 1u) || hits == 1u)
                continue;
            h = into * hits / slen;
            done = (t->rat_lanes >> (2u * i)) & 3u;
            if (h > done && h < hits) {
                t->rat_lanes = (t->rat_lanes & ~(3u << (2u * i))) | h << (2u * i);
                trk_note_on(t, LANE_NOTE[i], lvl_vel(dstep_lvl(s, i), 100));
                seq_out_on(t, LANE_NOTE[i], lvl_vel(dstep_lvl(s, i), 100));
            }
        }
        return;
    }
    {
        const step_t *s = &t->step[t->seq_idx % NSTEP];
        if (!step_fires(s->time) || !s->rat)
            return;
        for (i = 0; i < s->n; i++) {
            uint32_t hits = 1u + ((s->rat >> (2u * i)) & 3u), h;
            if (hits == 1u)
                continue;
            h = into * hits / slen;
            if (h > t->rat_done[i] && h < hits) {
                uint32_t j;
                uint32_t tn = seq_note_trans(t, s->note[i]);
                t->rat_done[i] = (uint8_t)h;
                trk_note_off(t, tn);
                trk_note_on(t, tn, step_vel(s, i, t->seq_accent));
                seq_out_on(t, tn, step_vel(s, i, t->seq_accent));
                t->seq_off = slen / hits * (uint32_t)t->p[P_SGATE] / 128u;
                for (j = 0; j < t->seq_n && t->seq_notes[j] != tn; j++)
                    ;
                if (j == t->seq_n && t->seq_n < 4u)
                    t->seq_notes[t->seq_n++] = (uint8_t)tn;   /* (its gate ends it) */
            }
        }
    }
}

/* the nudge of grid step abs of track t, in units of a step slen long: where in its own step it fires
 * (micro >= 0), or how far before its step (micro < 0, as a negative number) */
static int32_t micro_units(const track_t *t, uint32_t abs, uint32_t slen)
{
    int32_t m = t->micro[abs % trk_len(t) % NSTEP];
    return (int32_t)(slen / 64u) * m;                /* |m| <= 32: fits */
}

/* The steps fire in order, one a block at most, each at its nudged time (micro: 1/64 of a step early
 * or late): the step after the last one played (seq_abs) is due when the grid is in its own step past
 * its nudge, or, nudged early, in the previous grid step past (length - |nudge|). */
static void seq_tick(track_t *t, uint32_t adv)
{
    uint32_t len = trk_len(t), into, slen, abs, idx, nabs, fire = 0;
    int32_t rel;
    if (t->seq_n && !t->seq_hold) {
        if (t->seq_off <= adv)
            seq_release(t);
        else
            t->seq_off -= adv;
    }
    if (!song.playing)
        return;
    abs = trk_grid(t, &into, &slen);
    {
        uint32_t div = trk_div(t);
        if (t->seq_abs != SEQ_NONE && div != t->seq_den)
            t->seq_abs = abs;                        /* DIV changed: the next step of the new grid plays */
        t->seq_den = (uint8_t)div;
    }
    if (t->seq_abs == SEQ_NONE) {                    /* PLAY: the step the grid is in (nudged late: once there) */
        nabs = abs;
        fire = micro_units(t, nabs, slen) <= (int32_t)into;
    } else {
        int32_t mu;
        nabs = t->seq_abs + 1u;
        mu = micro_units(t, nabs, slen);
        if (abs == nabs)
            fire = mu <= (int32_t)into;              /* its own step: past its nudge (early: due already) */
        else if (abs + 1u == nabs)
            fire = mu < 0 && (int32_t)slen + mu <= (int32_t)into;   /* the step before: nudged early into it */
        else if ((int32_t)(abs - nabs) > 0)
            fire = 1;                                /* the grid jumped ahead: catch up, a step a block */
        /* (abs + 1 == seq_abs: SWING turned up inside a played odd step: nothing until the grid is back) */
    }
    if (fire) {                                      /* a new step: one a block at most */
        t->seq_abs = nabs;
        idx = nabs % len;
        t->seq_idx = (uint16_t)idx;
        t->rat_done[0] = t->rat_done[1] = t->rat_done[2] = t->rat_done[3] = 0;
        t->rat_lanes = 0;
        t->drum_fired = 0;
        if (!idx) {
            t->pass++;                               /* a new pass of the loop (recording: one undo) */
            if (t->p[P_EVOLVE]) {                   /* EVOLVE: request typed mutations after N loops */
                static const uint8_t EVL_DIV[4] = {0, 4, 8, 16};
                uint32_t ti = trk_index(t), packed = (uint32_t)(uint16_t)t->p[P_EVOLVE], st;
                for (st = 0; st < 4u; st++) {
                    uint32_t div = EVL_DIV[(packed >> (st * 2u)) & 3u];
                    if (div && t->pass && (t->pass % div) == 0)
                        evl_req |= (uint16_t)(1u << (ti * 4u + st));
                }
            }
        }
        if (erasing(t))
            erase_step(t, idx);                      /* EDIT + key held: gone as it passes */
        t->seq_skip = (uint8_t)!step_plays(t, idx);
        if (t->seq_skip) {                           /* its fill condition fails: as a REST with no lock */
            lock_step(t, NSTEP);                     /* (no step has locks there: the bases are back) */
            t->rskip_lanes = 0;
            t->rskip_n = 0;
            if (is_drum(t)) {
                seq_out_track_off(t);
            } else {
                rec_hold(t, idx, len, nabs);
                seq_release(t);
            }
        } else if (is_drum(t)) {
            uint32_t skip = t->rskip_abs == nabs ? t->rskip_lanes : 0u;
            uint32_t fill_len = 0, local = 0, fi = 0;
            lock_step(t, idx);                       /* its parameter locks, before the block renders */
            t->rskip_lanes = 0;
            { /* fill-row trigger: a sequenced step that fires a fill pattern */
                uint32_t fi;
                for (fi = 0; fi < FILL_SEQ_MAX; fi++) {
                    const fill_seq_t *fs = &dext.fseq[fi];
                    uint32_t cond, which;
                    if (fs->step == 0xFFu || fs->step != (uint8_t)idx) continue;
                    cond = fs->cfg & 0x0Fu;
                    if (!cond || cond_fires_idx(cond - 1u, t->pass)) {
                        which = (fs->cfg >> 4) & 0x07u;
                        drum_fill_req = (int8_t)(which < DRUM_NFILLS ? which : (int8_t)(rng() % DRUM_NFILLS));
                    }
                    break;
                }
            }
            if (drum_fill_req >= 0) {
                drum_fill_cur = drum_fill_req;
                drum_fill_req = (int8_t)-1;
                drum_fill_start_abs = nabs;
                drum_fill_end_abs = nabs + (len - idx);
            }
            if (drum_fill_cur >= 0 && nabs >= drum_fill_end_abs)
                drum_fill_cur = -1;
            if (drum_fill_cur >= 0) {
                fill_len = drum_fill_end_abs - drum_fill_start_abs;
                local    = nabs - drum_fill_start_abs;
                fi       = (16u - fill_len % 16u + local) % 16u;
                drum_step(t, &drum_fill_pats[(uint32_t)drum_fill_cur][fi], skip | (uint32_t)drum_mute);
            } else {
                drum_step(t, &t->dstep[idx], skip | (uint32_t)drum_mute);
            }
        } else {
            const step_t *s = &t->step[idx];
            uint32_t skip = 0, i, k;
            lock_step(t, idx);
            rec_hold(t, idx, len, nabs);
            if (t->rskip_n && t->rskip_abs == nabs)
                for (i = 0; i < s->n; i++)
                    for (k = 0; k < t->rskip_n; k++)
                        if (s->note[i] == t->rskip[k])
                            skip |= 1u << i;
            t->rskip_n = 0;
            seq_step(t, s, slen, skip);
        }
    }
    /* the ratchets of the step playing, timed from where it fired (its hits ride with its nudge) */
    if (t->seq_abs == abs)
        rel = (int32_t)into - micro_units(t, abs, slen);
    else if (t->seq_abs == abs + 1u)
        rel = (int32_t)into - ((int32_t)slen + micro_units(t, abs + 1u, slen));
    else
        rel = (int32_t)into;
    if (t->seq_abs != SEQ_NONE)
        seq_ratchets(t, rel < 0 ? 0u : (uint32_t)rel, slen);
}

/* MIDI in: the track a channel plays (0..15) */
static track_t *midi_track(uint32_t ch)
{
    if (song.g[G_DRCH] && ch + 1u == (uint32_t)song.g[G_DRCH])
        return TDRUM;
    return ch < NPART ? &trk[ch] : TSEL;
}

/* a channel that plays the selected track: its note-off goes to the track its note-on went to,
 * even when another track was selected in between (else that note would hang) */
static uint8_t midi_sel_on[16][128];                  /* per channel and note: track + 1, 0 = none */
static track_t *midi_route(uint32_t ch, uint32_t note, int on)
{
    track_t *t = midi_track(ch);
    if (ch < NPART || (song.g[G_DRCH] && ch + 1u == (uint32_t)song.g[G_DRCH]))
        return t;                                     /* a part's own channel, or the drum channel */
    if (on)
        midi_sel_on[ch & 15u][note & 127u] = (uint8_t)(song.sel + 1u);
    else if (midi_sel_on[ch & 15u][note & 127u]) {
        t = &trk[(midi_sel_on[ch & 15u][note & 127u] - 1u) % NTRK];
        midi_sel_on[ch & 15u][note & 127u] = 0;
    }
    return t;
}

/* MIDI clock in (GLO > SYSTEM > SYNC = USB or TRS; after Felucca 1.0's midi_clock.c, from contributions by
 * ChanceTheMaker and keremimo): 24 pulses a beat. While the clock runs, the sequencer advances by the
 * pulses (a pulse = BEAT_U / 24 units), interpolated up to the next one from the last interval but never
 * past it, so it follows the master's tempo changes and cannot drift; BPM shows the master's tempo
 * (the slicer, delay and arp follow it). START restarts from the top, CONTINUE carries on where it
 * stopped, STOP stops. With no pulse for 0.5 s, the internal tempo takes over (PLAY works as ever). */
#define MCLK_PULSE_U (BEAT_U / 24u)
static struct {
    uint32_t pos, done;          /* units: the master's position (pulses since START), ours */
    uint32_t last_ms, iv_ms;     /* the last pulse, the interval between pulses (smoothed) */
    uint32_t beat_ms;            /* when pulse 0 of the last 24 came: the tempo */
    uint8_t have, n24;           /* a pulse since START; pulses towards the next tempo reading */
    uint8_t alive;               /* pulses are coming (from the SYNC source) */
} mclk;

static int mclk_on(void)                          /* the clock drives the sequencer */
{
    return song.g[G_SYNC] && mclk.alive && fm1_ms - mclk.last_ms < 500u;
}

static void mclk_event(uint32_t st, uint32_t src)  /* a realtime message; src 1 USB, 2 TRS */
{
    uint32_t now = fm1_ms;
    if (!song.g[G_SYNC] || src != (uint32_t)song.g[G_SYNC])
        return;
    if (st == 0xFAu || st == 0xFBu) {              /* START: from the top; CONTINUE: on from where it stopped */
        mclk.pos = mclk.done = 0;
        mclk.have = 0;
        if (st == 0xFAu)
            transport_req = 3;                     /* (not 1: the master counts, never a count-in) */
        else if (!song.playing)
            song.playing = 1;
        return;
    }
    if (st == 0xFCu) {                             /* STOP */
        transport_req = 2;
        return;
    }
    if (st != 0xF8u)
        return;
    if (mclk.alive && now - mclk.last_ms < 200u) { /* the interval, smoothed (a gap is not a tempo) */
        uint32_t iv = now - mclk.last_ms;
        mclk.iv_ms = mclk.iv_ms ? (mclk.iv_ms * 3u + iv + 2u) / 4u : iv;
    }
    if (!mclk.alive || now - mclk.last_ms >= 500u) {   /* (re)started: count a fresh beat */
        mclk.n24 = 0;
        mclk.beat_ms = now;
    } else if (++mclk.n24 == 24u) {                /* a beat: the tempo */
        uint32_t dt = now - mclk.beat_ms;
        mclk.n24 = 0;
        mclk.beat_ms = now;
        if (dt >= 250u && dt <= 1500u)             /* 40..240 BPM */
            song.g[G_BPM] = (int16_t)clamp((int32_t)((60000u + dt / 2u) / dt), 40, 240);
    }
    mclk.alive = 1;
    mclk.last_ms = now;
    if (song.playing || transport_req == 1u || transport_req == 3u) {   /* (a START queued with it: the next block starts) */
        if (mclk.have)
            mclk.pos += MCLK_PULSE_U;
        mclk.have = 1;                             /* the first pulse after START is the downbeat */
    }
}

static uint32_t mclk_adv(uint32_t n)               /* units to advance this block (mclk_on) */
{
    uint32_t el, off = 0, tgt, adv, cap = n * 2u * (uint32_t)song.g[G_BPM];
    if (!mclk.have)
        return 0;                                  /* START seen: wait for the downbeat */
    el = fm1_ms - mclk.last_ms;
    if (mclk.iv_ms) {
        if (el > mclk.iv_ms)
            el = mclk.iv_ms;
        off = el * MCLK_PULSE_U / mclk.iv_ms;      /* (<= 200 x 110250: fits 32 bits) */
        if (off >= MCLK_PULSE_U)
            off = MCLK_PULSE_U - 1u;
    }
    tgt = mclk.pos + off;
    adv = (int32_t)(tgt - mclk.done) > 0 ? tgt - mclk.done : 0u;
    if (adv > cap)
        adv = cap;                                 /* behind: catch up at twice the tempo, no burst */
    mclk.done += adv;
    return adv;
}

static void midi_cc(uint32_t ch, uint32_t cc, uint32_t val) {
    if (cc >= 102u && cc <= 133u) {
        int idx = (int)cc - 102;
        if (idx < (int)PUNCH_NFX) {
            if (val > 0u) {
                punch.req     = (int8_t)idx;
                punch.cc_hold = 1u;
            } else {
                if (punch.req == idx) {
                    punch.cc_hold = 0u;
                    punch.req     = -1;
                }
            }
        }
        return;
    }
    if (ch < NPART) {
        track_t *t = &trk[ch];
        if (cc == 74u) t->p[P_E4] = (int16_t)val;
        else if (cc == 71u) t->p[P_E5] = (int16_t)val;
    } else if (ch == 15u) {
        switch (cc) {
        case 20u: song.g[G_SWING]  = (int16_t)(val > 100u ? 100u : val); break;
        case 24u: song.g[G_DFDBK]  = (int16_t)val; break;
        case 25u: song.g[G_DCOLOR] = (int16_t)val; break;
        case 26u: song.g[G_DMIX]   = (int16_t)val; break;
        case 27u: song.g[G_RSIZE]  = (int16_t)val; break;
        case 28u: song.g[G_RDAMP]  = (int16_t)val; break;
        case 29u: song.g[G_CRATE]  = (int16_t)val; break;
        case 30u: song.g[G_CDEPTH] = (int16_t)val; break;
        case 71u: song.g[G_DUST]   = (int16_t)val; break;
        case 74u: song.g[G_FILT]   = (int16_t)((int32_t)val - 64); break;
        }
    }
}

/* everything that happens between two rendered blocks: transport, input, the steps of every
 * track at the clock, the click, the rolls and the arps; then the clock moves on by n samples */
static void events_block(uint32_t n)
{
    uint32_t i, pr, adv;
    if (mo_any && !song.g[G_MIDI]) {            /* MIDI = KEYS again: end what the sequencer had sent */
        seq_out_all_off();
        mo_any = 0;
    }
    if (transport_req == 1u || transport_req == 3u) {
        uint32_t ext = transport_req == 3u;         /* a MIDI START: the master counts; cut a count-in short */
        transport_req = 0;
        if (ext && ci_on)
            ci_on = 0;
        if (ft_on) {
            ft_close();                             /* (PLAY from elsewhere: the editor) */
        } else if (ci_on) {
            ci_on = 0;                              /* PLAY again during the count-in: back to armed */
        } else if (!ext && rec_wait && rec_count && !song.playing && !mclk_on() &&
                   !(project_empty() && !rec_tempo)) {
            ci_on = 1;                              /* COUNT: one bar of clicks first (below) */
            ci_u = 0;
            ci_beat = 0;
            drum_on(77u, 120u);
        } else {
            seq_start();
            if (rec_wait && song.playing)
                rec_begin();                        /* PLAY while armed: record from the top */
        }
    } else if (transport_req == 2u) {
        seq_stop();
        transport_req = 0;
        song.rec = 0;                               /* STOP ends the take (and the wait) */
        rec_wait = 0;
        if (ft_on) {
            ft_on = 0;                              /* a free take: dropped */
            ft_bars = 0xFF;
        }
    }
    if (ci_on) {                                    /* the count-in: 4 beats at the tempo, then go */
        if (!rec_wait || song.playing) {
            ci_on = 0;                              /* (REC cancelled it, or it started some other way) */
        } else {
            uint32_t b;
            ci_u += n * (uint32_t)song.g[G_BPM];
            b = ci_u / BEAT_U;
            if (b >= 4u) {
                ci_on = 0;
                seq_start();
                if (song.playing)
                    rec_begin();
            } else if (b != ci_beat) {
                ci_beat = (uint8_t)b;
                drum_on(76u, 72u);
            }
        }
    }
    if (rec_wait && song.playing)
        rec_begin();                                /* started some other way: record now */
    adv = mclk_on() && song.playing ? mclk_adv(n) : n * (uint32_t)song.g[G_BPM];   /* (after a START) */
    ft_block();
#if FELUCCA_ARRANGER
    if (song.playing && arrangement_clock.running) {
        int scene = arr_next(&arrangement_clock, &arrangement, FS);
        if (scene == ARR_DONE) {
            arrangement_clock.running = 1;          /* (arr_next cleared it: seq_stop brings the loop back) */
            seq_stop();
        }
        else if (scene >= 0) {
            arrangement_apply((uint32_t)scene);
            punch.song = arrangement.entry[scene].rsv[0] == 0xFFu ? (int8_t)-1 : (int8_t)arrangement.entry[scene].rsv[0];
            arr_song_transpose = (int8_t)arrangement.entry[scene].rsv[1];
            arr_song_trans_inkey = arrangement.entry[scene].rsv[2] & ARR_FLAG_INKEY;
            seq_reset_tracks(arrangement_clock.phase);   /* (the remainder: exactly on the bar) */
        }
    } else if (song.playing) {
        live_block();
    }
#endif
    pr = panic_req;
    panic_req = 0;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if ((pr >> i) & 1u) {
            trk_all_off(t);
            t->nheld = 0;
        }
        if (i < NPART)
            engine_block(t);                          /* engine switch: fade, then switch (voice.c) */
    }
    keyboard_block();
    strum_block(n);                                   /* (voice.c: the strummed notes due) */
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, ch = (pkt >> 8) & 0x0Fu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        track_t *t;
        mi_r++;
        if ((pkt & 15u) == 0xFu) {                    /* clock / transport: cable 0 USB, 1 TRS */
            mclk_event((pkt >> 8) & 0xFFu, ((pkt >> 4) & 15u) ? 2u : 1u);
            continue;
        }
        if (st == 0xB0u) {
            midi_cc(ch, d1, d2);
            continue;
        } else if (st != 0x90u && st != 0x80u) {
            continue;
        }
        if (song.g[G_ROUTE] && st == 0x90u && d2)
            continue;                                 /* GLO > SYSTEM > IN = CLOCK: no notes */
        t = midi_route(ch, d1, st == 0x90u && d2);
        if (is_drum(t)) {
            if (st == 0x90u && d2)
                drum_input(lane_of_note(d1), vel_lvl(d2), 0, 1);
        } else if (st == 0x90u && d2) {
            input_on(t, d1, d2);
        } else {
            input_off(t, d1);
        }
    }
    if (song.playing) {
        uint32_t bar = clk_beat / 4u;
        if (bar != fill_last_bar) {
            fill_last_bar = bar;
            if (fill_arm) {
                fill_bar_on = 1;
                fill_arm = 0;
            } else {
                fill_bar_on = 0;
            }
        }
    }
    fill_now = (uint8_t)(fill_held || fill_bar_on);
    cseq_tick();
    for (i = 0; i < NTRK; i++)
        seq_tick(&trk[i], adv);
    click_tick();
    if (song.playing) {
        song.tick++;
        clk_pos += adv;
        while (clk_pos >= BEAT_U) {
            clk_pos -= BEAT_U;
            clk_beat++;
        }
#if FELUCCA_ARRANGER
        arr_elapse(&arrangement_clock, adv, 1u);   /* (units: n x BPM, or the MIDI clock) */
#endif
    }
}
