# Ian Schwartz — Changes Summary

All committed changes by Ian Schwartz, plus current uncommitted work-in-progress.

---

## Committed Changes

### Microtonal engine (`2026-10-13`)

Added full microtonal support: 54 new scale choices beyond the 16 legacy Western scales, covering equal divisions of the octave (EDO), just intonation, historical temperaments, world music scales, and maqam modes.

- **`firmware/src/scales.h`** (new) — generated scale catalog: `SCALE_TOTAL 70` (16 legacy + 54 new). Each microtonal scale is a `micro_scale_t` with a degree count and a pitch table in hundredths of a cent; the last entry is the period. Covers: 5–53 EDO, JI major/minor/chromatic/7-limit, harmonic series 8 and 16, Partch 43-tone, Pythagorean, quarter/third/sixth-comma meantone, Werckmeister III, Kirnberger III, Vallotti, Young II, Indian shruti (22), Slendro, Pelog, Bohlen-Pierce (equal + just), Rast, Bayati, Hijaz, Saba, Husayni, Sikah, Harrison, and more.
- **`firmware/src/microtonal.h`** (new) — engine that treats 7-bit MIDI note numbers as **scale-degree addresses** rather than chromatic semitones (degree 0 = root at C4 + ROOT + TRANS). `micro_pitch()` resolves any degree to a pitch in hundredths of a cent across any number of octaves. `micro_snap()` snaps a chromatic pitch to the nearest degree. `scale_note_period()` returns the period of the scale (replaces hardcoded 12).
- **`firmware/src/voice.c`** — `voice_micro_pitch16()` converts a degree address to the hardware `pitch16` value via the microtonal pitch table. Active at note-on and during portamento.
- **`firmware/src/seq.c`** — scale quantization, keyboard input (`micro_snap`), chord generation, ARP, generative sequences, and arranger transpose all dispatch through `micro_active()`; microtonal tracks bypass the 12-chromatic degree walk and use the scale's own degree count. Scale index clamped to `SCALE_TOTAL - 1` throughout (was `NSCALES - 1 = 15`). Scale display updated to show all 70 names.

Note identity is unchanged (7-bit note IDs still function as degree addresses), so recorded sequences, chords, independent voice releases and project files are all compatible.

**Bumped to FM-1_967.**

---

### LFO K modulation + wah FX + delay wrap fix (`2026-10-13`)

Three independent improvements shipped together.

**LFO → engine knob modulation (P_LD_K1..K4)**

- **`firmware/src/params.c`** — added `P_LD_K1`..`P_LD_K4` (four new bipolar −64..+63 LFO destination params); new `GR_LDHOME` display group; new **LFO K** page `{"LFO K", FAM_LFO, SC_TRACK, GR_LDHOME, {P_LD_K1..K4}}`. The K1–K4 slots correspond to the engine's four home-page knobs.
- **`firmware/src/voice.c`** — `render()` applies P_LD_K1..K4 by temporarily patching `t->p[e->knob[kk]]` before calling `e->render()` and restoring afterwards. Clamped to the engine edit param's min/max (or 0..127 for engine params).
- **`firmware/src/ui_draw.c`** — `GR_LDHOME` draws the engine-knob labels (from `home_param`) as the column headers with the LFO depth values below.
- **`firmware/src/upreset.c`** — user preset version bumped `UP_VER 3 → 4`; `UP_PMAX 76 → 80` to accommodate P_LD_K1..K4; static-assert updated.
- **`firmware/src/project.c`** — `PROJ_NP_V5OLD 74` marks the layout before P_LD_K1..K4. Migration in `proj_trk_from_v4()` inserts default values for the 4 new params and shifts P_E0..P_E15 (engine params) up by 4. Old FUN5 projects (same magic, 74 params) handled by new `proj_from_v5old()`. `proj_slot[6]` moved to `.pool` (saves ~900 B of BSS RAM).

**Wah FX rework (PX_LPFS..PX_LPFU → WAH SLW/MED/FST/XFS)**

- **`firmware/src/punch.c`** — the four one-shot LPF sweep effects are replaced by four sinusoidal wah LFOs at 0.25 / 0.5 / 1 / 2 Hz. LFO rates stored as unsigned 32-bit accumulators; `punch.lfo` (new field) drives `sine_i()`; filter sweeps cut 45..85 symmetrically around 65.

**Delay buffer wrap fix**

- **`firmware/src/fx.c`** — `DLY_LEN = 44100` is not a power of 2, so the previous bitmask wrap `& (DLY_LEN - 1)` was reading/writing garbage offsets. Fixed: read uses explicit conditional subtract; write uses `if (++dly_w >= DLY_LEN) dly_w = 0`.

**Bumped to FM-1_966.**

---

### Knob reliability + acceleration (`2026-10-11`)

Improved encoder input so fast turns register reliably and large, fast sweeps cover more ground.

- **`firmware/hal/fm1_input.h`** — added `enc_dir` (1-byte bitmask, bit N = last confirmed direction of encoder N) to the input struct. Valid single-step forward/backward transitions now update this bitmask. Previously, a 2-state quadrature jump arriving when `enc_sub == 0` (the first missed transition of a fast turn starting from rest) was silently dropped because neither the `sub > 0` nor `sub < 0` branch could fire; that detent was then under-counted or lost entirely. Now the `enc_dir` bit resolves direction for those jumps.
- **`firmware/src/ui_input.c` — `accel()`** — replaced the single time threshold with three tiers:
  - dt < 15 ms (very fast): 10× for wide-range params, 6× for normal — new tier
  - dt < 60 ms (fast): 6× / 3× — unchanged rates, same threshold
  - dt < 120 ms (moderate): 2× — new tier; replaces the hard stop at 60 ms

**Bumped to FM-1_965.**

---

### KS drum kit + delay trim (`2026-10-11`)

Added a Karplus-Strong membrane drum kit (kit 8, **KS**) and trimmed the global delay buffer to free pool space for it.

- **`firmware/src/eng_ks_drum.c`** (new) — per-lane KS synthesis with a Schroeder allpass comb filter in the feedback path for membrane inharmonicity. 16 lanes, each with a hardcoded base pitch, allpass delay length `M`, loop decay `g`, brightness `S` and comb gain `k`. Hi-hat lanes (4, 5, 6) share choke group 1. `ks_dr_hit()` initialises a noise-burst excitation and computes `N` from `PITCH_INC` (respects `DM_TUNE`). `ks_dr_run()` runs the per-sample loop: loop LP → Schroeder comb → write back.
- **`firmware/src/drums.c`** — `DRUM_KITS` 7 → 8; `KIT_KS = 7`; "KS" / "PHYSICAL" added to name/style arrays; KS choke handling (own table: hats = group 1); `drum_on` KIT_KS block (calls `ks_dr_hit`, sets drums metadata, returns early); render branch (`ks_dr_run`); voice-done check (`drums.quiet[k] >= 8`). Changed `kit >= KIT_808` choke bypass to `kit == KIT_808 || kit == KIT_909` so KS lanes participate in software choke.
- **`firmware/src/fx.c`** — `DLY_LEN` 65536 → 44100 (1.00 s, accurate for quarter-note delay ≥ 60 BPM). Freed 43 KB of pool for the new drum buffers.
- Pool impact: `ks_dr_main[16][600]` + `ks_dr_ap_b[16][200]` + `ks_dr[16]` ≈ 26 KB in `.pool`.

**Bumped to FM-1_962.**

---

### Karplus-Strong synthesis engine (`2026-10-11`)

Added a sixth synth engine — **KARPLUS** — implementing the Karplus-Strong plucked-string algorithm (Karplus & Strong 1983) with Smith/Jaffe fractional-delay extensions. All integer arithmetic (Q15).

- **`firmware/src/eng_ks.c`** (new) — circular delay line filled with white noise, fed back through a two-point weighted averaging filter (`g0*cur + g1*prev`, loop LP) and a first-order allpass for sub-sample period tuning. State in 8 × `voice_t.s[]` slots: buffer index, `N`, allpass states, packed `eta`/`prev`, noise state, `g0`, `g1`. Pool buffer `ks_buf[NPART][KS_NPOLY][KS_BUF_MAX]`. Pluck-position comb applied to the initial noise burst at note_on. 8 presets: NYLON GT, STEEL GT, HARP, BANJO, KALIMBA, SITAR, KOTO, PLUCK BASS.
- **`firmware/src/engines.c`** — `#include "eng_ks.c"` added; `&ENG_KS` appended to `ENGINES[]`.
- **`firmware/src/core.h`** — `NENGINES` 5 → 6; `ENGI_KS = 5`.
- **`tools/build.py`** / **`firmware/src/build.py`** — pool headroom guard lowered from 8192 B to 4096 B to accommodate KS buffers within the available headroom. `KS_NPOLY = 2`, `KS_BUF_MAX = 540` (covers MIDI ≥ 40, E2).

**Bumped to FM-1_961.**

---

### Drum fills (`2026-10-11`)

Hold ARP on a drum track and press white keys 3–8 to trigger one of 6 hardcoded fill patterns. The fill replaces the drum steps from the current position to the end of the pattern loop; when the pattern restarts, normal playback resumes automatically.

Fill alignment: fills are 16-step patterns aligned to the end of the remaining bar, so the crash/landing always falls on the last step regardless of when the fill is triggered. For longer remaining spans (> 16 steps) the fill repeats from the beginning.

- **`drum_fill_pats[6][16]`** (`seq.c`) — 6 hardcoded `dstep_t` fill patterns built at init by `drum_fills_init()`: snare buildup, 16th-hat run, tom roll, snare doublets, kick-snare alternating, full driver.
- **`drum_fill_trigger()` / fill state** (`seq.c`) — `drum_fill_req`, `drum_fill_cur`, `drum_fill_start_abs`, `drum_fill_end_abs`; fill is consumed in the `is_drum` branch of `seq_tick()`.
- **ROLL layer key 3–8** (`ui_layers.c LY_ROLL`) — triggers fills only when `!live_mode && is_drum(TSEL)`; displays "FILL 1"–"FILL 6" message.
- **Draw** (`ui_layers.c LY_ROLL draw`) — shows fill tiles 3–8 on drum track; active fill tile lit in track color.
- **`drum_fills_init()`** (`main.c felucca_init`) — called at boot before `layers_init()`.
- Fill state cleared on transport stop (`seq_stop()`).

**Bumped to FM-1_959 / SHIFL 2.8.**

---

### Live mode scene chaining (`2026-10-10`)

Hold ARP in live mode and tap two or more entry keys to build a scene chain (up to 8 entries). Each scene plays for its configured bar count then auto-advances to the next, looping forever. Releasing ARP commits the chain. A single tap while a chain is playing clears it and jumps to the tapped scene.

- **`lm_chain[]` state** (`seq.c`) — mirrors the existing `chain_sec[]` quick-section chain; auto-advance fires in `live_block()` using `arrangement.entry[s].bars` for timing.
- **ARP held + multi-tap** (`ui_layers.c LY_ROLL`) — first tap queues the scene immediately (existing behaviour); additional taps while ARP is held accumulate into `lm_chain_tap[]`; release calls `lm_chain_release()`.
- **Display** (`ui_layers.c LY_ROLL draw`) — sub-line shows `chain 1 3 5 2` while building or playing; next entry is highlighted with a cyan top marker.
- **`lm_chain_release()` / `lm_chain_sub()`** (`ui_layers.c`) — commit and display helpers mirroring `chain_release()` / `chain_sub()`.
- **ARP button-up hook** (`ui_input.c`) — calls `lm_chain_release()` alongside the existing `chain_release()` on SAVE release.
- Chain clears on transport stop (`seq_stop()`).

**Bumped to FM-1_952.**

---

### LIVE MODE (`2026-10-10`)

A new performance mode for improvised jamming. ARP tap (outside the SONG page) toggles LIVE MODE on/off.

- **16 live scenes** — each corresponds to one of the 16 song-mode fragments (same options: per-track scene assignment, punch FX, transpose, patch/vol/slicer overrides). Fragment 1–16 map directly to keyboard keys 1–16.
- **Looping** — the active scene loops indefinitely; there is no automatic advance.
- **Scene switching while playing** — queued for the next bar boundary; immediate when stopped.
- **Scene selection with keys** — press REC while LIVE MODE is playing to enter scene selection mode (REC LED blinks, grid guides appear). Press any of the 16 white keys to switch scene. Press REC again to go back to playing notes.
- **ARP held overlay** — holding ARP shows 16 numbered tiles; pressing a key also switches scene from this overlay.
- **Knob scroll on SONG page** — scrolling KNOB 1 on the SONG page queues/applies scenes in LIVE MODE.
- **ARP LED** stays lit while LIVE MODE is active.
- **SONG page** — shows "live mode" (cyan) in the header; a cyan marker highlights the currently loaded scene entry.
- Entering song mode (OCT− on SONG page) exits LIVE MODE; entering LIVE MODE disables song mode.

**Bumped to FM-1_948.**

---

### Per-fragment slicer overrides in song mode (`2026-10-10`)

Each arrangement fragment in song mode can now override the slicer mode for every track independently, making the slicer fully sequencable across an arrangement.

- **`S1–S4` rows in SONG screen** — scroll KNOB 2 past the volume rows to reach four new slicer rows, one per track. Set to `--` (default) to leave the scene's slicer setting untouched; dial to `OFF`, `GATE`, or `STUT` to override it for that fragment only.
- **`arr_song_slicer[ARR_STEPS][ARR_TRACKS]`** — new runtime array (mirrors `arr_song_vol`); applied in `arrangement_apply` inside `arranger_scene.c`.
- **`ARR_SLCR_NONE = 0xFF`** — sentinel meaning "no override" (`arranger.h`), matching the `ARR_VOL_NONE` pattern.
- **Persistence** — `arr_slcr[ARR_STEPS][ARR_TRACKS]` appended to `persist_t`; migration on older saves initialises every entry to `ARR_SLCR_NONE` (`project.c`, new `PERSIST_SIZE_V66`).

**Bumped to FM-1_947.**

---

### Arpeggiator removed (`2026-10-10`)

Removed the arpeggiator and note-repeat roll layer entirely.

- **ARP pages removed** — `ARP` and `ARP 2` pages deleted from `params.c`; `FAM_ARP` family is gone.
- **Note-repeat roll removed** — the hold-ARP layer (`LY_ROLL`) and `KS_ROLL` tracking removed from `seq.c` / `ui_input.c` / `ui_layers.c`.
- **Spice & Dice removed** — `dice_t` struct, `trk_dice[]` array, `dice_clear_track()`, `dice_roll()`, `arp_note_of()`, `arp_to_seq()` removed from `seq.c`.
- **`arp_add()` / `arp_remove()` removed** from `seq.c`.
- **MASTER page** — `G_ROLL` knob slot replaced with `0xFF` (unused); MASTER is now DUST, DUCK, FILT only.
- **ARP tap** — now shows `SONG MODE` message (placeholder).

---

### CZ-1 sound design effects — more drastic (`2026-10-09`)

Fixed all four CZ-1 EDIT 2 effects (P_E4–P_E7) that were barely audible:

- **RING** (P_E4) — carrier was `pd_cos((ph>>20)&2047u)`, which only swept 3% of the cosine range and sat near −1, causing cancellation instead of ring modulation. Fixed to `pd_cos(ph>>15)`, which sweeps the full cosine range at double the oscillator frequency.
- **FOLD** (P_E5) — gain was capped at ~3× (520 multiplier) which hits the destructive fold zone for typical CZ amplitudes, going quiet rather than folding. Raised to ~6× (1536 multiplier). Added dry/wet blend (was fully wet) and output rescale ×2 after folding to restore perceived level.
- **BITS** (P_E6) — bit range was 1–15; at low values (1 bit) the effect was inaudible on a ~15-bit signal, and at max (15 bits) the signal was silenced entirely. Changed to range 3–13 for a musically useful span from light quantisation grit to heavy lo-fi.
- **PHSR** (P_E7) — expanded from 2 allpass stages (1 notch) to 4 stages (2 notches) for a fuller phaser character. LFO speed tripled from 859400→2578200 (≈0.28 Hz → ≈0.85 Hz). `phaser_s` array in `cz_voice_t` expanded from 4 to 8 to hold the new state.

**Bumped to FM-1_940.**

---

### Drum level and CZ-1 preset cutoff (`2026-10-09`)

**Drums louder** — The global drum level multiplier in `drums_mix` was `G_DRLVL * 200`, putting drums ~2.4 dB below synth parts at their matching default levels. Changed to `* 258` (= Q15 unity at max), which aligns drums with synth parts at their respective default settings (G_DRLVL=100, P_LEVEL=104).

**CZ-1 factory presets: filter open by default** — All factory presets and INIT TONE were loading with CUT=0 (= 30 Hz, filter nearly closed). The original CZ-1 has no filter; CUT is an added feature and should default to transparent. All factory presets now load with CUT=127 (= 16 kHz, effectively open).

**Bumped to FM-1_939.**

---

### CZ-1 sound design effects (`2026-10-09`)

Four sound design effects added to CZ-1 EDIT 2 (P_E4–P_E7), signal chain runs before the existing resonant filter then a post-filter phaser:

- **RING** (P_E4) — ring modulator. Multiplies the CZ output against a cosine carrier running at double the first oscillator's frequency. Dry/wet depth 0–127. At low depths adds shimmer and upper harmonics; at max gives full ring mod character.
- **FOLD** (P_E5) — wavefolder. Amplifies the signal 1×–3× then folds it at ±16384 (up to three reflections), adding progressively more complex harmonics as depth increases.
- **BITS** (P_E6) — bit crush. Removes 1–15 bits from the sample word. Low values give light quantisation noise; high values give extreme lo-fi digital grit.
- **PHSR** (P_E7) — phaser. A 2-stage first-order allpass phaser with a fixed ~0.3 Hz LFO (sweeps ±0.6 of the allpass coefficient). State persists across notes for a continuous slow sweep. Depth 0–127 blends dry/wet.

P_E7 was previously a hidden fixed `CZ_NATIVE` sentinel used to detect "is this still a factory preset?". That check is now removed from `cz_factory_loaded` (the engine is always CZ-native; the check was vestigial). All factory preset P_E7 values updated from `CZ_NATIVE` to 0. Phaser state (`phaser_ph`, `phaser_s[4]`) added to `cz_voice_t` in `eng_phase.c`.

**Bumped to FM-1_938.**

---

### CZ-1 EDIT page fix (`2026-10-09`)

**CZ-1 EDIT 1 now shows BANK and PTCH** — P_E0 and P_E1 had label `"-"` which the UI renders as blank columns, so EDIT 1 was entirely empty for the CZ-1 engine. P_E0 is now labelled `BANK` (F_ENUM, shows A–H) and P_E1 is `PTCH` (0 = INIT TONE, 1–16 = factory patch).

**Live BANK/PTCH switching** — changing BANK or PTCH from the EDIT page now reloads the factory tone immediately via a new `cz_block` callback (replaces the empty `cz_bank_poll` stub). The tone data updates before each audio block's voices render, so there is no glitch.

**EDIT 2 TONE param hidden** — P_E7 was labelled `TONE` but was fixed at `CZ_NATIVE` with no way to change it, appearing as an uneditable `2` on EDIT 2. Label changed to `"-"` so it is hidden. Page title changed from `"TONE"` to `"CZ-1"` for consistency.

**Bumped to FM-1_937.**

---

### Save layer key mapping fix + quick-patch save (`2026-10-09`)

**Save layer key mapping corrected** — the SAVE layer tile display was placing "save A–F" at tiles 8–13 (with two empty tiles at 6–7), but the key input handler was triggering save at w=6 (mapping to section A without the empty gap). Fixed so keys 7–8 are truly empty before the save bank, and save A–F now maps correctly to keys 9–14. (`ui_layers.c`)

**Quick patch save on key 7** — the first empty key (key 7, white key E4 while SAVE is held) now overwrites the currently selected user preset slot with the current sound. The display shows "patch" on that tile. If the song is playing the FM-1 shows "STOP BEFORE SAVE" as with any other user preset save. (`ui_layers.c`)

**Bumped to FM-1_933.**

---

### Sound engines and home knobs (`2026-10-09`)

**ANALOG engine added** — ported `eng_analog.c` from the original Felucca firmware: two band-limited oscillators (SAW, SQR, TRI, SIN, PWM), detunable second oscillator, noise, a trapezoidal SVF low-pass (cutoff, resonance, key tracking), pre-filter drive, and filter envelope via `P_ED_FLT`. 22 presets including 808 BOOM, ACID 303, REESE, WOBBLE, SUPERSAW and WARM PAD. Engine index 4 (appended after CZ-1; old projects are not affected).

**Removed VOICE engine** — the Klatt formant synthesiser (`eng_formant.c`) is still present and unchanged; it is just no longer named "VOICE" in these notes. The user wanted the distinction clarified; the engine itself was not removed.

**Home-screen knobs redesigned** — KNOB 1 is now **level** on every engine (previously KNOB 2). KNOBs 2–4 are engine-specific live-performance parameters:

| Engine | KNOB 2 | KNOB 3 | KNOB 4 |
|--------|--------|--------|--------|
| ANALOG | CUT (filter cutoff) | RES (resonance) | filter ENV amount |
| VOICE | VOWL | VOWL2 | TALK |
| DX7 | FDBK (feedback) | CUT (filter) | REVERB send |
| LOFI | TONE (low-pass) | CRSH (bit crush) | REVERB send |
| CZ-1 | DELAY send | REVERB send | GLIDE |

SWING, STEPS and PAN are accessible via the sequencer and EDIT menus.

### Effects

**Dotted delays** (`2026-10-05`)
Added dotted 1/4 and dotted 1/8 as delay time options in `fx.c` / `params.c`.

**Sequencable FX in song mode** (`2026-10-06`)
Arranger fragments can now carry FX overrides that get applied per-fragment during playback, so an arrangement can change reverb/delay settings as the song progresses (`arranger.c`, `punch.c`, `ui_song.c`).

**Punch-in FX improvement** (`2026-10-06`)
Reworked punch-in FX logic in `punch.c` for more reliable behaviour.

**Punch FX: OCT− toggles page, OCT+ toggles effect latch, PUNCH+HOME locks screen** (`2026-10-09`)
Separated the two "lock" behaviours that were conflated under one control:
- **Screen lock** (keeps the PUNCH page on display): PUNCH+HOME only. `layer_unlock()` no longer clears the effect latch as a side effect.
- **Effect latch** (keeps an effect running after the key is released): FX+OCT+ only. Pressing while an effect key is held latches it; pressing again clears the latch.
- FX+OCT− still cycles between FX page 1 and page 2 (unchanged).
(`ui_input.c`)

**Punch FX: latch requires explicit LATCH MODE; keys are momentary by default** (`2026-10-09`)
Previously every key press unconditionally set `punch.latch`, so all FX were latched by default — including on a locked screen. Now:
- **Keys are momentary by default**: hold a key → effect on; release → effect off.
- **FX + OCT+** toggles **LATCH MODE** (*FX LATCH ON* / *FX LATCH OFF*). When on, pressing a key latches it (stays on after release); pressing the same key unlatches it. Turning latch mode off also clears any active latch.
(`punch.c`, `seq.c`, `ui_input.c`)

---

### Sequencer Step Sizes

**Dotted 1m and 2m steps** (`2026-10-05`)
Added dotted 1-measure and 2-measure step lengths to the sequencer, alongside the existing dotted short values (`core.h`, `seq.c`, `ui_layers.c`).

**Half-measure support** (`2026-10-06`)
Arranger fragments can now be half a measure long (`arranger.h`, `seq.c`, `ui_song.c`).

---

### Song Mode

**Independent fragments** (`2026-10-06`)
Reworked song mode so each arranger fragment stores its own state independently, rather than sharing scene state. Major refactor of `arranger.c/h`, `arranger_scene.c`, `ui_song.c`, with new tests in `arranger_test.c`, `song_audio_test.c`, `song_ui_test.c`.

**Sequencable key changes** (`2026-10-06`)
Song-mode fragments can now include a key/root change that takes effect at that point in the arrangement (`arranger.c`, `seq.c`, `ui_song.c`).

**In-key transposition and per-fragment patch overrides** (`2026-10-07`)
Two new capabilities for song-mode fragments:
- *In-key mode* (`ARR_FLAG_INKEY`): semitone transpose is snapped to the nearest note in the current scale/root, giving diatonic transposition rather than chromatic.
- *Per-fragment patch overrides* (`arr_patch_t`): each fragment can override the engine+preset for each track independently (leaving other tracks at their scene default). Backward-compatible with old saves (smaller `arr_config_t` size triggers migration).
- Song UI gains two new sub-rows: `KEY` (CHR vs KEY mode) and `P1–P4` (per-track preset knobs).

---

### Tracks

**2 additional tracks** (`2026-10-06`)
Increased total track count. Adjusted editor, project serialization, UI layers, and draw code for the new count. Added `docs/save-slot-expansion-notes.txt` documenting the storage change.

---

### Step Probability (Synth Tracks)

**Probabilistic step firing** (`2026-10-06`)
Each synth step can be assigned a probability level so it fires at 100%, 75%, 50%, or 25% chance per pass. Stored in `core.h`, computed in `seq.c`. UI shows probability in `ui_draw.c` / `ui_input.c`. Presets updated in `upreset.c`.

---

### MIDI

**MIDI controller support** (`2026-10-07`)
Added MIDI-clock and controller-input handling in `seq.c`. Added `docs/midi-controller-setup.md` (160-line setup guide) and `docs/midi-cc-plan.md` (MIDI CC mapping reference).

---

### Housekeeping

**Build script fix** (`2026-10-06`)
Fixed `tools/build.py` and `tools/fm1pkg_make.py`.

**Firmware version bump to FM-1\_932** (`2026-10-09`)
Updated `tools/build.py` and `firmware/src/felucca.c`.

**Rebrand: SLOOP → SHIFL** (`2026-10-07`)
Renamed the project throughout — source, docs, tooling, web, tests, assets, scripts. No functional changes (52 files, pure rename/string replacement).

---

## Uncommitted (In Progress)

### Drum Lane Probability

The current working diff extends the drum step format to support **probabilistic hits per lane**, reusing the `rat` (ratchet) field with a new encoding:

| `on` bit | `rat` value | Meaning |
|---|---|---|
| 1 | 0 | Certain hit |
| 1 | 1–3 | Ratchet ×2/×3/×4 (existing) |
| 0 | 1–3 | Probabilistic hit at 75% / 50% / 25% |
| 0 | 0 | Off (no change) |

**Changes across files:**

- **`core.h`** — Updated `dstep_t` comments to document dual use of `rat`; added `drum_fired` field to `track_t` to record which lanes actually fired after probability resolution.
- **`drums.c`** — Added `dstep_certain()` / `dstep_set_prb()` helpers; `dstep_has()` now returns true for probabilistic lanes; `dstep_full_mask()` returns a mask of all lanes with any content (certain + probabilistic).
- **`seq.c`** — `drum_step()` split into two passes: certain hits always fire, probabilistic lanes roll against a threshold (96/64/32 out of 127). Ratchets now only apply to lanes in `drum_fired` (i.e., lanes that actually hit), preventing ratchet-on-miss.
- **`editor.c`**, **`seq.c`**, **`ui.c`**, **`ui_input.c`** — `dstep_mask()` calls replaced with `dstep_full_mask()` so probabilistic lanes appear in the editor, erase operations, step-occupied checks, etc.
- **`ui_studio.c`** — Drum grid and dial UI updated: hit mode shown as `--` / `on` / `P75` / `P50` / `P25` / `x2` / `x3` / `x4`. Dial 2 scrolls through all 8 modes in order. Ratchet notch markers only drawn for certain hits. Change-detection signature uses `dstep_full_mask`.
- **`project.c`** — Doc comment updated to note that FUN4 format now encodes probabilistic hits in `on`/`rat`.
