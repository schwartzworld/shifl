# Ian Schwartz — Changes Summary

All committed changes by Ian Schwartz, plus current uncommitted work-in-progress.

---

## Committed Changes

### Effects

**Dotted delays** (`2026-10-05`)
Added dotted 1/4 and dotted 1/8 as delay time options in `fx.c` / `params.c`.

**Sequencable FX in song mode** (`2026-10-06`)
Arranger fragments can now carry FX overrides that get applied per-fragment during playback, so an arrangement can change reverb/delay settings as the song progresses (`arranger.c`, `punch.c`, `ui_song.c`).

**Punch-in FX improvement** (`2026-10-06`)
Reworked punch-in FX logic in `punch.c` for more reliable behaviour.

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

**Firmware version bump to FM-1\_912** (`2026-10-07`)
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
