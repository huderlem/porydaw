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
on for its VBlank work, shared with the sound driver. Past it the readout is
red: the sound driver eats into the rest of the frame, whatever your budget.
The whole frame (280,896 cycles = 335%) is shown for scale only. -->

## Your CPU budget

<!-- TODO: Settings → Audio → CPU budget, in CPU cycles per frame (default
60,000, about 72% of the VBlank window; the percent is shown beside it).
Above it the readout turns amber. Lower it if your game does heavy work during VBlank; it
is your judgement, porydaw cannot know the game's own load. -->

## The CPU dock

<!-- TODO: View → CPU. At the top, whether this frame is Within budget, Over
budget or Past VBlank (Stopped when not playing). The current frame's percent
in large type and its cycles. The meter:
one scale from 0 to 140% of the VBlank window; mixer, sequencer and CGB
stacked from the left; the amber line is your budget, the red line the end of
VBlank (hatched past it), the dashed line the peak. "Last 10 seconds": every
frame of the last ten seconds of playback, newest at the right; the caption
gives the highest and how often it was over budget; after a stop the last run
stays, dimmed, until you play again from the start of a run. "Where it goes":
each part's cycles and share of the frame ("est." on the sequencer, see
below). The peak card: the peak since Play, its cycles and where in the song it
happened (m:ss.s); Reset peak starts the peak and the 10 seconds over. Reset
while stopped clears the peak until the song plays again, and so does a change
of mix rate, mixer or voicegroup (a voice edit that reloads it) while stopped;
nothing is shown as a peak before the song has played. The legend at the
bottom: Budget, VBlank and the whole frame (for scale), in cycles and percent.
The line under it names the engine settings it was estimated for (mix rate,
PCM channels, stock or HQ mixer). Changing Settings → Audio moves it live. -->

## The profile strip

<!-- TODO: The thin "CPU" strip under the ruler (hidden until turned on with
View → CPU Profile; the choice is remembered): the whole song's cost per
frame without playing it, from a quick render of the song in the background
(a looping song once through to its loop end; any other to its end plus the
3 s release tail playback plays) with the same engine settings. Colour per
column = the most expensive frame there: a cool grey for cheap frames,
warming as it nears the budget, amber above the budget, red past the VBlank
window. Hover a spot for its percent, cycles, bar and the three parts; hover
the "CPU" label for the song's peak. It dims ("profiling…") for a moment
after an edit or a Settings → Audio change, then refreshes. Changing the
budget recolours it at once. View → CPU Profile, or right-click → Hide CPU
Profile, hides it (and stops the background renders). Songs over 10 minutes
are not profiled. -->

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
HQ mixer decodes compressed and reversed samples onto the stack and skips or
copies differently by the stack's depth at VBlank: porydaw models the test
ROM's stack pointer (0x03007DD0); a game whose VBlank stack sits elsewhere can
cost differently there. Keyboard notes and auditions are not counted. -->

## For plugin authors

<!-- TODO: porydaw.audio.cpu() gives the same numbers as the CPU dock
(cycles, percent, peak, the three parts, budget in cycles and as a percent);
porydaw.audio.cpuProfile() gives the profile strip's frames ({tick, cycles,
percent}), its max and the budget. Both
null when there is nothing to show; see the scripting reference. -->
