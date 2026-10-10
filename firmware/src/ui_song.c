/* SPDX-License-Identifier: GPL-3.0-only */
/* Compact 240 x 240 song-order editor. Each entry shows one header row (bars)
 * and four track rows (one per track) so each track independently picks a scene
 * (A-D) or MUTE. All changes and flash saves require stopped transport. */
static uint8_t song_cursor, song_sub;
static uint8_t song_store_armed, song_load_armed;
static uint32_t song_store_deadline;

static uint32_t song_patch_count(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NENGINES; i++) n += ENGINES[i]->npresets;
    return n;
}
static void song_flat_to_ep(uint32_t flat, uint8_t *eng, uint8_t *pst)
{
    uint32_t i;
    for (i = 0; i < NENGINES; i++) {
        if (flat < (uint32_t)ENGINES[i]->npresets) break;
        flat -= (uint32_t)ENGINES[i]->npresets;
    }
    *eng = (uint8_t)(i < NENGINES ? i : NENGINES - 1u);
    *pst = (uint8_t)flat;
}
static int32_t song_ep_to_flat(uint8_t eng, uint8_t pst)
{
    uint32_t i; int32_t flat = 0;
    for (i = 0; i < (uint32_t)eng && i < NENGINES; i++) flat += (int32_t)ENGINES[i]->npresets;
    return flat + (int32_t)pst;
}
static int on_song_page(void) { return !ui.home && cur_page()->scope == SC_SONG; }
static void song_sane(void)
{
    uint32_t i, k, ok = arrangement.count >= 1u && arrangement.count <= ARR_STEPS;
    for (i = 0; ok && i < arrangement.count; i++) {
        const arr_entry_t *e = &arrangement.entry[i];
        ok = e->bars >= 1u && e->bars <= 128u;
        for (k = 0; ok && k < ARR_TRACKS; k++)
            ok = e->track[k] == ARR_MUTE || e->track[k] < ARR_SCENES;
    }
    if (!ok) arr_defaults(&arrangement);
    if (song_cursor >= arrangement.count)
        song_cursor = (uint8_t)(arrangement.count - 1u);
    if (song_sub > ARR_TRACKS + 10u) song_sub = 0;
}

/* Scene targeted by REC/LOAD: the selected track's scene, or the first non-muted
 * track when on the bars row. Returns 0 (false) if the target track is muted. */
static int song_ref_scene(uint32_t *out)
{
    const arr_entry_t *e = &arrangement.entry[song_cursor];
    uint32_t k = (song_sub > 0u && song_sub <= ARR_TRACKS) ? (uint32_t)(song_sub - 1u) : 0u;
    if (e->track[k] == ARR_MUTE) return 0;
    *out = e->track[k];
    return 1;
}

/* Insert a copy of the current fragment after it, shifting subsequent ones. */
static void song_clone_fragment(void)
{
    uint32_t i;
    if (song.playing || transport_req) { ui_message("STOP FIRST"); return; }
    if (arrangement.count >= ARR_STEPS) { ui_message("SONG FULL"); return; }
    for (i = arrangement.count; i > (uint32_t)song_cursor + 1u; i--) {
        arrangement.entry[i] = arrangement.entry[i - 1u];
        arrangement.patch[i] = arrangement.patch[i - 1u];
        memcpy(arr_song_vol[i], arr_song_vol[i - 1u], ARR_TRACKS);
    }
    arrangement.entry[song_cursor + 1u] = arrangement.entry[song_cursor];
    arrangement.patch[song_cursor + 1u] = arrangement.patch[song_cursor];
    memcpy(arr_song_vol[song_cursor + 1u], arr_song_vol[song_cursor], ARR_TRACKS);
    arrangement.count++;
    song_cursor++;
    ui_message("CLONED");
}

/* SONG: each entry as a header row (bars) + four track rows */
static void song_screen_draw(void)
{
    song_sane();
    static uint32_t previous;
    static const uint16_t SC[ARR_SCENES] = {RGB(40, 124, 255), RGB(30, 204, 112), RGB(255, 198, 24), RGB(255, 98, 26),
                                            RGB(180, 80, 220), RGB(0, 200, 220)};
    uint32_t i, k;
    uint32_t sig = (uint32_t)song_cursor + 17u * arrangement_enabled + 37u * song.playing +
                   71u * arrangement_clock.index + 127u * arrangement_clock.bar +
                   257u * arrangement.count + 509u * (uint32_t)(uint16_t)song.g[G_BPM] +
                   1021u * song_sub;
    char b[30];
    for (i = 0; i < arrangement.count; i++) {
        const arr_entry_t *e = &arrangement.entry[i];
        sig = sig * 31u + e->bars;
        for (k = 0; k < ARR_TRACKS; k++) sig = sig * 7u + e->track[k];
        sig = sig * 5u + e->rsv[0];
        sig = sig * 3u + (uint32_t)(uint8_t)e->rsv[1];
        sig = sig * 2u + e->rsv[2];
        for (k = 0; k < ARR_TRACKS; k++) {
            sig = sig * 11u + arrangement.patch[i].engine[k];
            sig = sig * 13u + arrangement.patch[i].preset[k];
            sig = sig * 17u + arr_song_vol[i][k];
        }
    }
    for (i = 0; i < ARR_SCENES; i++) sig = sig * 3u + (uint32_t)project_used(i);
    if (!ui.force && !ui.msg_t && sig == previous) return;
    previous = ui.msg_t ? ~sig : sig;

    /* Header */
    cv_begin(240, 40, C_BLACK);
    cv_text(4, 4, &FONT_L, "SONG", C_WHITE);
    cv_text(76, 20, &FONT_S, arrangement_enabled ? "song mode" : "loop mode",
            arrangement_enabled ? SC[3] : RGB(118, 118, 126));
    fmt_int(b, song.g[G_BPM]);
    cv_text(236 - text_w(&FONT_S, b) - 28, 4, &FONT_S, b, C_WHITE);
    cv_text(236 - 24, 4, &FONT_S, "bpm", RGB(118, 118, 126));
    cv_text(236 - text_w(&FONT_S, song.playing ? "playing" : "stopped"), 20, &FONT_S,
            song.playing ? "playing" : "stopped", song.playing ? SC[1] : RGB(118, 118, 126));
    cv_rect(0, 39, 240, 1, RGB(26, 26, 30));
    cv_blit(0, 0);

    /* Entry header row (sub=0): entry number, bar-length viz, 4 mini track tiles */
    {
        const arr_entry_t *e = &arrangement.entry[song_cursor];
        int sel = (song_sub == 0u);
        int32_t w = (int32_t)e->bars * 100 / 128 + 4;
        cv_begin(240, 26, C_BLACK);
        fmt_int(b, (int32_t)song_cursor + 1);
        cv_text(4, 5, &FONT_S, b, C_WHITE);
        cv_rect(26, 9, w, 8, sel ? RGB(180, 180, 190) : RGB(54, 54, 60));
        { uint32_t whole = e->bars / 2u, half = e->bars & 1u;
          if (whole) { fmt_int(b, (int32_t)whole); if (half) str_cpy(b + str_len(b), ".5", 4); }
          else str_cpy(b, "0.5", 4);
          str_cpy(b + str_len(b), (whole == 1u && !half) ? " bar" : " bars", 8); }
        cv_text(26 + w + 4, 5, &FONT_S, b, sel ? C_WHITE : RGB(118, 118, 126));
        /* mini tiles for all 4 tracks at right edge */
        for (k = 0; k < ARR_TRACKS; k++) {
            uint8_t tr = e->track[k];
            uint16_t c16 = (tr == ARR_MUTE) ? RGB(40, 40, 44) :
                           project_used(tr) ? SC[tr] : RGB(60, 60, 66);
            cv_rect((int32_t)(188 + k * 13), 8, 10, 10, c16);
        }
        if (arrangement_clock.running && arrangement_clock.index == song_cursor)
            cv_rect(26, 19, (int32_t)(arrangement_clock.bar + 1u) * w / (e->bars ? e->bars : 1), 2, C_WHITE);
        if (sel) cv_rect(0, 1, 240, 1, RGB(54, 54, 60)), cv_rect(0, 24, 240, 1, RGB(54, 54, 60));
        cv_blit(0, 42);
    }

    /* Four track rows (sub=1-4) */
    for (k = 0; k < ARR_TRACKS; k++) {
        const arr_entry_t *e = &arrangement.entry[song_cursor];
        uint8_t tr = e->track[k];
        int sel = (song_sub == (uint8_t)(k + 1u));
        uint16_t tsc = SC[k];
        cv_begin(240, 26, C_BLACK);
        b[0] = (char)('1' + k); b[1] = 0;
        cv_text(4, 5, &FONT_S, b, tsc);
        if (tr == ARR_MUTE) {
            cv_rect(26, 3, 22, 20, RGB(26, 26, 30));
            cv_text(30, 5, &FONT_S, "--", RGB(80, 80, 88));
            cv_text(56, 5, &FONT_S, "mute", sel ? C_WHITE : RGB(80, 80, 88));
        } else {
            int used = project_used(tr);
            uint16_t sc16 = used ? SC[tr] : RGB(26, 26, 30);
            cv_rect(26, 3, 22, 20, sc16);
            b[0] = (char)('A' + tr); b[1] = 0;
            cv_text(33, 5, &FONT_S, b, used ? C_BLACK : SC[tr]);
            if (!used) cv_text(56, 5, &FONT_S, "empty", sel ? C_WHITE : RGB(118, 118, 126));
        }
        if (sel) cv_rect(0, 1, 240, 1, RGB(54, 54, 60)), cv_rect(0, 24, 240, 1, RGB(54, 54, 60));
        cv_blit(0, (int32_t)(68 + 26u * k));
    }

    /* FX row (sub=5): punch-in effect for this fragment */
    {
        const arr_entry_t *e = &arrangement.entry[song_cursor];
        int sel = (song_sub == (uint8_t)(ARR_TRACKS + 1u));
        uint8_t fx = e->rsv[0];
        cv_begin(240, 26, C_BLACK);
        cv_text(4, 5, &FONT_S, "FX", RGB(118, 118, 126));
        if (fx == 0xFFu) {
            cv_rect(26, 3, 22, 20, RGB(26, 26, 30));
            cv_text(30, 5, &FONT_S, "--", RGB(80, 80, 88));
            cv_text(56, 5, &FONT_S, "none", sel ? C_WHITE : RGB(80, 80, 88));
        } else {
            cv_rect(26, 3, 92, 20, RGB(40, 40, 52));
            cv_text(30, 5, &FONT_S, PUNCH_NAME[fx], sel ? C_WHITE : RGB(180, 180, 220));
        }
        if (sel) cv_rect(0, 1, 240, 1, RGB(54, 54, 60)), cv_rect(0, 24, 240, 1, RGB(54, 54, 60));
        cv_blit(0, 172);
    }

    /* Bottom detail area (sub=6..11) replaces footer; footer shown for sub 0..5 */
    if (song_sub == (uint8_t)(ARR_TRACKS + 2u)) {
        /* Transpose (sub=6) */
        const arr_entry_t *e = &arrangement.entry[song_cursor];
        int8_t tr = (int8_t)e->rsv[1];
        cv_begin(240, 42, C_BLACK);
        cv_text(4, 11, &FONT_S, "TRANS", RGB(118, 118, 126));
        if (tr == 0) {
            cv_rect(60, 9, 22, 20, RGB(26, 26, 30));
            cv_text(64, 11, &FONT_S, "0", RGB(80, 80, 88));
        } else {
            char tb[8];
            if (tr > 0) { tb[0] = '+'; fmt_int(tb + 1, tr); }
            else fmt_int(tb, tr);
            cv_rect(60, 9, 52, 20, RGB(40, 52, 40));
            cv_text(64, 11, &FONT_S, tb, RGB(120, 220, 120));
        }
        cv_rect(0, 1, 240, 1, RGB(54, 54, 60));
        cv_blit(0, 198);
    } else if (song_sub == (uint8_t)(ARR_TRACKS + 3u)) {
        /* Key mode (sub=7): chromatic vs in-key transposition */
        const arr_entry_t *e = &arrangement.entry[song_cursor];
        int inkey = (e->rsv[2] & ARR_FLAG_INKEY) ? 1 : 0;
        cv_begin(240, 42, C_BLACK);
        cv_text(4, 11, &FONT_S, "KEY", RGB(118, 118, 126));
        if (!inkey) {
            cv_rect(60, 9, 40, 20, RGB(26, 26, 30));
            cv_text(64, 11, &FONT_S, "CHR", RGB(80, 80, 88));
            cv_text(108, 11, &FONT_S, "chromatic", RGB(80, 80, 88));
        } else {
            cv_rect(60, 9, 40, 20, RGB(40, 52, 40));
            cv_text(64, 11, &FONT_S, "KEY", RGB(120, 220, 120));
            cv_text(108, 11, &FONT_S, "in key", RGB(120, 220, 120));
        }
        cv_rect(0, 1, 240, 1, RGB(54, 54, 60));
        cv_blit(0, 198);
    } else if (song_sub >= (uint8_t)(ARR_TRACKS + 4u) && song_sub < (uint8_t)(ARR_TRACKS + 4u + NPART)) {
        /* Patch override rows (sub=8..10, synth tracks 1-3 only) */
        uint32_t pk = (uint32_t)song_sub - (ARR_TRACKS + 4u);
        const arr_patch_t *pa = &arrangement.patch[song_cursor];
        cv_begin(240, 42, C_BLACK);
        b[0] = 'P'; b[1] = (char)('1' + pk); b[2] = 0;
        cv_text(4, 11, &FONT_S, b, SC[pk]);
        if (pa->engine[pk] == ARR_PATCH_NONE) {
            cv_rect(36, 9, 26, 20, RGB(26, 26, 30));
            cv_text(40, 11, &FONT_S, "--", RGB(80, 80, 88));
            cv_text(70, 11, &FONT_S, "default", RGB(80, 80, 88));
        } else {
            uint32_t eng = (uint32_t)pa->engine[pk] % NENGINES;
            uint32_t pst = ENGINES[eng]->npresets ? (uint32_t)pa->preset[pk] % ENGINES[eng]->npresets : 0u;
            str_cpy(b, ENGINES[eng]->name, 12);
            str_cpy(b + str_len(b), " ", 2);
            str_cpy(b + str_len(b), ENGINES[eng]->presets[pst].name, 17);
            cv_rect(36, 9, 196, 20, RGB(40, 40, 52));
            cv_text(40, 11, &FONT_S, b, RGB(180, 180, 220));
        }
        cv_rect(0, 1, 240, 1, RGB(54, 54, 60));
        cv_blit(0, 198);
    } else if (song_sub >= (uint8_t)(ARR_TRACKS + 4u + NPART)) {
        /* Vol override rows (sub=11..14, all 4 tracks) */
        uint32_t vk = (uint32_t)song_sub - (ARR_TRACKS + 4u + NPART);
        uint8_t vv = arr_song_vol[song_cursor][vk];
        cv_begin(240, 42, C_BLACK);
        b[0] = 'V'; b[1] = (char)('1' + vk); b[2] = 0;
        cv_text(4, 11, &FONT_S, b, SC[vk]);
        if (vv == ARR_VOL_NONE) {
            cv_rect(36, 9, 26, 20, RGB(26, 26, 30));
            cv_text(40, 11, &FONT_S, "--", RGB(80, 80, 88));
            cv_text(70, 11, &FONT_S, "default", RGB(80, 80, 88));
        } else {
            char vb[8];
            fmt_int(vb, (int32_t)vv);
            cv_rect(36, 9, 52, 20, RGB(40, 52, 40));
            cv_text(40, 11, &FONT_S, vb, RGB(120, 220, 120));
            cv_text(96, 11, &FONT_S, "vol override", RGB(80, 80, 88));
        }
        cv_rect(0, 1, 240, 1, RGB(54, 54, 60));
        cv_blit(0, 198);
    } else {
        cv_begin(240, 42, C_BLACK);
        if (ui.msg_t) {
            cv_rect(0, 4, 240, 30, C_WHITE);
            cv_text((240 - text_w(&FONT_S, ui.msg)) / 2, 11, &FONT_S, ui.msg, C_BLACK);
        } else {
            static const char *const L[4] = {"entry", "field", "value", "length"};
            for (i = 0; i < 4u; i++) {
                cv_rect((int32_t)i * 60 + 4, 2, 52, 3, SC[i]);
                cv_text((int32_t)i * 60 + 30 - text_w(&FONT_S, L[i]) / 2, 8, &FONT_S, L[i], RGB(118, 118, 126));
            }
            cv_text(4, 22, &FONT_S, "rec: store   save: chain", RGB(196, 196, 204));
            cv_text(4, 33, &FONT_S, "oct-: loop/song  arp: clone", RGB(118, 118, 126));
        }
        cv_blit(0, 198);
    }
}

static void song_screen_input(uint32_t pressed, uint32_t home)
{
    uint32_t k, b;
    int32_t steps;
    song_sane();
    if (home == 1u) { go_home(); return; }
    for (k = 0; k < NB; k++) {
        if (!((pressed >> panel.btn[k]) & 1u)) continue;
        b = k;
        if ((b == B_PLAY || b == B_REC) && ft_owns_press()) continue;
        if (b == B_PLAY) {
            if (!song.playing && arrangement_enabled && !arr_valid(&arrangement, arrangement_ready()))
                ui_message("EMPTY SECTION: REC");
            else {
                if (!song.playing && arrangement_enabled)
                    arrangement_start_index = song_cursor;
                transport_req = song.playing ? 2 : 1;
            }
        } else if (b == B_SEQ) {
            open_family(FAM_SEQ);
        } else if (b == B_SAVE || b == B_REC || b == B_OCTDN || b == B_OCTUP) {
            uint32_t scene = 0;
            if (song.playing || transport_req) { ui_message("STOP FIRST"); continue; }
            if (b == B_SAVE) {
                arrangement_save();
                song_store_armed = 0;
            } else if (b == B_OCTDN) {
                arrangement_enabled ^= 1u;
                ui_message(arrangement_enabled ? "SONG MODE" : "LOOP MODE");
            } else if (b == B_OCTUP) {
                if (!song_ref_scene(&scene)) { ui_message("TRACK IS MUTED"); continue; }
                if (song_load_armed && (int32_t)(fm1_ms - song_store_deadline) <= 0) {
                    song_load_armed = 0;
                    project_load(scene);
                } else {
                    song_load_armed = 1;
                    song_store_armed = 0;
                    song_store_deadline = fm1_ms + 3000u;
                    ui_message("OCT+ AGAIN: LOAD");
                }
            } else {    /* B_REC */
                if (!song_ref_scene(&scene)) { ui_message("TRACK IS MUTED"); continue; }
                if (project_used(scene) &&
                    (!song_store_armed || (int32_t)(fm1_ms - song_store_deadline) > 0)) {
                    song_store_armed = 1;
                    song_store_deadline = fm1_ms + 3000u;
                    ui_message("REC AGAIN: REPLACE");
                } else {
                    project_save(scene);
                    settings_save();
                    song_store_armed = 0;
                }
            }
        }
    }
    for (k = 0; k < 4u; k++) {
        steps = panel_enc(EN_K1 + k);
        if (!steps) continue;
        song_store_armed = 0;
        song_load_armed = 0;
        if (k == 0) {
            song_cursor = (uint8_t)clamp(song_cursor + steps, 0, arrangement.count - 1);
            continue;
        }
        if (song.playing || transport_req) { ui_message("STOP FIRST"); continue; }
        if (k == 1) {
            song_sub = (uint8_t)clamp((int32_t)song_sub + steps, 0, (int32_t)ARR_TRACKS + 10);
        } else if (k == 2) {
            arr_entry_t *e = &arrangement.entry[song_cursor];
            if (song_sub == 0u) {
                e->bars = (uint8_t)clamp(e->bars + steps, 1, 128);
            } else if (song_sub <= ARR_TRACKS) {
                uint32_t t = (uint32_t)(song_sub - 1u);
                int32_t cur = (int32_t)e->track[t];   /* 0-5=A-F, 6=MUTE */
                e->track[t] = (uint8_t)clamp(cur + steps, 0, (int32_t)ARR_MUTE);
            } else if (song_sub == ARR_TRACKS + 1u) {
                /* FX row: rsv[0] holds punch FX index, 0xFF = none */
                int32_t cur_fx = (e->rsv[0] == 0xFFu) ? -1 : (int32_t)e->rsv[0];
                int32_t new_fx = clamp(cur_fx + steps, -1, (int32_t)PUNCH_NFX - 1);
                e->rsv[0] = (new_fx < 0) ? 0xFFu : (uint8_t)new_fx;
            } else if (song_sub == ARR_TRACKS + 2u) {
                /* Transpose row: rsv[1] holds signed semitone offset */
                int32_t cur_tr = (int8_t)e->rsv[1];
                int32_t new_tr = clamp(cur_tr + steps, -24, 24);
                e->rsv[1] = (uint8_t)(int8_t)new_tr;
            } else if (song_sub == ARR_TRACKS + 3u) {
                /* Key mode: rsv[2] bit 0 toggles chromatic vs in-key */
                int32_t cur_k = (e->rsv[2] & ARR_FLAG_INKEY) ? 1 : 0;
                int32_t new_k = clamp(cur_k + steps, 0, 1);
                e->rsv[2] = (uint8_t)((e->rsv[2] & ~ARR_FLAG_INKEY) | (uint8_t)new_k);
            } else if (song_sub >= ARR_TRACKS + 4u && song_sub < ARR_TRACKS + 4u + NPART) {
                /* Patch override rows (sub=8..10): synth tracks only */
                uint32_t tk = (uint32_t)song_sub - (ARR_TRACKS + 4u);
                arr_patch_t *pa = &arrangement.patch[song_cursor];
                int32_t cur_f = (pa->engine[tk] == ARR_PATCH_NONE) ? -1 :
                                song_ep_to_flat(pa->engine[tk], pa->preset[tk]);
                int32_t new_f = clamp(cur_f + steps, -1, (int32_t)song_patch_count() - 1);
                if (new_f < 0) {
                    pa->engine[tk] = ARR_PATCH_NONE;
                    pa->preset[tk] = ARR_PATCH_NONE;
                } else {
                    uint8_t eng, pst;
                    song_flat_to_ep((uint32_t)new_f, &eng, &pst);
                    pa->engine[tk] = eng;
                    pa->preset[tk] = pst;
                }
            } else if (song_sub >= ARR_TRACKS + 4u + NPART) {
                /* Vol override rows (sub=11..14): all 4 tracks, 0-127 or -1=none */
                uint32_t vk = (uint32_t)song_sub - (ARR_TRACKS + 4u + NPART);
                if (vk < NTRK) {
                    int32_t cur_v = (arr_song_vol[song_cursor][vk] == ARR_VOL_NONE) ? -1 :
                                    (int32_t)arr_song_vol[song_cursor][vk];
                    int32_t new_v = clamp(cur_v + steps, -1, 127);
                    arr_song_vol[song_cursor][vk] = (new_v < 0) ? ARR_VOL_NONE : (uint8_t)new_v;
                }
            }
        } else if (k == 3) {
            arrangement.count = (uint8_t)clamp(arrangement.count + steps, 1, ARR_STEPS);
            if (song_cursor >= arrangement.count) song_cursor = arrangement.count - 1;
        }
    }
    /* Drain unused encoders */
    panel_enc(EN_SELECT); panel_enc(EN_ALGO); panel_enc(EN_PRESET);
}
