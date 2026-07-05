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
**fresh scratch copy** of a decomp project:

```bash
SCRATCH=$(mktemp -d)/scratch
cp -r /home/huderlem/pokeemerald/. "$SCRATCH"
QT_QPA_PLATFORM=offscreen ./build/porydaw --vgcheck "$SCRATCH" mus_abandoned_ship
```

`QT_QPA_PLATFORM=offscreen` is required (headless WSL). Song labels come
from `sound/song_table.inc`. `sound/songs/midi/midi.cfg` is CRLF — keep
byte-conservative edits.

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
