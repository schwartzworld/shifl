# shifl-fm1 dev notes

## Bumping the firmware version

Two files must be updated together:

1. **`tools/build.py`** line ~50 — `PRODUCT = "FM-1_9XY"` — this is what actually gets embedded; it overrides the source-file define via `-DFELUCCA_ID`.
2. **`firmware/src/felucca.c`** — `#define FELUCCA_ID "FM-1_9XY"` — keep in sync so the source is readable without running the build.

The version string in `firmware/src/ui.c` (`FELUCCA_VERSION`, e.g. `"SHIFL 2.4"`) is the human-readable display string shown on-screen and is separate from the package identity number.

## Building

```sh
bash build.sh
```

Output lands in `build/felucca.fwsc`. The last line of the build confirms the identity string.
