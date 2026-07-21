---
name: verify
description: Build porydaw and observe changes at runtime — in-binary harnesses plus offscreen widget driving.
---

# Verifying porydaw changes

## Build

```bash
cmake --build build -j"$(nproc)"
```

## In-binary harnesses (the repo's convention)

Model/engine changes are covered by `--*check` flags on the main binary
(see `src/main.cpp` for the full list and required args — every flag needs
ALL of its args). They write into the project, so always run against a
**fresh scratch copy** of a decomp project.

Find a decomp project to copy, in this order:

1. `$PORYDAW_TEST_PROJECT` if set.
2. An existing checkout of pokeemerald/pokefirered/pokeruby near this repo
   (check siblings of the repo and `~`; it's a decomp project if it has
   `sound/song_table.inc`). Prefer a vanilla checkout over forks/hacks
   unless the change under test targets a specific fork.
3. Otherwise shallow-clone one:
   `git clone --depth 1 https://github.com/pret/pokeemerald "$(mktemp -d)/pokeemerald"`
   (no ROM build needed — the harnesses only read the source tree).

```bash
SCRATCH=$(mktemp -d)/scratch
cp -r "$DECOMP_PROJECT/." "$SCRATCH"
QT_QPA_PLATFORM=offscreen ./build/porydaw --vgcheck "$SCRATCH" mus_abandoned_ship
```

`QT_QPA_PLATFORM=offscreen` is required when there's no display
(WSL/CI/headless); it's harmless otherwise. Song labels come from
`sound/song_table.inc`. `sound/songs/midi/midi.cfg` is CRLF — keep
byte-conservative edits. (`mus_abandoned_ship` is a vanilla pokeemerald
label — pick a label from the project's own `song_table.inc` if using a
different project.)

## Full sweep + AddressSanitizer

`tools/run_checks.sh` runs EVERY harness against fresh scratches — use it
for the pre-push ritual instead of hand-running flags:

```bash
tools/run_checks.sh build/porydaw "$DECOMP_PROJECT" [songsmk-fork]
```

The third argument is a songs.mk-only fork checkout for `--mkcheck`
(skipped with a note if omitted); `PORYDAW_SAMPLE_CORPUS=<built tree>`
adds samplecheck's corpus pass. For memory-bug detection (use-after-free
from mid-event widget rebuilds passes SILENTLY in a normal build), run the
sweep on the ASAN build — CI does this on every push (`asan-checks` job):

```bash
cmake -B build-asan -DPORYDAW_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j"$(nproc)"
tools/run_checks.sh build-asan/porydaw "$DECOMP_PROJECT"
```

The script defaults `ASAN_OPTIONS=detect_leaks=0` (Qt's process-lifetime
allocations drown real leaks).

## Driving a widget the harnesses don't cover

Pattern (used for the voicegroup browser's editor panel): a standalone
CMake project in a scratch dir that compiles the widget's .cpp files from
this repo directly, links `Qt6::Widgets Qt6::Test` plus a static lib built
from `external/poryaaaa/plugin/*.c`, instantiates the widget offscreen,
drives it with QTest key events / combo popups, asserts on the source
model, and saves `widget.grab().toImage()` as evidence. Include dirs:
`src`, `external/poryaaaa/plugin`, `external/poryaaaa/third_party`;
`CMAKE_AUTOMOC ON`. Find private child widgets via `findChildren` +
tooltips/item text, not member access.

For a rendered-view smoke test of a song, `--viewcheck <root> [song shot.png]`
already saves a SongView screenshot.
