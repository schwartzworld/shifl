# Memory savings implementation guide

Three independent changes, listed in dependency order. Read this whole file before touching any code.

---

## Context

The firmware has two memory regions that matter here:

| Region | Size | Pressure |
|--------|------|----------|
| `.bss`/`.data` RAM | 96 KB | **CRITICAL — ~160 B free** |
| `.pool` (NOLOAD) | 336 KB | Nearly full — ~1 KB free before change 1 |

The build system is a unity build: all `.c` files are `#include`d into
`firmware/src/felucca.c`. There is no separate compilation of individual
source files. Build with `bash build.sh` from repo root; output is
`build/felucca.fwsc`.

---

## Change 1 — Shorten delay buffer from 1.0 s → 0.75 s (saves 22 KB pool)

**This change must happen first** — it frees the pool space that changes 2 and 3 require.

**File:** `firmware/src/fx.c`, line 5

```c
// Before:
#define DLY_LEN 44100u           /* 1.00 s at 44.1 kHz; NOT power-of-2 — use explicit wrap, not bitmask */

// After:
#define DLY_LEN 33075u           /* 0.75 s at 44.1 kHz; NOT power-of-2 — use explicit wrap, not bitmask */
```

**Why this is safe:**
- `DLY_LEN` is only used in three places in `fx.c`:
  - The buffer declaration (line 7)
  - `delay_samples()` (line 185): guards `s >= DLY_LEN` — still correct
  - The write-pointer wrap (line 229): `if (++fx.dly_w >= DLY_LEN)` — still correct
- The punch FX echo effects (`PX_ECHO`, `PX_ECH2`, `PX_ECH4`) use `punch_ring` (a separate 32 KB buffer), NOT `dly_buf`. They are unaffected.
- 0.75 s covers all tempo-synced delays up to ~160 BPM at 2 bars.

**Pool savings:** (44100 − 33075) × 2 bytes = **22,050 B freed**

---

## Change 2 — Move `rev_line` from RAM to pool (saves 17 KB RAM)

**Depends on change 1** being applied first, otherwise pool overflows.

**File:** `firmware/src/fx.c`, line 15

```c
// Before:
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2];   /* (.bss: the pool is full) */

// After:
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2] __attribute__((section(".pool")));
```

Also update the comment (it's no longer true that the pool is full):

```c
static int16_t rev_line[1559 + 1931 + 2389 + 2791 + REV_MOD + 2] __attribute__((section(".pool")));  /* FDN reverb delay lines */
```

**Why this is safe:**
- `rev_line` contains reverb delay-line samples. They start at zero (silence) and are written before they are read; zero-initialization at boot is fine.
- `.pool` is a NOLOAD section that the startup code zero-initializes, same as `.bss`. Verify this by grepping for `.pool` in `firmware/app.ld` and the startup/init code — confirm it is either in the BSS clear loop or has its own zeroing pass.
- `rev_ap` (line 16) is already in `.pool` with no issues, confirming reverb buffers belong there.

**RAM savings:** (1559 + 1931 + 2389 + 2791 + REV_MOD + 2) × 2 bytes ≈ **17,368 B freed from RAM**

**Pool cost:** 17,368 B  
**Pool budget after changes 1+2:** 22,050 − 17,368 = **~4,682 B remaining free in pool**

---

## Change 3 — Move `autosave_buf` to pool (saves 3.3 KB RAM)

**Depends on changes 1+2** (needs the pool space they free).

**File:** `firmware/src/project.c`, line 544

```c
// Before:
static project_t autosave_buf;

// After:
static project_t autosave_buf __attribute__((section(".pool")));
```

**Why this is safe:**
- `autosave_buf` is used only in `autosave_tick()` (writes a snapshot, then saves to flash) and `autosave_resume()` (power-on restore). It is never accessed from an ISR or DMA. `.pool` is valid for this.
- Zero-initialization is fine: the buffer is always written (`proj_capture` or `st_load`) before it is read for meaningful content.
- `autosave_hash` (line 545) is a plain `uint32_t` — leave it in `.bss`, it's tiny.

**RAM savings:** sizeof(project_t) ≈ **3,274 B freed from RAM**

**Pool cost:** 3,274 B  
**Pool budget after changes 1+2+3:** ~4,682 − 3,274 = **~1,408 B remaining free in pool**

---

## What was considered but rejected

### Union `autosave_buf` + `proj_tmp`
**NOT SAFE.** `autosave_resume()` (project.c:578) loads flash into `proj_tmp` on line 585
then passes `&autosave_buf` as the destination to `proj_import` on line 586. Both are live
simultaneously. A union would alias the source and destination of a copy — corrupt data.

### Union `proj_v3_tmp` + `proj_tmp`
**NOT SAFE.** `proj_from_v2()` and `proj_from_v1()` (called from `proj_import`) take
`proj_tmp` as their source and use `proj_v3_tmp` as an intermediate scratch buffer. Both
are live simultaneously during v1/v2 format migration.

### Move `proj_tmp` or `proj_v3_tmp` to pool
Technically valid for safety, but after changes 1–3 the pool has only ~1.4 KB free — not
enough for either buffer (project_t ≈ 3,274 B, project_v3_t = 2,584 B). Would require
finding additional pool savings first (e.g. reducing `proj_slot[6]` to 5 slots frees
~3,274 B of pool, which would then fit one more move).

### Scatter 16× → 4× to shrink `punch_ring`
Does not reduce `PUNCH_N`. The ring size is driven by the echo effects (`PX_ECHO`,
`PX_ECH2`, `PX_ECH4`, `PX_SLAP`) which read up to `PUNCH_N − 64` samples back. The
scatter effect (PX_SHUF) reads 16 × (beat/16) = 1 beat — the same as echo. Changing
scatter alone leaves the constraint unchanged.

### Remove delay COLOR param
Saves ~0 RAM. `G_DCOLOR` is one entry in `song.g[]`. The single-pole IIR in the delay
feedback loop has no buffer of its own.

---

## Expected totals after all three changes

| What | Where saved | Amount |
|------|-------------|--------|
| `rev_line` → pool | RAM | 17,368 B |
| `autosave_buf` → pool | RAM | 3,274 B |
| **Total RAM freed** | | **~20,642 B** |
| DLY_LEN shrink | pool | 22,050 B consumed by changes 2+3 |

From ~160 B free → roughly **20.8 KB free** in RAM.
Pool ends at roughly 1.4 KB free (tight but viable — same as before if no new allocations added).

---

## Verification after making changes

1. Build must succeed: `bash build.sh`
2. Check that the last build line confirms the identity string (per CLAUDE.md).
3. Confirm `.pool` section does not overflow: look for linker errors about `.pool` region.
4. Confirm `.bss`/RAM section does not overflow: look for linker errors about RAM region.
5. Optionally run `arm-none-eabi-size build/felucca.elf` (or equivalent) and confirm `.bss`
   decreased by ~20 KB and `.pool` is within its 336 KB limit.
