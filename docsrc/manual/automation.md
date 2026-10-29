# Automation (Volume, Pan, Tempo & More)

## What automation is

<!-- TODO: Values that change over time instead of staying fixed — fade-outs,
panning sweeps, tempo changes, vibrato depth. Drawn as line/step lanes under
the piano roll. -->

## Adding a lane

<!-- TODO: Per-track lane picker from the m4a parameter list; the tempo lane
always available at the top level. -->

## The parameters you can automate

<!-- TODO: Friendly table of the m4a-meaningful controls with plain-language
descriptions and typical ranges:
- Volume, Pan
- Instrument (program change)
- MOD (vibrato depth) / MODT (LFO type: 0 vibrato, 1 tremolo, 2 auto-pan)
- BEND (pitch bend) / BENDR (bend range)
- LFOS / LFODL (vibrato speed / delay before it starts on each note)
- TUNE (fine tune, c_v-centred like PAN)
- Tempo (top-level lane)
Flag which ones quantize on GBA hardware. -->

## Drawing and editing values

<!-- TODO: Click/drag to draw, step vs. line rendering, snapping, deleting
points; undoable like everything else. -->

## The value axis and zooming

<!-- TODO: Gutter menu → Value range (auto-fit or fixed 0–16/32/64/127,
plus 0–24 on BENDR and 0–2 on MODT); display-only — data is never clipped;
MOD auto-fits because its useful range is small. -->

## Tempo changes

<!-- TODO: The tempo lane; BPM; how tempo interacts with the position display
and loops. -->
