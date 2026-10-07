# Rebase notes

Notes on non-obvious conflicts resolved when rebasing onto upstream, so the same decisions don't need to be re-litigated next time.

---

## 2026-10-07 — FM6 + visualizer onto upstream (post-FM-1_912)

### Context

Upstream added a visualizer (`ui_vis.c`) and scope buffers in `audio.c` at the same time as our FM6 engine work landed. After rebasing, the build failed with:

```
region `RAM' overflowed by 8468 bytes
```

### Root cause

The `.pool` region has a **fixed** start VMA (`0x1c20000`). `.bss` must not grow past it. Upstream's `ui_vis.c` and `audio.c` added several large audio/FFT buffers but didn't tag them `.pool` — presumably the upstream tree had more headroom. Our FM6 additions (`fm6_note`, `fm6_eff`, `fm6_patch`, etc.) consumed the remaining slack and pushed `.bss` over.

### Resolution

Added `__attribute__((section(".pool")))` to five buffers that have no fast-RAM requirement:

| Buffer | File | Size |
|---|---|---|
| `scope_buf[512]`, `scope_bufr[512]` | `firmware/src/audio.c` | 2,048 B |
| `vis_l[512]`, `vis_r[512]` | `firmware/src/ui_vis.c` | 2,048 B |
| `vis_lj[3][256][2]` | `firmware/src/ui_vis.c` | 1,536 B |

Total freed from `.bss`: **5,632 B** — enough to clear the 8,468 B overflow given the FM6 additions (~2–3 KB combined).

### For future rebases

If upstream adds more visualizer buffers (anything in `ui_vis.c` or `audio.c` prefixed `vis_` or `scope_`), check whether they got `.pool` annotations. Large ring buffers and FFT scratch arrays should always go to `.pool`; only small, frequently-accessed state needs fast RAM.
