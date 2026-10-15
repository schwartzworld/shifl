---
name: project-chord-seq
description: Chord sequencer added in SHIFL 3.0 — global 32-step chord progression track, hold-SEQ layer renamed CHORD
metadata:
  type: project
---

Global chord progression track added (SHIFL 3.0 / CHANGES-IAN.md entry 2026-10-14).

**Architecture:**
- 32-step `chord_step_t` array: 2 bytes/step (root MIDI note + packed type+inv+cond)
- 14 chord qualities `CQ_*` defined in `core.h`, semitone table `CSEQ_SEMI` in `seq.c`
- `cstep[]`, `clen`, `cseq_div` are static globals in `seq.c`; `chord_root/type/inv` track active chord
- `cseq_tick()` called every audio block to advance on grid boundaries
- Saves with project as FUN7 (`0x46554E37`); FUN6 files load with empty defaults via `proj_from_v6()`
- Display (`cstep_label()`) lives in `ui_layers.c` (needs `note_name` from `ui.c`)

**UI (hold-SEQ = LY_STEP layer):**
- Normal mode: K1=cursor, K2=sequence length, K3=step DIV (N_SDIV), K4=cond/REST
- Entry mode (white key held): K1=chord quality, K3=inversion; key sets root note
- `ui.chord_entry_key` = 0xFF when no entry in progress (initialized in `layers_init()`)
- `ui.chord_page`, `ui.chord_cursor` in the `ui` struct (ui.c)

**Why:** chord follow feature; active chord (chord_root/type/inv) readable for future scale-quantize option in SEL/SCL menu.

**How to apply:** When extending chord follow or the quantizer, chord state is in seq.c statics; display is in ui_layers.c `case LY_STEP:`.
