# MIDI CC Controller Mapping Plan

## 1. Current State

**There are zero CC handlers in the firmware.** All CC messages (status byte 0xB0) are silently
discarded at `firmware/src/seq.c` line 1707:

```c
if (st != 0x90u && st != 0x80u)
    continue;  // <-- CC, pitch bend, program change all dropped here
```

The MIDI input pipeline is otherwise fully functional:
- TRS MIDI → `firmware/src/midi_uart.c` → `midi_in_q[]`
- USB MIDI → `firmware/src/usb.c` → same `midi_in_q[]`
- Packets drain each audio block in `events_block()` at `seq.c:1698`
- Packet format: `status<<8 | d1<<16 | d2<<24`, so `ch = (pkt>>8) & 0x0F`, `d1 = (pkt>>16) & 0x7F`, `d2 = (pkt>>24) & 0x7F`

The single insertion point for all CC handling is `seq.c:1707` — one `else if (st == 0xB0u)` branch.

---

## 2. Controller Layout

Your controller has: **16 pads × 2 banks** + **8 knobs × 2 layers**

### Pad Bank A — PUNCH FX (momentary, one pad per effect)

Program each pad as a **momentary CC** (value 127 while held, 0 on release) on **channel 16**.

| Pad | CC  | Effect     | Description                   |
|-----|-----|------------|-------------------------------|
| 1   | 102 | LOOP4      | 4-step loop stutter            |
| 2   | 103 | LOOP8      | 8-step loop stutter            |
| 3   | 104 | LOOP16     | 16-step loop stutter           |
| 4   | 105 | LOOP32     | 32-step loop stutter           |
| 5   | 106 | STUT       | Beat stutter                   |
| 6   | 107 | OCT        | Octave shift                   |
| 7   | 108 | STOP       | Freeze/stop                    |
| 8   | 109 | SLAP       | Slap echo                      |
| 9   | 110 | FLANGE     | Flanger                        |
| 10  | 111 | ECH2       | Echo x2                        |
| 11  | 112 | TEL        | Telephone filter               |
| 12  | 113 | CRUSH      | Bit crush                      |
| 13  | 114 | DOWN       | Pitch down                     |
| 14  | 115 | GATE       | Gate chop                      |
| 15  | 116 | ECHO       | Echo                           |
| 16  | 117 | WOBBLE     | Wobble/LFO stutter             |

### Pad Bank B — Drum Triggers (notes, already handled)

Program as **note-on/note-off** on **channel 10**. No firmware work needed — the drum channel
already dispatches by nearest-note. Map the 16 pads to the 16 drum sound pitches already in the
preset (check the drum kit mapping in the UI to get the right note numbers).

### Knob Layer 1 — Per-Track Filter (each knob sends on its track's channel)

| Knob | Channel | CC | Maps to                  |
|------|---------|----|--------------------------|
| 1    | 1       | 74 | Track 1 Filter Cutoff (P_E4) |
| 2    | 1       | 71 | Track 1 Resonance (P_E5) |
| 3    | 2       | 74 | Track 2 Filter Cutoff    |
| 4    | 2       | 71 | Track 2 Resonance        |
| 5    | 3       | 74 | Track 3 Filter Cutoff    |
| 6    | 3       | 71 | Track 3 Resonance        |
| 7    | 16      | 74 | Master DJ Filter (G_FILT, 64=center/off) |
| 8    | 16      | 71 | Master DUST (lo-fi) (G_DUST) |

> CC 74 and CC 71 are the MIDI standard numbers for filter cutoff and resonance.
> Knobs 7–8 use channel 16 (the global CC channel).

### Knob Layer 2 — Global FX

Program all on **channel 16**.

| Knob | CC | Maps to                  | Notes                   |
|------|----|--------------------------|-------------------------|
| 1    | 24 | Delay Feedback (G_DFDBK) | 0–127 direct            |
| 2    | 25 | Delay Color (G_DCOLOR)   | 0–127 direct            |
| 3    | 26 | Delay Mix (G_DMIX)       | 0–127 direct            |
| 4    | 27 | Reverb Size (G_RSIZE)    | 0–127 direct            |
| 5    | 28 | Reverb Damp (G_RDAMP)    | 0–127 direct            |
| 6    | 29 | Chorus Rate (G_CRATE)    | 0–127 direct            |
| 7    | 30 | Chorus Depth (G_CDEPTH)  | 0–127 direct            |
| 8    | 20 | Global Swing (G_SWING)   | 0–100 (clamp at 100)    |

---

## 3. Implementation Plan

All changes live in `firmware/src/seq.c` unless noted. The tasks are ordered by dependency.

---

### Task 1 — CC Dispatch Skeleton

**Goal:** Stop dropping CC messages; add a dispatchable branch.

**Prompt for agent:**

> In `firmware/src/seq.c`, inside the function `events_block()`, find the while loop that drains
> `midi_in_q[]` (around line 1698). The loop currently decodes each 32-bit packet into `st` (status
> high nibble), `ch` (channel 0-indexed), `d1`, and `d2`. At approximately line 1707 there is:
>
> ```c
> if (st != 0x90u && st != 0x80u)
>     continue;
> ```
>
> Replace that guard with a structure that handles three cases:
>
> 1. Note-on (`st == 0x90u`) — existing note-on logic follows, unchanged.
> 2. Note-off (`st == 0x80u`) — existing note-off logic follows, unchanged.
> 3. CC (`st == 0xB0u`) — call a new static function `midi_cc(ch, d1, d2)` that you add just
>    above `events_block()`.
> 4. Anything else — `continue` (keep the existing drop behavior).
>
> The new `midi_cc` function should be a stub for now:
>
> ```c
> static void midi_cc(uint32_t ch, uint32_t cc, uint32_t val) {
>     (void)ch; (void)cc; (void)val;
> }
> ```
>
> Do not move or alter any of the existing note-on/note-off logic. Do not change any other file.
> Build with `bash build.sh` and confirm it compiles cleanly.

---

### Task 2 — Per-Track CC Handler (cutoff, resonance)

**Goal:** CC 74 → P_E4 (filter cutoff) and CC 71 → P_E5 (resonance) on channels 1–3.

**Context for agent:**
- MIDI channels are 0-indexed internally: `ch=0` = track 1, `ch=1` = track 2, `ch=2` = track 3.
- `trk[n].p[]` is the parameter array for track `n` (where `n` = 0, 1, 2 for synths).
- The param IDs `P_E4` and `P_E5` are defined in `firmware/src/core.h`.
- The helper `midi_track(ch)` (defined near seq.c line 1493) maps a channel to a track index;
  it returns -1 if the channel has no track.
- Changing a parameter via CC must also mark the track dirty for the display. Look at how the
  keyboard note handler calls `param_set()` or directly assigns `trk[n].p[P_Exx]`, and do the same.

**Prompt for agent:**

> In `firmware/src/seq.c`, fill in the `midi_cc(ch, cc, val)` stub added in Task 1.
>
> Add handling for the following CCs on MIDI channels 0–2 (tracks 1–3):
>
> - **CC 74**: set `trk[t].p[P_E4]` to `val` (filter cutoff). `P_E4` is defined in `core.h`.
> - **CC 71**: set `trk[t].p[P_E5]` to `val` (resonance). `P_E5` is defined in `core.h`.
>
> Where `t = midi_track(ch)` — use the existing `midi_track()` helper to resolve channel to
> track index. Guard with `if (t < 0) return;` for unmapped channels.
>
> These are per-voice engine parameters. Look at how the existing `param_set()` function (or
> equivalent direct assignment) is used elsewhere in seq.c when the UI changes a parameter — mirror
> that pattern exactly so the engine and display stay in sync.
>
> Do not touch any other CCs or parameters yet. Build with `bash build.sh` and confirm clean.

---

### Task 3 — Global FX CC Handler (channel 16)

**Goal:** Layer 2 knobs control global FX parameters on MIDI channel 16 (`ch=15` internally).

**Context for agent:**
- `song.g[]` is the global parameter array. IDs are in `firmware/src/core.h`.
- `G_SWING` range is 0–100; clamp `val` to 100.
- All others are 0–127 direct.
- The master DJ filter `G_FILT` is bipolar (-64..63). CC 74 on ch 15 maps 0–127 → value-64.

**Prompt for agent:**

> In the `midi_cc(ch, cc, val)` function in `firmware/src/seq.c`, add a second branch for
> **channel 15** (`ch == 15`, which is MIDI channel 16).
>
> Map the following CCs to global parameters in `song.g[]`:
>
> | CC | Global param | Notes |
> |----|--------------|-------|
> | 20 | G_SWING | clamp val to 100 |
> | 24 | G_DFDBK | direct (0–127) |
> | 25 | G_DCOLOR | direct |
> | 26 | G_DMIX | direct |
> | 27 | G_RSIZE | direct |
> | 28 | G_RDAMP | direct |
> | 29 | G_CRATE | direct |
> | 30 | G_CDEPTH | direct |
> | 71 | G_DUST | direct |
> | 74 | G_FILT | map: `(int32_t)val - 64` (so 64→0=off, 0→-64=LP, 127→+63=HP) |
>
> Use the same parameter assignment pattern identified in Task 2. Build cleanly.

---

### Task 4 — PUNCH FX CC Handler (channel 16, CC 102–117, momentary)

**Goal:** Pads on bank A trigger PUNCH FX as long as the CC value is nonzero (hold-style).

**Context for agent:**
- The PUNCH FX state lives in a global struct. Relevant fields (from `firmware/src/punch.c`):
  - `punch.req` — `int8_t`, the effect index to run (-1 = none)
  - `punch.hold` — `int8_t`, nonzero means the effect is being held active
- Effect indices (enum in punch.c): `PX_LOOP4=0, PX_LOOP8=1, ..., PX_WOBBLE=15`
- The audio ISR reads `punch.hold ? punch.req : punch.song` each block.
- The semantic to implement: CC 102–117 → effects 0–15. Value > 0: set `punch.req = index`,
  `punch.hold = 1`. Value == 0: if `punch.req == index`, set `punch.hold = 0`, `punch.req = -1`.
- Only one effect can be active at a time; a new non-zero CC replaces the previous one.
- The struct and fields may need to be declared `extern` or accessed via a header. Check
  `firmware/src/punch.c` and `firmware/src/fx.h` (or wherever `punch` is declared) for the
  correct access pattern. Do not add a new extern declaration if one already exists.

**Prompt for agent:**

> In the `midi_cc(ch, cc, val)` function in `firmware/src/seq.c`, add PUNCH FX handling.
>
> Regardless of channel: if `cc` is in the range 102–117 inclusive:
>
> 1. Compute effect index: `int idx = (int)cc - 102;` (0–15)
> 2. If `val > 0`: set `punch.req = (int8_t)idx` and `punch.hold = 1`.
> 3. If `val == 0`: only clear if this is the currently held effect —
>    `if (punch.req == idx) { punch.hold = 0; punch.req = -1; }`
>
> Find where `punch` is declared (likely `fx.c` or `fx.h`) and confirm the correct extern
> reference is visible in `seq.c`. If `punch` is already referenced elsewhere in `seq.c`, use the
> same mechanism.
>
> Test the boundary: sending CC 102 value 127 should start LOOP4; sending CC 102 value 0 should
> stop it. Sending CC 110 while 102 is held should switch immediately to FLANGE. Build cleanly.

---

## 4. CC Number Reference Card

> (Program this into your controller's patch memory)

```
CHANNEL 1  — Track 1
  CC 71  Resonance
  CC 74  Filter Cutoff

CHANNEL 2  — Track 2
  CC 71  Resonance
  CC 74  Filter Cutoff

CHANNEL 3  — Track 3
  CC 71  Resonance
  CC 74  Filter Cutoff

CHANNEL 10 — Drums (note-on only, no CC needed)

CHANNEL 16 — Global / FX
  CC 20  Swing
  CC 24  Delay Feedback
  CC 25  Delay Color
  CC 26  Delay Mix
  CC 27  Reverb Size
  CC 28  Reverb Damp
  CC 29  Chorus Rate
  CC 30  Chorus Depth
  CC 71  Master Lo-Fi (DUST)
  CC 74  Master DJ Filter (64=off, <64=LP, >64=HP)

  CC 102  PUNCH: LOOP4    (momentary)
  CC 103  PUNCH: LOOP8    (momentary)
  CC 104  PUNCH: LOOP16   (momentary)
  CC 105  PUNCH: LOOP32   (momentary)
  CC 106  PUNCH: STUT     (momentary)
  CC 107  PUNCH: OCT      (momentary)
  CC 108  PUNCH: STOP     (momentary)
  CC 109  PUNCH: SLAP     (momentary)
  CC 110  PUNCH: FLANGE   (momentary)
  CC 111  PUNCH: ECH2     (momentary)
  CC 112  PUNCH: TEL      (momentary)
  CC 113  PUNCH: CRUSH    (momentary)
  CC 114  PUNCH: DOWN     (momentary)
  CC 115  PUNCH: GATE     (momentary)
  CC 116  PUNCH: ECHO     (momentary)
  CC 117  PUNCH: WOBBLE   (momentary)
```

---

## 5. Notes and Caveats

- **Engine-specific cutoff**: `P_E4`/`P_E5` are only meaningful on the ANALOG engine. On DIGITAL
  (FM) they control FM index and carrier ratio; on PHASE they control phase depth. The CC will
  write the value regardless. That may be desirable (knob always controls the engine's primary
  timbre parameter) or not — decide before testing.

- **Drum track**: Drums have no filter. CC 74/71 on channel 10 should probably be silently
  ignored rather than writing garbage to the drum parameter table. The `midi_track()` guard
  (Task 2) handles this if the drum track's parameter set doesn't include P_E4.

- **Pad bank B**: No firmware work is required. The 16 drum sounds already respond to note-on on
  channel 10. Just map your pads to the correct note numbers for whatever drum kit is loaded.

- **Parameter display**: After a CC write, the on-screen parameter display may not update unless
  the firmware's dirty/redraw mechanism is triggered. If values change silently, check whether
  `param_set()` is preferred over direct assignment for UI refresh.

- **Implementation order**: Tasks 1→2→3→4. Each task compiles independently. Do not start
  Task 2 without Task 1's stub in place.
