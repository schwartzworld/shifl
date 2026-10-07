# MIDI Controller Setup Guide

This guide tells you exactly how to program every knob and pad on your controller to work with the FM-1.

---

## The Big Picture

The FM-1 listens on **multiple MIDI channels at once**:

| Channel | What it controls |
|---------|-----------------|
| **Channel 1** | Track 1 filter |
| **Channel 2** | Track 2 filter |
| **Channel 3** | Track 3 filter |
| **Channel 10** | Drums (pads only — hit to trigger) |
| **Channel 16** | Global FX, swing, delay, reverb, chorus, master filter, punch effects |

---

## Programming a Knob

Open the knob's edit panel. You'll see these fields:

- **Type** → set to **CC**
- **Channel** → pick the channel from the table above
- **Curve** → leave as **Normal** unless you have a reason to change it
- **CC** → the CC number from the table below
- **Min** → **0**
- **Max** → **127**

Then hit **Close**.

---

## Programming a Pad

Open the pad's edit panel. You'll see these fields:

- **Type** → set to **Note**
- **Channel** → set to **Channel 10** (drums)
- **Note** → the note number for the drum sound you want (0–127; check your kit's mapping)
- **Min** → **0**
- **Max** → **127**
- **Led** → **255** (full brightness, or pick a lower value to dim it)
- **Color** → tap the color swatch and pick whatever color helps you remember what the pad does

Then hit **Close**.

### Punch FX pads (momentary effects)

Punch effects work differently: they activate while the pad is **held** and release when you **let go**. Program them as notes **or** as momentary CC controls:

- **Type** → **CC** (if you want hold-to-activate behavior via CC)
- **Channel** → **Channel 16**
- **CC** → the punch CC number from the table below
- **Min** → **0**
- **Max** → **127**

Send value > 0 to start the effect, value 0 to stop it. Most controllers can do this automatically with a "momentary" mode.

---

## CC Number Reference

### Channels 1, 2, 3 — Per-track filters

| CC | Parameter |
|----|-----------|
| **71** | Resonance |
| **74** | Filter Cutoff |

Set the channel to match the track (knob → Ch 1 for Track 1, Ch 2 for Track 2, Ch 3 for Track 3).

---

### Channel 16 — Global controls

#### Mix & time

| CC | Parameter | Notes |
|----|-----------|-------|
| **20** | Swing | 0 = no swing, 127 = max swing |

#### Delay

| CC | Parameter |
|----|-----------|
| **24** | Delay Feedback |
| **25** | Delay Color |
| **26** | Delay Mix |

#### Reverb

| CC | Parameter |
|----|-----------|
| **27** | Reverb Size |
| **28** | Reverb Damp |

#### Chorus

| CC | Parameter |
|----|-----------|
| **29** | Chorus Rate |
| **30** | Chorus Depth |

#### Master

| CC | Parameter | Notes |
|----|-----------|-------|
| **71** | Master Lo-Fi (DUST) | 0 = clean, 127 = maximum grit |
| **74** | Master DJ Filter | **64 = filter off.** Below 64 = low-pass (darker). Above 64 = high-pass (thinner). |

#### Punch FX (momentary — hold to activate, release to stop)

| CC | Effect |
|----|--------|
| **102** | LOOP4 |
| **103** | LOOP8 |
| **104** | LOOP16 |
| **105** | LOOP32 |
| **106** | STUT |
| **107** | OCT |
| **108** | STOP |
| **109** | SLAP |
| **110** | FLANGE |
| **111** | ECH2 |
| **112** | TEL |
| **113** | CRUSH |
| **114** | DOWN |
| **115** | GATE |
| **116** | ECHO |
| **117** | WOBBLE |

Only one punch effect can be active at a time. If you hold LOOP4 and then press FLANGE, FLANGE takes over immediately.

---

## Quick-reference cheat sheet

```
Ch 1  CC 71 = Track 1 Resonance      Ch 1  CC 74 = Track 1 Cutoff
Ch 2  CC 71 = Track 2 Resonance      Ch 2  CC 74 = Track 2 Cutoff
Ch 3  CC 71 = Track 3 Resonance      Ch 3  CC 74 = Track 3 Cutoff

Ch 10  Notes = Drum pads (kit-dependent note numbers)

Ch 16  CC 20  = Swing
Ch 16  CC 24  = Delay Feedback
Ch 16  CC 25  = Delay Color
Ch 16  CC 26  = Delay Mix
Ch 16  CC 27  = Reverb Size
Ch 16  CC 28  = Reverb Damp
Ch 16  CC 29  = Chorus Rate
Ch 16  CC 30  = Chorus Depth
Ch 16  CC 71  = Master Lo-Fi
Ch 16  CC 74  = DJ Filter (64=off)

Ch 16  CC 102–117 = Punch FX (momentary, 102=LOOP4 … 117=WOBBLE)
```
