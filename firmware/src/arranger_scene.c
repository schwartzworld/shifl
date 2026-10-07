/* SPDX-License-Identifier: GPL-3.0-only */
/* Shared scene application for firmware and host audio integration tests. */
static uint32_t arrangement_ready(void)
{
    uint32_t i, ready = 0;
    for (i = 0; i < ARR_SCENES; i++)
        if (proj_ok(&proj_slot[i])) ready |= 1u << i;
    return ready;
}

/* Apply one track from a project slot into the live track, clamping all values. */
static void proj_apply_track(const project_t *p, uint32_t k)
{
    uint32_t i;
    track_t *t = &trk[k];
    const proj_trk_t *s = &p->t[k];
    uint32_t e = k < NPART ? s->engine % NENGINES : 0u;
    t->eng_req = (uint8_t)e;
    t->user = 0;
    for (i = 0; i < P_COUNT; i++) {
        const param_desc_t *d = k == TRK_DRUM && i == P_E0 ? &DRUM_KIT_DESC :
                                i >= P_E0 && i <= P_E7 ? &ENGINES[e]->edit[i - P_E0] : &TP[i];
        t->p[i] = (int16_t)clamp(s->p[i], d->min, d->max);
    }
    t->preset = (uint8_t)(ENGINES[e]->npresets ? (s->preset == 0xFFu ? 0u : s->preset) % ENGINES[e]->npresets : 0u);
    memcpy(t->step, s->step, sizeof t->step);
    if (k != TRK_DRUM)
        for (i = 0; i < NSTEP; i++) {
            step_t *st = &t->step[i];
            uint32_t j;
            if (st->n > 4u) st->n = 4;
            if (st->time > ST_P25) st->time = ST_REST;
            for (j = 0; j < 4u; j++) st->note[j] &= 127u;
        }
}

static void trk_clear_state(track_t *t)
{
    seq_release(t);
    trk_all_off(t);
    t->nheld = t->arp_phys = t->arp_note = t->rh_n = t->rskip_n = 0;
    t->rskip_lanes = 0;
}

/* the bars a section's loop takes (its longest pattern: ceil(LEN x step / bar), at least 1): the quick
 * chain (seq.c chain_*) plays each of its sections this long */
static uint32_t section_bars(uint32_t s)
{
    const project_t *p = &proj_slot[s & 3u];
    uint32_t k, bars = 1;
    if (!proj_ok(p))
        return 1;
    for (k = 0; k < NTRK; k++) {
        uint32_t len = (uint32_t)clamp(p->t[k].p[P_SLEN], 1, NSTEP), u = div_units((uint32_t)p->t[k].p[P_SDIV] % NDIV_STEP);
        uint32_t b = (len * u + 4u * BEAT_U - 1u) / (4u * BEAT_U);   /* (64 x 8 beats fits 32 bits) */
        if (b > bars)
            bars = b;
    }
    return bars > 64u ? 64u : bars;
}

/* Per-entry section switch: each track independently picks its scene or mutes.
 * Called only at an audio-block boundary after start-time validation. The song
 * keeps one tempo and global FX (only drum level/reverb come from the section). */
static void arrangement_apply(uint32_t entry_index)
{
    uint32_t k;
    const arr_entry_t *e = &arrangement.entry[entry_index];
    const arr_patch_t *pa = &arrangement.patch[entry_index];
    for (k = 0; k < NTRK; k++) {
        trk_clear_state(&trk[k]);
        if (e->track[k] == ARR_MUTE) {
            trk[k].p[P_LEVEL] = 0;     /* P_LEVEL=0 silences the track in the mixer */
            if (k == TRK_DRUM)
                trk[k].p[P_MUTE] = 1;  /* drum track uses trk_silent/att, not P_LEVEL */
        } else {
            proj_apply_track(&proj_slot[e->track[k]], k);
            if (k < NPART && pa->engine[k] != ARR_PATCH_NONE) {
                trk[k].eng_req = (uint8_t)(pa->engine[k] % NENGINES);
                if (pa->preset[k] != ARR_PATCH_NONE)
                    apply_preset_to(&trk[k], pa->preset[k]);
            }
        }
    }
    sync_reload = 1;
    ui.force = 1;
}

/* Live section jump (SAVE+key): all tracks from one scene, no per-track split. */
static void arrangement_apply_scene(uint32_t scene)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++)
        trk_clear_state(&trk[k]);
    proj_apply(&proj_slot[scene], 0);
    sync_reload = 1;
    ui.force = 1;
}

/* ---- song mode keeps the loop you made: PLAY in song mode puts it aside (each section then plays
 * over the tracks), STOP (or the song's end) brings it back */
static project_t song_keep __attribute__((section(".pool")));
static uint8_t song_kept;
static void song_backup(void)                  /* (audio ISR: seq_start) */
{
    proj_capture(&song_keep);
    song_kept = 1;
}
static void song_restore(void)                 /* (audio ISR: seq_stop) */
{
    uint32_t i;
    if (!song_kept)
        return;
    song_kept = 0;
    for (i = 0; i < NTRK; i++) {
        trk_all_off(&trk[i]);
        trk[i].nheld = trk[i].arp_phys = trk[i].arp_note = 0;
    }
    proj_apply(&song_keep, 1);
    song.sel = (uint8_t)(song_keep.sel < NTRK ? song_keep.sel : 0u);
    sync_reload = 1;
    ui.force = 1;
}
