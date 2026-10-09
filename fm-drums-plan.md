# FM Drum Kit Implementation Plan

Add a full 6-operator FM drum kit to sloop-fm1 as a selectable kit alongside the existing synthesized kits. The kit reuses the existing `fm6_core.c` (the DX7-compatible engine already compiled in) and ports voice data and the noise-operator trick from `~/dxsloop`. Kit selection stays at `P_E0`; the FM kit appears at the end of the list.

Reference repos:
- Working repo: `/Users/ischwartz/sloop2/sloop-fm1`
- Source for FM drum ideas: `/Users/ischwartz/dxsloop`

---

## Step 1 — Add noise operator support to `fm6_core.c`

**What and why:** Hats, cymbals, snare body noise, and claps in the FM drum system use a "noise operator" trick: selected DX7 operators output xorshift-based noise instead of a sine wave (same envelope, same level — just a different waveform). Without this, those sounds come out as tonal buzzes. This is the enabling primitive for the whole FM drum system.

**Prompt for agent:**

> You are adding noise operator support to the FM6 engine in `/Users/ischwartz/sloop2/sloop-fm1/firmware/src/fm6_core.c`. This is step 1 of adding an FM drum kit to the firmware.
>
> **Background:**
> The sister repo at `/Users/ischwartz/dxsloop/firmware/src/dx7_core.c` has an identical DX7 engine with one drum-specific addition: `dx_op_noise()` at line 496. Selected operators play white noise instead of a sine (same gain/envelope math, just replacing the sine call with a sample-and-hold xorshift32). A `noise` bitmask in the voice state (`dxv_t.noise`, set externally by the drum system) directs those operators to `dx_op_noise` instead of the normal operator functions.
>
> **Task:** Make the same modification to `fm6_core.c`:
>
> 1. Read `/Users/ischwartz/dxsloop/firmware/src/dx7_core.c` lines 496–560 to understand `dx_op_noise` and how `v->noise` is checked in `dx_compute_ext`.
>
> 2. In `fm6_core.c`, the `fm6_note_t` struct is at line 402. Add three fields:
>    - `uint8_t noise;` — bitmask, bit k = operator k plays noise instead of sine
>    - `int32_t noise_val[6];` — last noise sample per operator (the `*val` in dx_op_noise)
>    - `uint32_t noise_seed[6];` — xorshift state per operator
>    Initialize all three to 0 in `fm6_note_init` (line 468) in the fresh-start branch.
>
> 3. Add a `fm6_op_noise()` function modeled exactly on `dx_op_noise()`. Port it from the dxsloop version; adjust the calling convention to match `fm6_op_run` (it uses `int32_t *out`, `int32_t phase`, `int32_t freq`, `int32_t g1`, `int32_t g2`, `int add`), plus two extra pointer params `int32_t *val, uint32_t *seed`. The xorshift and output math can be copied verbatim; only the scale differs — `dx_sin` outputs Q24 (`>> 24` final shift), `fm6_sin` outputs Q15, and the operator's gain is Q24, so the output should be `(int32_t)(((int64_t)x * g) >> 15)` matching `fm6_op_run`'s `((int64_t)fm6_sin(...) * g) >> 15`.
>
> 4. In `fm6_core_render()` (line 371), after computing `g2` for operator `k`, check `(n->noise >> k) & 1`. If set, call `fm6_op_noise` instead of `fm6_op_run` / `fm6_op_fb`. A noise operator never has feedback (ignore `FM6_FBIN`/`FM6_FBOUT` flags), so just call noise unconditionally. Pass `&n->noise_val[k]` and `&n->noise_seed[k]`.
>    - Note: `fm6_core_render` takes `fm6_op_t *op` but not `fm6_note_t *n`. You will need to either thread the noise state through differently, or change the function signature to accept a `fm6_note_t *`. The cleanest approach: add `fm6_note_t *n` as a first parameter to `fm6_core_render` (it is only called once, from `fm6_note_compute` at line 540). Pass `n` there and use `n->noise`, `n->noise_val`, `n->noise_seed` inside.
>
> 5. The `noise` field should default to 0 (no operators are noisy), so all existing melodic voices are completely unaffected. Verify that `fm6_note_compute` still compiles with the signature change.
>
> Do not change anything in `drums.c`, `drum_synth.c`, or any other file. Only modify `fm6_core.c`.
>
> After making changes, run `bash /Users/ischwartz/sloop2/sloop-fm1/build.sh` to confirm the build succeeds.

---

## Step 2 — Create FM drum voice data header

**What and why:** The drum voices are 156-byte DX7 patches (one per lane) plus a small metadata struct per lane (note, level, pitch sweep params, burst for clap, choke group, noise bitmask, pan). We port these from dxsloop's `dx7_bank.h` into a new file `firmware/src/fm_drumkit.h`, renaming types to avoid collision.

**Prompt for agent:**

> You are creating a new file `/Users/ischwartz/sloop2/sloop-fm1/firmware/src/fm_drumkit.h` as step 2 of adding an FM drum kit. This file contains the voice data for the FM drum kit.
>
> **Source material:**
> Read `/Users/ischwartz/dxsloop/firmware/src/dx7_bank.h`. The relevant sections are:
> - Line 30: `typedef struct { ... } dx_drum_t;` — per-lane metadata
> - Lines 32–50: `DX_DRUM_VOICE[17][156]` — the 156-byte DX7 patches for the default "DX KIT"
> - Lines 51–69: `DX_DRUM[17]` — the per-lane metadata for the default kit
>
> **Task:** Create `fm_drumkit.h` with:
>
> 1. A `typedef` named `fm_drum_t` that is identical to `dx_drum_t` (same fields, same types). Do not include the `const char *name` field — names will be supplied inline. Fields to keep:
>    `uint8_t note, level, sweep; uint16_t sweep_k; uint8_t burst; uint16_t burst_n; uint8_t choke, noise; int8_t pan; uint8_t rev;`
>
> 2. `#define FM_NDRUM 17` (16 lanes + 1 click/clave)
>
> 3. Copy `DX_DRUM_VOICE[DX_NDRUM][156]` verbatim as `FM_DRUM_VOICE[FM_NDRUM][156]` — same byte arrays, same comments. These are standard DX7 patches that work identically with `fm6_core.c`.
>
> 4. Copy `DX_DRUM[DX_NDRUM]` as `FM_DRUM[FM_NDRUM]`, adapting each initializer to drop the `name` field and keep the remaining fields in the same order. So each entry becomes:
>    `{note, level, sweep, sweep_k, burst, burst_n, choke, noise, pan, rev}`
>
> 5. Add a string list for names (for the lane display). The names come from the `name` field of `DX_DRUM`:
>    `static const char *const FM_DRUM_LANE_NAME[FM_NDRUM] = {"KICK 808", "KICK PUNCH", ...};`
>
> Do not modify any existing files. Only create `fm_drumkit.h`.
>
> After creating it, verify it is valid C by doing a quick syntax check: from within the firmware source directory, run `gcc -std=c11 -fsyntax-only -x c /dev/null -include /Users/ischwartz/sloop2/sloop-fm1/firmware/src/fm_drumkit.h 2>&1 || true` — a "no such file" error on includes is fine since we are not in the full build; what matters is no syntax errors in the new file itself. Then run `bash /Users/ischwartz/sloop2/sloop-fm1/build.sh` to confirm the overall build still succeeds (the new file is not yet included anywhere, so the build should be clean).

---

## Step 3 — Add FM drum dispatch to `drums.c`

**What and why:** Wire the FM kit into the existing drum trigger and render paths in `drums.c`. On a hit the FM kit calls `fm6_note_init`; each audio block it calls `fm6_note_compute` and adds the result to the mix. Pitch sweep (the characteristic drum pitch drop) is handled as a per-voice Q24 log2 offset that decays each block.

**Prompt for agent:**

> You are adding FM drum triggering and rendering to `/Users/ischwartz/sloop2/sloop-fm1/firmware/src/drums.c`. This is step 3 of adding an FM drum kit. Steps 1 and 2 are already done: `fm6_core.c` now has noise operator support, and `fm_drumkit.h` exists with voice data and the `fm_drum_t` struct.
>
> **Context — key files:**
> - `/Users/ischwartz/sloop2/sloop-fm1/firmware/src/drums.c` — the drum engine you are modifying
> - `/Users/ischwartz/sloop2/sloop-fm1/firmware/src/fm6_core.c` — the FM engine (already compiled in via `eng_fm6.c`; its types and functions are available)
> - `/Users/ischwartz/sloop2/sloop-fm1/firmware/src/fm_drumkit.h` — the voice data you created in step 2
> - `/Users/ischwartz/dxsloop/firmware/src/drums.c` — reference for how dxsloop handles FM drum triggering (lines ~560–760); read this for the sweep/burst/choke patterns
>
> **How `fm6_core.c` works (key API):**
> - `fm6_note_init(fm6_note_t *n, const uint8_t *p, int32_t note, int32_t vel, int fresh)` — trigger a note; `fresh=1` from silence, `p` is the 156-byte patch (but note: `fm6_core.c` uses `FP_SIZE = 155`; the dxsloop patches are 156 bytes where byte 155 is a null terminator — pass `p` directly, the engine only reads the first 155 bytes)
> - `fm6_note_compute(fm6_note_t *n, const uint8_t *p, int32_t *out, int32_t lfo_val, int32_t lfo_delay, int32_t logfreq, uint32_t alg, int32_t fb, const int32_t *dt, int32_t modlvl)` — render one block (CTL samples) into `out` (Q24); returns 0 if silent
> - `fm6_note_done(const fm6_note_t *n, const uint8_t *p, uint32_t alg)` — returns 1 when the voice can be freed
> - `fm6_note_logfreq(int32_t pitch16, int32_t fine)` — converts a MIDI note (in 1/16 semitones) to log2 frequency (Q24)
> - For drums: pass `lfo_val = 1<<23`, `lfo_delay = 1<<24`, `dt = (int32_t[6]){0}`, `modlvl = 0`, `alg = p[FP_ALG]`, `fb = p[FP_FB]`
>
> **Pitch sweep:** At note-on, `sweep = FM_DRUM[lane].sweep` semitones. In Q24 log2: `sweep_offset = (int32_t)((int64_t)sweep * (1<<24) / 12)`. Each CTL block it decays: `sweep_offset = (int32_t)(((int64_t)sweep_offset * sweep_k) >> 16)` where `sweep_k = FM_DRUM[lane].sweep_k`. Pass `logfreq + sweep_offset` to `fm6_note_compute`.
>
> **Burst (for clap):** On note-on, `burst = FM_DRUM[lane].burst`. If burst > 1, schedule additional retriggering: after `burst_n` samples, call `fm6_note_init` again (with `fresh=0` for a retrigger). Track burst state per voice slot.
>
> **Task:**
>
> 1. At the top of `drums.c` (after the existing `#include "drum_synth.c"` at line 10), add:
>    ```c
>    #include "fm_drumkit.h"
>    ```
>
> 2. Add `DRUM_FM` constant after `DRUM_KITS` (line 18):
>    ```c
>    #define DRUM_FM DRUM_KITS
>    #define DRUM_KITS_TOTAL (DRUM_FM + 1u)
>    ```
>    Then update `DRUM_KIT_NAMES` (line 19) to append `"DX KIT"` and `DRUM_KIT_STYLES` (line 21) to append `"FM DRUMS"`. Update the `_Static_assert` on line 23 to use `DRUM_KITS_TOTAL`. Update `drum_kit()` on line 24 to clamp to `DRUM_KITS_TOTAL - 1`.
>
> 3. In the `drums` state struct (lines 27–40), add:
>    ```c
>    fm6_note_t fmd[NDRUM];         /* FM drum voices */
>    int32_t fmd_sweep[NDRUM];      /* Q24 log2 pitch offset, decaying */
>    uint8_t fmd_burst[NDRUM];      /* remaining burst hits */
>    uint32_t fmd_burst_t[NDRUM];   /* samples until next burst hit */
>    uint8_t fmd[NDRUM];            /* flag: slot plays an FM drum voice */
>    ```
>    Rename the existing `drums.ds[NDRUM]` / `drums.synth[NDRUM]` approach: add a separate `uint8_t fm[NDRUM]` flag for FM voices (analogous to `synth[NDRUM]` for synth voices).
>
>    **Note:** avoid naming conflicts — call the flag `uint8_t fmdrm[NDRUM]` since `fmd` is taken by the `fm6_note_t` array.
>
> 4. In `drum_on()` (line 160), add an FM kit branch at the top of the synth-kit `else if (kit >= DRUM_SAMPLED)` block (line 184). Before the existing synth branch, add:
>    ```c
>    } else if (kit == DRUM_FM) {
>        /* FM drum kit */
>        uint32_t lane = lane_of_note(note);
>        const fm_drum_t *d = &FM_DRUM[lane < FM_NDRUM - 1 ? lane : FM_NDRUM - 1];
>        const uint8_t *patch = FM_DRUM_VOICE[lane < FM_NDRUM - 1 ? lane : FM_NDRUM - 1];
>        uint32_t alg = patch[FP_ALG];
>        /* hat choke */
>        if (note == 42u || note == 44u)
>            for (i = 0; i < NDRUM; i++)
>                if (drums.v[i].active && drums.v[i].note == 46u && drums.fmdrm[i]) {
>                    drums.v[i].active = 0;
>                    drums.tail += drums.v[i].s[7];
>                }
>        /* steal oldest or find free slot */
>        for (i = 0; i < NDRUM; i++) {
>            if (!drums.v[i].active) { v = &drums.v[i]; break; }
>            if (drums.v[i].age < v->age) v = &drums.v[i];
>        }
>        if (v->active) drums.tail += v->s[7];
>        i = (uint32_t)(v - drums.v);
>        v->note = (uint8_t)note;
>        v->vel = (uint8_t)vel;
>        v->active = 1;
>        v->s[7] = 0;
>        v->age = ++drums.age;
>        drums.kit[i] = (uint8_t)kit;
>        drums.synth[i] = 0;
>        drums.fmdrm[i] = 1;
>        /* set noise bitmask from kit data */
>        drums.fmd[i].noise = d->noise;
>        fm6_note_init(&drums.fmd[i], patch, d->note + (int32_t)vel / 32 - 2, vel, 1);
>        /* pitch sweep */
>        drums.fmd_sweep[i] = d->sweep ? (int32_t)((int64_t)d->sweep * (1 << 24) / 12) : 0;
>        /* burst */
>        drums.fmd_burst[i] = d->burst > 1 ? d->burst - 1 : 0;
>        drums.fmd_burst_t[i] = d->burst_n;
>        return;
>    ```
>
> 5. In `drums_mix()` (line 265), in the synthesized-voices loop (lines 279–304), the loop currently checks `!drums.synth[k]` and skips. Add a second loop after it (or extend it) for FM voices:
>    ```c
>    for (k = 0; k < NDRUM; k++) {
>        voice_t *v = &drums.v[k];
>        static int32_t dt6[6] = {0};
>        static int32_t fm_out[CTL];
>        uint32_t m = n < CTL ? n : CTL;
>        if (!v->active || !drums.fmdrm[k]) continue;
>        /* burst: count down, retrigger */
>        if (drums.fmd_burst[k] && drums.fmd_burst_t[k] <= m) {
>            uint32_t lane = lane_of_note(v->note);
>            const uint8_t *patch = FM_DRUM_VOICE[lane < FM_NDRUM - 1 ? lane : FM_NDRUM - 1];
>            fm6_note_init(&drums.fmd[k], patch, drums.fmd[k]./* need saved note */, v->vel, 0);
>            drums.fmd_burst[k]--;
>            drums.fmd_burst_t[k] = FM_DRUM[lane < FM_NDRUM - 1 ? lane : FM_NDRUM - 1].burst_n;
>        } else if (drums.fmd_burst[k]) {
>            drums.fmd_burst_t[k] -= m;
>        }
>        /* sweep decay */
>        uint32_t lane = lane_of_note(v->note);
>        const fm_drum_t *d = &FM_DRUM[lane < FM_NDRUM - 1 ? lane : FM_NDRUM - 1];
>        const uint8_t *patch = FM_DRUM_VOICE[lane < FM_NDRUM - 1 ? lane : FM_NDRUM - 1];
>        int32_t logfreq = fm6_note_logfreq((int32_t)d->note * 16, 0) + drums.fmd_sweep[k];
>        if (drums.fmd_sweep[k])
>            drums.fmd_sweep[k] = (int32_t)(((int64_t)drums.fmd_sweep[k] * d->sweep_k) >> 16);
>        /* render */
>        int sounded = fm6_note_compute(&drums.fmd[k], patch, fm_out, 1<<23, 1<<24,
>                                        logfreq, patch[FP_ALG], patch[FP_FB], dt6, 0);
>        if (!sounded || fm6_note_done(&drums.fmd[k], patch, patch[FP_ALG]))
>            v->active = 0;
>        if (sounded) {
>            for (i = 0; i < m; i++) {
>                int32_t s = (int32_t)(((int64_t)fm_out[i] * lvl) >> 24);
>                s = mulq15(s, mulq15((int32_t)(d->level * 258), 32767 - drums.a0 - (((drums.a1 - drums.a0) * (int32_t)i) >> CTL_LOG2)));
>                v->s[7] = s;
>                if (s > pk || -s > pk) pk = s < 0 ? -s : s;
>                if (mono) { mono[i] += s; continue; }
>                ml[i] += (s * gl) >> 12;
>                mr[i] += (s * gr) >> 12;
>                if (send) rev[i] += mulq15(s, send);
>            }
>        }
>        if (!v->active) { drums.tail += v->s[7]; v->s[7] = 0; }
>    }
>    ```
>    The code above has a placeholder comment for "saved note" — store the base note in a field or compute it from `d->note` directly (it's fixed per lane, not from the MIDI note).
>
>    Also: in the existing synth voice loop (line 279) and the existing sample loop (line 305), add `|| drums.fmdrm[k]` guards where `drums.synth[k]` is checked, so FM voices are skipped in those loops.
>
> 6. In the `drum_on` function, make sure `drums.fmdrm[i] = 0` is set when a synth or sample kit voice starts (so the FM render loop skips it). Similarly set `drums.synth[i] = 0` when an FM voice starts.
>
> After making changes, run `bash /Users/ischwartz/sloop2/sloop-fm1/build.sh`. Fix any compile errors before finishing.

---

## Step 4 — Final wiring, level calibration, and build verification

**What and why:** After step 3 the FM kit compiles and appears in the kit list. This step calibrates the output level (fm6_core outputs Q24; the mix expects roughly the same magnitude as `ds_render`'s output), verifies the kit name appears correctly in the existing UI string arrays, and does a clean build check. No new features — just make sure everything is connected and the build is clean.

**Prompt for agent:**

> You are doing final cleanup and verification for the FM drum kit feature in `/Users/ischwartz/sloop2/sloop-fm1`. Steps 1–3 are complete: `fm6_core.c` has noise operators, `fm_drumkit.h` has voice data, and `drums.c` has FM drum triggering and rendering.
>
> **Tasks:**
>
> 1. **Build check.** Run `bash /Users/ischwartz/sloop2/sloop-fm1/build.sh` and fix any remaining compile errors or warnings from the changes made in steps 1–3.
>
> 2. **Level audit.** The FM engine (`fm6_core.c`) renders into Q24 output (`fm6_note_compute` fills `int32_t out[]` with Q24 values). The drums mix loop expects signal in the same range as `ds_render` (which outputs values that, after `mulq15(ds_buf[i], mulq15(lvl, ...))`, land in a usable range). In the FM render loop added in step 3, the scaling line is:
>    ```c
>    int32_t s = (int32_t)(((int64_t)fm_out[i] * lvl) >> 24);
>    ```
>    where `lvl = song.g[G_DRLVL] * 200` (same as the synth voices). Since `fm_out[i]` is Q24 and the synth voices use Q15 at full level ≈ 32767, this may need a shift adjustment. Look at how `eng_fm6.c` scales FM output for the melodic parts (the file is at `firmware/src/eng_fm6.c`) and apply the same normalization here so FM drums are roughly the same loudness as the synth drums at the same LEVEL setting.
>
> 3. **Kit name in UI.** The kit name `"DX KIT"` and style `"FM DRUMS"` were added to `DRUM_KIT_NAMES[]` and `DRUM_KIT_STYLES[]`. Verify the static assert on line 23 of `drums.c` (`_Static_assert(sizeof(DRUM_KIT_NAMES) / sizeof(DRUM_KIT_NAMES[0]) == DRUM_KITS_TOTAL, ...)`) passes. If the assert macro name changed (from `DRUM_KITS` to `DRUM_KITS_TOTAL`), make sure the assert uses the right constant.
>
> 4. **Default kit check.** `DRUM_DEFAULT_KIT` is set to `DRUM_SAMPLED` (line 25 of `drums.c`) so existing projects load the synth drums by default. Confirm the FM kit does not change the default and that `DRUM_DEFAULT_KIT` still points to the first synthesized kit.
>
> 5. **Verify the final build output** by running `bash /Users/ischwartz/sloop2/sloop-fm1/build.sh` and confirming the last line reports the identity string (e.g., `FM-1_916` or whatever the current version is). The output file should be `build/felucca.fwsc`.
>
> Report any issues found and confirm the build is clean.
