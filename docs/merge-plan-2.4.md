# Merge Plan: Incorporating Upstream 2.4 / 2.4.1

## Background and problem

The `with-merge-conflicts` branch contains all upstream 2.4 and 2.4.1 changes rebased onto our `main`. Merging that branch wholesale caused a **RAM overflow**: the firmware's fixed-address `.bss` region overflowed by 8,468 bytes.

### Why it happened

Two things landed at the same time:

1. **The visualizer (`ui_vis.c`)** — added large audio/FFT buffers: `scope_buf[512]`, `scope_bufr[512]` in `audio.c`, and `vis_l[512]`, `vis_r[512]`, `vis_lj[3][256][2]` in `ui_vis.c`. These consumed most of the remaining `.bss` slack.
2. **The FM6 engine** — added its own data: `fm6_note`, `fm6_eff`, `fm6_patch`, etc., which pushed `.bss` past the limit.

### The fix (documented in `docs/rebase-notes.md`)

Five large buffers were tagged `__attribute__((section(".pool")))` to move them out of `.bss` into the `.pool` memory region, which has room:

| Buffer | File | Size freed |
|---|---|---|
| `scope_buf[512]`, `scope_bufr[512]` | `firmware/src/audio.c` | 2,048 B |
| `vis_l[512]`, `vis_r[512]` | `firmware/src/ui_vis.c` | 2,048 B |
| `vis_lj[3][256][2]` | `firmware/src/ui_vis.c` | 1,536 B |

**Total freed from `.bss`: 5,632 B** — enough to clear the overflow.

### Why we're going one at a time

Rather than land everything at once and re-trigger the overflow, we're incorporating changes incrementally. This lets us verify the `.bss` / `.pool` map at each step and catch any new overflow before it compounds.

**Key ordering rules:**
- The `.pool` annotation on `scope_buf` / `scope_bufr` in `audio.c` (task 5) **must land before** the FM6 engine is wired in (task 19), because FM6 fills the `.bss` slack that annotation frees.
- The visualizer's `.pool` annotations land with `ui_vis.c` itself (task 22) — those buffers don't exist on `main` yet, so there's no risk until that task.
- FM6 source files can be added as inert new files (task 1) without any RAM impact — they're not compiled until wired into the build (task 19).

### Note on dotted delays / longer steps

Our `main` branch has a local implementation of dotted delays and longer step times. Upstream 2.4 has its own version of the same feature. **Use the upstream version** — task 11 handles the replacement.

---

## Tasks

### Phase 1 — New files, zero conflict risk

---

**Task 1 — Add FM6 engine source files**

*Files:* `firmware/src/fm6_core.c` (new), `firmware/src/eng_fm6.c` (new), `firmware/src/fm6_bank.c` (new), `firmware/src/editor_fm6.c` (new), `LICENSES/Apache-2.0-msfa.txt` (new)

These are entirely new files. They will not be compiled until task 19 wires them into the build, so they carry zero RAM risk at this stage.

> **Agent prompt:** Cherry-pick the new FM6 source files from the `with-merge-conflicts` branch into `main`. The working directory is `/Users/ischwartz/sloop2/sloop-fm1`. The files are `firmware/src/fm6_core.c`, `firmware/src/eng_fm6.c`, `firmware/src/fm6_bank.c`, `firmware/src/editor_fm6.c`, and `LICENSES/Apache-2.0-msfa.txt`. These are entirely new — copy them verbatim using `git show with-merge-conflicts:<path>`. Do not modify any existing files or add the new files to the build system yet. Run `bash build.sh` and confirm it still passes (the new files won't be compiled yet). Commit.

---

**Task 2 — Add new test files and tool scripts**

*Files:* `tests/fm6_test.c` (new), `tests/seq2_test.c` (new), `tests/stress_test.c` (new), `tests/userkit_test.c` (new), `tools/gen_fm6_patches.py` (new), `tools/gen_webfont.py` (new)

All new files — no conflict risk.

> **Agent prompt:** Cherry-pick the new test and tool files from the `with-merge-conflicts` branch into `main`. The working directory is `/Users/ischwartz/sloop2/sloop-fm1`. All are new files: `tests/fm6_test.c`, `tests/seq2_test.c`, `tests/stress_test.c`, `tests/userkit_test.c`, `tools/gen_fm6_patches.py`, `tools/gen_webfont.py`. Copy them verbatim using `git show with-merge-conflicts:<path>`. Run `bash build.sh` to confirm nothing breaks. Commit.

---

**Task 3 — Add documentation**

*Files:* `GUIDE.md` (new), `docs/rebase-notes.md` (new)

> **Agent prompt:** Cherry-pick the new documentation files from `with-merge-conflicts` into `main`. The working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Files: `GUIDE.md` and `docs/rebase-notes.md`. Both are new — copy verbatim using `git show with-merge-conflicts:<path>`. Commit.

---

**Task 4 — New web editor**

*Files:* `web/` directory, `web/EDITOR_PROTOCOL.md`

> **Agent prompt:** The `with-merge-conflicts` branch contains a rebuilt web editor. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Diff the `web/` directory between `main` and `with-merge-conflicts` (`git diff main...with-merge-conflicts -- web/`). Identify all new files vs modified files. Copy in any entirely new files verbatim. For modified files, check whether `main` has local changes (`git log main -- <file>`). If a file has no local changes, take the upstream version directly. If it has local changes, flag them and stop rather than silently dropping work. Run `bash build.sh`. Commit.

---

### Phase 2 — Targeted changes to files user hasn't touched

---

**Task 5 — RAM safety: `.pool` annotation on `scope_buf` in `audio.c`**

*Files:* `firmware/src/audio.c`

**This is a prerequisite for task 19 (FM6 integration).** The FM6 engine fills the remaining `.bss` slack; these annotations free 2,048 bytes from `.bss` before that happens.

> **Agent prompt:** In the `with-merge-conflicts` branch, `firmware/src/audio.c` has `__attribute__((section(".pool")))` added to `scope_buf[512]` and `scope_bufr[512]`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Apply the same annotations to those two buffers on `main` (`git show with-merge-conflicts:firmware/src/audio.c` to see the exact change). Run `bash build.sh` and confirm the build map shows `.bss` within limits. Commit.

---

**Task 6 — HAL changes for FM6 bank and IRQ**

*Files:* `firmware/hal/fm1_flash.h`, `firmware/hal/fm1_irq.h`

Small, targeted additions — flash layout constants for the FM6 patch bank and new IRQ entries.

> **Agent prompt:** Diff `firmware/hal/fm1_flash.h` and `firmware/hal/fm1_irq.h` between `main` and `with-merge-conflicts` (`git diff main...with-merge-conflicts -- firmware/hal/`). Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Apply the upstream additions. These are small header additions with no local conflict. Run `bash build.sh`. Commit.

---

**Task 7 — Bigger values display**

*Files:* `firmware/src/ui_draw.c`

Pages without a graph (EDIT, VOICE, GLOBAL, etc.) show their four parameter values in large type in the center of the screen.

> **Agent prompt:** Diff `firmware/src/ui_draw.c` between `main` and `with-merge-conflicts` (`git diff main...with-merge-conflicts -- firmware/src/ui_draw.c`). Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Apply the upstream change that adds large-type rendering of parameter values on pages without a graph. Check for local changes in this file on `main` (`git log main -- firmware/src/ui_draw.c`) and note any conflicts. Run `bash build.sh`. Commit.

---

**Task 8 — Per-track filter**

*Files:* `firmware/src/fx.c`, `firmware/src/editor.c`, `firmware/src/core.h`

One knob per track: low-pass to the left, high-pass to the right. Accessible via FX → FILTER or FX held + KNOB 4. Lockable on a step.

> **Agent prompt:** Diff `firmware/src/fx.c`, `firmware/src/editor.c`, and `firmware/src/core.h` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract only the per-track filter feature. **Important:** `fx.c` also contains dotted delay changes — do NOT include those yet; they are handled separately in task 11. Apply the filter-only changes. Run `bash build.sh`. Commit.

---

**Task 9 — SELECT turns the pages**

*Files:* `firmware/src/ui_input.c`

On a page (LFO, FX, EDIT, etc.), SELECT advances to the next or previous page of its group.

> **Agent prompt:** Diff `firmware/src/ui_input.c` between `main` and `with-merge-conflicts` (`git diff main...with-merge-conflicts -- firmware/src/ui_input.c`). Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract only the change that makes SELECT advance pages within a page group. Apply it. Run `bash build.sh`. Commit.

---

### Phase 3 — Features touching files the user has modified

---

**Task 10 — USR4 drum kit slot**

*Files:* `firmware/src/drums.c`, `firmware/src/ui_studio.c`, `firmware/src/project.c`

Adds a 4th user sample slot (USR4) and a USR3+4 combined large kit option (~15 s). The user has local drum changes on `main` ("Drum better" commit) — resolve carefully.

> **Agent prompt:** Diff `firmware/src/drums.c`, `firmware/src/ui_studio.c`, and `firmware/src/project.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Before applying anything, run `git log main -- firmware/src/drums.c firmware/src/ui_studio.c` to see what local drum changes exist. Apply the USR4 additions (new 4th user sample slot, USR3+4 combined kit) while preserving local work. Do not silently drop any local changes. Run `bash build.sh` and any drum-related tests. Commit.

---

**Task 11 — Dotted delays + longer steps — USE UPSTREAM VERSION**

*Files:* `firmware/src/seq.c`, `firmware/src/fx.c`

The `main` branch has a local implementation of this feature. **Replace it with the upstream version.** DIV goes to 1/2, 1 bar, 2 bars; the delay has 1/8 dotted and 1/16 dotted options.

> **Agent prompt:** The `main` branch at `/Users/ischwartz/sloop2/sloop-fm1` has a local implementation of dotted delays and longer step times (DIV going to 1/2, 1 bar, 2 bars; dotted delay options). The upstream `with-merge-conflicts` branch has its own version. Per project instructions, the upstream version should replace the local one. Diff `firmware/src/seq.c` and `firmware/src/fx.c` between both branches. Identify the local implementation of this specific feature and remove it, replacing it with the upstream implementation. Be surgical — preserve any other local changes in those files (song mode work, etc.). Run `bash build.sh` and the sequencer tests. Commit.

---

**Task 12 — Micro timing**

*Files:* `firmware/src/seq.c`, `firmware/src/ui_input.c`

Hold a step: KNOB 4 nudges it up to half a step early or late, in 1/64 increments.

> **Agent prompt:** Diff `firmware/src/seq.c` and `firmware/src/ui_input.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract only the micro-timing feature (hold step + KNOB 4 to nudge timing ±½ step in 1/64 increments). Avoid pulling in fills or parameter locks changes — those are separate tasks. Run `bash build.sh`. Commit.

---

**Task 13 — Fills**

*Files:* `firmware/src/seq.c`, `firmware/src/ui_layers.c`, `firmware/src/ui_input.c`

Hold a step + OCT+ to mark it fill-only or no-fill. GLO + key 9 plays a fill while held; key 10 plays a fill on the whole next bar.

> **Agent prompt:** Diff `firmware/src/seq.c`, `firmware/src/ui_layers.c`, and `firmware/src/ui_input.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract only the fills feature. Note: `ui_layers.c` also contains CHORD+ and quick chain changes — isolate only fills. The user has local song mode work in `ui_layers.c`; preserve it. Run `bash build.sh`. Commit.

---

**Task 14 — Parameter locks**

*Files:* `firmware/src/seq.c`, `firmware/src/editor.c`, `firmware/src/ui_input.c`

Hold a step: PRESETS assigns a sound parameter a different value for that step only; ALGORITHM picks which parameter. Up to 24 locks per track, multiple on one step.

> **Agent prompt:** Diff `firmware/src/seq.c`, `firmware/src/editor.c`, and `firmware/src/ui_input.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract the parameter locks feature. Apply carefully alongside existing local changes in these files. Run `bash build.sh` and relevant tests. Commit.

---

**Task 15 — Quick chain**

*Files:* `firmware/src/ui_layers.c`

Hold SAVE and tap section keys A B B C…; release to play sections in sequence, each for its pattern's length, looped.

> **Agent prompt:** Diff `firmware/src/ui_layers.c` between `main` and `with-merge-conflicts` (`git diff main...with-merge-conflicts -- firmware/src/ui_layers.c`). Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract only the quick chain feature. The user has local song mode changes in this file — preserve them. Run `bash build.sh`. Commit.

---

**Task 16 — CHORD+**

*Files:* `firmware/src/ui_layers.c`

In chord mode, the black keys modify the chord quality (major↔minor, 7th, sus4, 9th, inversion) even while held. STRUM strums chords; VLEAD voices them smoothly.

> **Agent prompt:** Diff `firmware/src/ui_layers.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract only the CHORD+ feature (black-key chord modification, STRUM, VLEAD). Preserve all local song mode work. Run `bash build.sh`. Commit.

---

**Task 17 — Clock only + Sequencer to MIDI out**

*Files:* `firmware/src/seq.c`, `firmware/src/ui_menu.c`

Two new SYSTEM menu options: IN = CLOCK (follow DAW clock, ignore DAW notes); MIDI = SEQ (send sequencer steps, arp, and rolls out on USB MIDI).

> **Agent prompt:** Diff `firmware/src/seq.c` and `firmware/src/ui_menu.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract these two features: (1) Clock only — GLO → SYSTEM → IN = CLOCK follows DAW clock and ignores notes; (2) SEQ MIDI out — GLO → SYSTEM → MIDI = SEQ sends sequencer steps, arp, and rolls out on USB MIDI. Apply both. Run `bash build.sh`. Commit.

---

**Task 18 — Drums with the keys**

*Files:* `firmware/src/ui_studio.c`, `firmware/src/ui_input.c`

On the drum grid page, the keys become steps for the selected sound — press to set, press again to clear; selecting a sound or step plays it.

> **Agent prompt:** Diff `firmware/src/ui_studio.c` and `firmware/src/ui_input.c` between `main` and `with-merge-conflicts`. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Extract the 'drums with keys' feature. The user has local drum changes in these files — preserve them. Run `bash build.sh`. Commit.

---

### Phase 4 — FM6 integration (requires Phase 1 files to exist)

---

**Task 19 — Wire FM6 into the build**

*Files:* `firmware/src/core.h`, `firmware/src/voice.c`, `firmware/src/engines.c`, `firmware/src/project.c`, `firmware/src/editor.c`

This is the step that brings FM6 online. The source files were added in task 1 but not compiled. The `.pool` annotation from task 5 must already be in place — it frees the `.bss` space this step consumes.

> **Agent prompt:** The FM6 source files (`fm6_core.c`, `eng_fm6.c`, `fm6_bank.c`, `editor_fm6.c`) were added as inert new files in a previous step. Now wire them into the build. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Diff `firmware/src/core.h`, `firmware/src/voice.c`, `firmware/src/engines.c`, `firmware/src/project.c`, and `firmware/src/editor.c` between `main` and `with-merge-conflicts`. Apply the changes that wire FM6 into engine selection, voice allocation, project format (FUN5), and editor UI. This enables the 8 factory FM6 patches and the FM6 patch bank in NOR flash. After applying, run `bash build.sh` and inspect the `.bss` / `.pool` map in the build output to confirm there is no overflow — the `.pool` annotation added earlier should provide the necessary slack. Run `tests/fm6_test.c`. Commit.

---

**Task 20 — DX7 AMS noise fix (2.4.1)**

*Files:* `firmware/src/fm6_core.c` or `firmware/src/eng_fm6.c`

Bug: an FM6 operator with amplitude modulation sensitivity (AMS) above zero received a wrong level calculation, producing noise and static that worsened with each held key. About one in four DX7 patches was affected. Factory FM6 sounds were not affected.

> **Agent prompt:** In the `with-merge-conflicts` branch, there is a 2.4.1 fix for a bug where FM6 operators with AMS > 0 produced noise due to a wrong level calculation. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Diff `firmware/src/fm6_core.c` and `firmware/src/eng_fm6.c` between the 2.4 base commit and the tip of `with-merge-conflicts` to isolate this specific fix. Apply only this fix. Run `bash build.sh` and the FM6 test suite. Commit.

---

**Task 21 — Sequencer and misc 2.4 bug fixes**

*Files:* `firmware/src/seq.c`, `firmware/src/editor.c`, `firmware/src/audio.c`

Six specific bug fixes from the 2.4 changelog — not features.

> **Agent prompt:** Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. Diff `firmware/src/seq.c`, `firmware/src/editor.c`, and `firmware/src/audio.c` between `main` and `with-merge-conflicts`. Extract and apply only these six bug fixes — not any feature additions: (1) MIDI START during REC count-in now records immediately; (2) swing no longer affects triplets; (3) turning DIV right after PLAY no longer skips a step; (4) the SEQ layer's DIV now reaches 1/2, 1 bar, 2 bars; (5) the editor never writes flash while the song plays; (6) 12 kHz samples read in full. Apply surgically. Run `bash build.sh` and `tests/regress.c`. Commit.

---

### Phase 5 — Last

---

**Task 22 — Visualizer**

*Files:* `firmware/src/ui_vis.c` (new), `firmware/src/ui_input.c`, `firmware/src/ui_draw.c`

**The visualizer is the original cause of the RAM overflow.** Its large audio/FFT buffers must have `.pool` annotations — confirm before committing.

> **Agent prompt:** Add the visualizer from the `with-merge-conflicts` branch. Working directory is `/Users/ischwartz/sloop2/sloop-fm1`. The main file `firmware/src/ui_vis.c` is new — copy it verbatim using `git show with-merge-conflicts:firmware/src/ui_vis.c`. **Before committing:** verify that `vis_l[512]`, `vis_r[512]`, and `vis_lj[3][256][2]` all have `__attribute__((section(".pool")))`. If any are missing this annotation, add it — these buffers caused the original `.bss` overflow and must stay in `.pool`. Also apply the upstream changes to `firmware/src/ui_input.c` and `firmware/src/ui_draw.c` that wire up the visualizer (HOME tap → full-screen styles, SELECT to change). Wire `ui_vis.c` into the build. Run `bash build.sh` and inspect the `.bss` / `.pool` map to confirm no overflow. The 12 visualizer styles are: oscilloscope, spectrum, spectrogram, Lissajous, VU meters, circle, tape, LCD, bounce, orbit, wires, SHIFL logo. Commit.
