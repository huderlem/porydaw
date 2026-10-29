# The GBA CPU Meter

<!-- For hackers raising PCM polyphony or the mix rate: "how much of the
GBA's time does my song's sound take?" Plain language first, cycles second. -->

## What the number is

<!-- TODO: The status bar's `CPU` item and the CPU dock show how much GBA CPU
time the sound driver (m4aSoundMain) would take each frame, estimated from
the same engine that plays the song. No ROM or emulator needed; it updates
live while the song plays and shows "--" when stopped. -->

## VBlank and the whole frame

<!-- TODO: 100% is the VBlank window (83,776 cycles), the time the game counts
on for its VBlank work, shared with the sound driver. The whole frame
(280,896 cycles = 335%) is the hard ceiling: past it the game skips the next
frame's sound, and the music stutters. Red means that. -->

## Your CPU budget

<!-- TODO: Settings → Audio → CPU budget (default 100% of VBlank). Above it the
readout turns amber. Lower it if your game does heavy work during VBlank; it
is your judgement, porydaw cannot know the game's own load. -->

## The CPU dock

<!-- TODO: View → CPU. Current frame and peak (since playback last started
from stop) in large type with their cycles; Reset starts the peak over. The
breakdown bar: sequencer, CGB and mixer from the left; its first three
quarters are the VBlank window up to the 100% line, the tinted last quarter
is the rest of the frame (compressed) up to the red frame end; the amber line
is the budget, the triangle under the bar the peak. Reset while stopped
clears the peak until the song plays again. The legend's percentages are of
the VBlank window.
The line under it names the engine settings it was estimated for (mix rate,
PCM channels, stock or HQ mixer). Changing Settings → Audio moves it live. -->

## The three parts

<!-- TODO: Sequencer (MPlayMain, reading the song's commands), CGB (CgbSound,
the four PSG channels), Mixer (SoundMainRAM, mixing the PCM channels; the
big one: grows with channels playing and the mix rate; the HQ mixer costs
more per sample and has a fixed downsampler pass). -->

## The sequencer estimate

<!-- TODO: porydaw plays MIDI, not the compiled song, so the sequencer part is
estimated from the events ("(estimated)"); typically within a few hundred
cycles of the real thing. The mixer, the biggest part, is not estimated this
way. -->

## How accurate is it?

<!-- TODO: The cycle costs are measured against mGBA 0.10.5 running the real
m4a engine, not against hardware; mGBA's ROM prefetch model isn't perfect, so
a real GBA may differ by a few percent. -->

## What it doesn't cover

<!-- TODO: Only the sound driver: not the game's own VBlank handler or main
loop. Drifts for hacks with a modified mixer, a changed WAITCNT, samples
moved to RAM, or an engine other than the stock one / ipatix's HQ mixer. The
HQ mixer's stack depth is not checked. Keyboard notes and auditions are not
counted. -->
