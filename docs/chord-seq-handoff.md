# Chord Sequencer — Implementation Handoff (FM-1_933 / SHIFL 3.0)

Written 2026-10-14 for the next implementer to pick up from.

---

## What was built

A global 32-step chord progression sequencer, accessible via hold-SEQ (the LY_STEP layer, previously "STEPS", now renamed "CHORD"). The sequencer produces no audio itself; it tracks an active chord (`chord_root`, `chord_type`, `chord_inv`) for a future scale-quantize feature.

---

## Architecture

### Data type (`core.h`)

```c
#define NCHORD_STEP 32
enum { CQ_NONE=0, CQ_MAJ, CQ_MIN, CQ_DIM, CQ_AUG,
       CQ_MAJ7, CQ_MIN7, CQ_DOM7, CQ_DIM7, CQ_M7B5, CQ_MMAJ7,
       CQ_MAJ9, CQ_MIN9, CQ_DOM9, CQ_COUNT };   /* 14 qualities */

typedef struct {
    uint8_t note;   /* 0=REST, 1-127=MIDI root */
    uint8_t pack;   /* [7:4]=CQ_type  [3:2]=inversion 0-3  [1:0]=cond 0-3 */
} chord_step_t;
```

Four inline accessors: `cstep_type()`, `cstep_inv()`, `cstep_cond()`, `cstep_pack()`.

### Sequencer state (`seq.c`)

Static globals (RAM, 64 B data + overhead):
- `chord_step_t cstep[NCHORD_STEP]` — the 32 steps
- `uint8_t clen` — active length (default 8)
- `uint8_t cseq_div` — step duration index into N_SDIV (default 7 = 1 bar)
- `uint32_t cseq_abs` — absolute tick of last step advance (SEQ_NONE = not running)
- `uint32_t cseq_idx` — current playhead step
- `uint8_t chord_root, chord_type, chord_inv` — currently active chord (readable by future quantizer)

Flash constants:
- `CSEQ_SEMI[CQ_COUNT][4]` — semitone offsets for each quality (up to 4 notes)
- `N_CQ[CQ_COUNT]` — display names ("MAJ", "MIN", …)

Key functions:
- `cseq_notes(root, type, inv, buf[4])` — fills a 4-byte MIDI note buffer from a chord step
- `cseq_tick()` — called once per audio block after the per-track `seq_tick()` loop; advances on grid boundaries

`seq_reset_tracks()` resets `cseq_abs = SEQ_NONE; cseq_idx = 0;`.

### UI state (`ui.c` — added to `ui` struct)

```c
uint8_t chord_page;       /* page shown (page*16 offset) */
uint8_t chord_cursor;     /* selected step */
uint8_t chord_entry_key;  /* white key held (0xFF = none) */
uint8_t chord_entry_root; /* MIDI root for the held key */
```

`chord_entry_key` initialised to `0xFF` in `layers_init()` (ui_layers.c) and cleared in the layer-release path (ui_input.c).

### Layer display and input (`ui_layers.c`)

`cstep_label(char *buf, const chord_step_t *cs)` — generates "Cm7", "F", "--" etc.
Must live here (not seq.c) because it calls `note_name()` from `ui.c`; include order in `felucca.c` is: `seq.c` (line 53) → `ui.c` (56) → `ui_layers.c` (62).

`case LY_STEP:` draw block:
- 16 tiles per page (page 0 = steps 0–15, page 1 = steps 16–31)
- Black keys: C# → page 0, D# → page 1
- White key down → enters chord entry mode for that step
- Normal knobs: K1=cursor, K2=len, K3=div, K4=cond/REST
- Entry knobs: K1=chord quality, K3=inversion

### Project format (`project.c`)

- `PROJ_MAGIC` = `0x46554E37u` (FUN7)
- `PROJ_MAGIC_V6` = `0x46554E36u` kept for migration
- `project_t` gains: `chord_step_t cstep[NCHORD_STEP]`, `uint8_t clen`, `uint8_t cseq_div`, `uint8_t rsv_chord[2]`
- `proj_from_v6()` copies old data, sets `clen=8; cseq_div=7;`
- `proj_apply()` guards with `if (p->clen > 0)` so pre-FUN7 projects (memset-zero) skip chord loading

---

## REST semantics

A step with `note == 0` (REST) leaves the active chord unchanged. The sequencer fires on every step regardless; REST steps simply do not update `chord_root/type/inv`. Steps beyond `clen` are not advanced through.

---

## Known gaps / pending decisions

### K2 in entry mode
The user's spec mentioned "K2 = chord type" for entry mode but K1 is already chord type. K2 is currently unused during entry mode. Needs clarification before finalising.

### `cseq_notes()` inversion handling
The current implementation rotates semitone table entries for inversion. For 9th chords (5 notes collapsed to 4 semitone entries), this can produce unexpected voicing. An alternative bitmask-based implementation was shared by the user:

```c
static const uint16_t cq_mask[CQ_COUNT] = {
    0, 0x111, 0x109, 0x249, 0x129,        /* NONE MAJ MIN DIM AUG */
    0x891, 0x889, 0x489, 0xA49, 0x269, 0x889,  /* MAJ7 MIN7 DOM7 DIM7 M7B5 mMaj7 */
    0x491, 0x289, 0x689                    /* MAJ9 MIN9 DOM9 */
};
```

Consider swapping to this when the quantizer is implemented, as it handles chord membership cleanly regardless of voicing.

### Chord follow / scale quantize
The active chord state (`chord_root`, `chord_type`, `chord_inv`) is stored as statics in `seq.c`. The scale-quantize option will live in the SEL/SCL menu and will need to read these. No changes to seq.c should be needed on that side — just read the values.

---

## Memory footprint (FM-1_933)

- RAM (.data+.bss): 61992 / 98304 B (36.3 KB free)
- POOL: 338240 / 344064 B (5.8 KB free, 4 KB minimum enforced)
- The chord step array adds 64 B to .bss. No pool changes.

---

## Files changed

| File | What changed |
|------|-------------|
| `firmware/src/core.h` | `NCHORD_STEP`, `CQ_*` enum, `chord_step_t`, 4 accessors |
| `firmware/src/seq.c` | `CSEQ_SEMI`, `N_CQ`, chord statics, `cseq_notes()`, `cseq_tick()`, reset hook |
| `firmware/src/ui.c` | 4 fields added to `ui` struct |
| `firmware/src/ui_layers.c` | `LAYER_NAME[LY_STEP]`="chord", `cstep_label()`, full replacement of `LY_STEP` draw + knob + key handlers |
| `firmware/src/ui_input.c` | Entry key cleared on layer release; first-show guard updated for chord_page/chord_cursor |
| `firmware/src/project.c` | FUN7 magic, `project_v6_t`, `proj_from_v6()`, project_t extension, capture/apply |
| `tools/build.py` | `PRODUCT = "FM-1_933"` |
| `firmware/src/felucca.c` | `#define FELUCCA_ID "FM-1_933"` |

---

## Chord Follow (SHIFL 3.0 — FM-1_934)

### Parameter

`P_CFOL` added just before `P_E0` in `core.h` (position 63, pushing `P_E0` to 64). 14 modes defined in `CF_*` enum in `core.h`: `CF_OFF=0` through `CF_ROO5=13`.

### UI

K4 on the **"SCL 2"** page (`params.c` PAGES table) — page 2 of 2 of the SCL family, shown as "SCL 2 2/2" in the status bar. Turn K4 to cycle through the 14 modes. The slot was previously `0xFF` (empty).

### Preset behavior

`param_kept()` in `ui.c` now includes `|| i == P_CFOL`. Preset loads never touch it — must be changed manually.

### Saves with pattern

`P_CFOL` lives in `proj_trk_t.p[P_COUNT]` so it saves and loads with the project (slots A–H, autosave, song sections). A NEW/INIT project clears it to `CF_OFF`.

### Project format

Bumped to **FUN8** (`0x46554E38u`). FUN7 (`0x46554E37u`) migrates via `proj_from_v7()` in `project.c`:
- `PROJ_NP_V7 = 79u` records the old P_COUNT
- Params 0..62 copy directly; `p[P_CFOL]` initialised to 0; `P_E0..P_E15` shift from old 63..78 to new 64..79
- `project_v7_t` typedef holds the old layout for the size check

### Transformation engine (`seq.c`)

Four static helpers placed immediately after `cseq_tick()`, before the grid section:

| Function | Purpose |
|---|---|
| `seq_snap_scale(n, t)` | Snap to scale (moved out of `FELUCCA_ARRANGER` guard — now always available) |
| `cfol_snap(n, t, with_fifth)` | Snap to scale, optionally adding `chord_root+7` as a valid pitch class |
| `cfol_closest(ref, pcs, cnt)` | Nearest octave of any pitch class in `pcs[]` to `ref` |
| `cfol_apply(t, n)` | Dispatches on `t->p[P_CFOL]`; reads `chord_root/chord_type/chord_inv` from the chord sequencer statics in `seq.c` |

`cfol_apply` returns `n` unchanged if `is_drum(t)`, `chord_root == 0` (no active chord), or mode is `CF_OFF`.

### Integration points

- **Sequenced notes**: `seq_note_trans(t, n)` calls `cfol_apply(t, n)` first, then applies arranger transpose (if `FELUCCA_ARRANGER`).
- **Live keyboard notes**: `kb_nt[k][0] = (uint8_t)cfol_apply(t, n)` in the key-press path — applied when `P_CHORD == 0` only (chord-mode skips cfol). MIDI input is not affected (follow-up work).

### Mode algorithms

| Mode | Algorithm |
|---|---|
| `CF_OFF` | No transformation |
| `CF_LGL` | `seq_snap_scale(n, t)` |
| `CF_TRS1/2/3` | `n += cbuf[0/1/2] − 60`, then `seq_snap_scale` |
| `CF_TR15/25/35` | Same, but `cfol_snap(..., with_fifth=1)` adds `chord_root+7` as valid |
| `CF_CLS3` | `cfol_closest(n, cbuf[0..min(cn,3)-1])` |
| `CF_CLSA` | `cfol_closest(n, cbuf[0..cn-1])` |
| `CF_CL35` | `cfol_closest(n, cbuf[0..min(cn,3)-1] + [root+7])` |
| `CF_CLA5` | `cfol_closest(n, cbuf[0..cn-1] + [root+7])` |
| `CF_ROOT` | `cfol_closest(n, [chord_root])` |
| `CF_ROO5` | `cfol_closest(n, [chord_root, chord_root+7])` |

`cfol_closest` finds the pitch class from `pcs[]` whose nearest octave to `ref` minimises semitone distance; ties prefer the upward direction.

### Memory footprint (FM-1_934)

- RAM (.data+.bss): 62008 / 98304 B (+16 B vs FM-1_933)
- POOL: 338304 / 344064 B (+64 B vs FM-1_933)

### Files changed (FM-1_934)

| File | What changed |
|---|---|
| `firmware/src/core.h` | `CF_*` enum (14 modes), `P_CFOL` added before `P_E0` |
| `firmware/src/params.c` | `N_CFOL[]` names, `TP[P_CFOL]` descriptor, SCL 2 page K4 → `P_CFOL` |
| `firmware/src/ui.c` | `param_kept()` includes `P_CFOL` |
| `firmware/src/seq.c` | `seq_snap_scale`, `cfol_snap`, `cfol_closest`, `cfol_apply` after `cseq_tick`; `seq_note_trans` calls `cfol_apply`; live key path applies `cfol_apply` |
| `firmware/src/project.c` | FUN8 magic, `PROJ_NP_V7`, `proj_trk_v7_t`, `project_v7_t`, `proj_from_v7()`, `proj_import` chain, `proj_tmp` union |
| `tools/build.py` | `PRODUCT = "FM-1_934"` |
| `firmware/src/felucca.c` | `#define FELUCCA_ID "FM-1_934"` |
