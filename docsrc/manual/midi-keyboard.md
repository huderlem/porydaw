# Playing and Recording with a MIDI Keyboard

<!-- For musicians plugging in a keyboard for the first time. Symptom-first:
"I plugged it in, now what?" -->

## Connecting a keyboard

<!-- TODO: Plug in a USB MIDI keyboard; porydaw finds it on its own within
about 2 seconds, no restart (hot-plug both ways). Settings → MIDI lists every
input port with a checkbox to turn it on/off (remembered); a port another
program is holding shows "(unavailable)" — Refresh retries. Two identical
keyboards show as "Name" and "Name (2)". The Record button only appears while
a keyboard is connected. -->

<!-- TODO: Platform notes. Windows: works out of the box (WinMM). macOS:
CoreMIDI, works out of the box. Linux: needs the ALSA sequencer (the
snd-seq kernel module); WSL has no MIDI at all, so Settings → MIDI explains
that there's no MIDI system. -->

## Playing the selected track

<!-- TODO: Whatever you play sounds with the selected track's instrument
(its voice, volume and pan), even while the song plays — so you can jam
along. Every channel on the keyboard plays the same track. The roll's
piano keyboard lights the keys you hold. -->

<!-- TODO: Controls: sustain pedal holds notes; pitch bend bends the whole
track (song notes too) using the track's bend range; the mod wheel sets the
whole track's modulation, scaled to the MOD lane's visible range (MOD
defaults to 0..16). Switching tracks hands the old track's bend/mod back to
the song. -->

<!-- TODO: Settings → MIDI → "Play incoming notes" turns the sound off
(recording still works). "Fixed velocity" makes every note the same
loudness instead of how hard you hit the key. -->

<!-- TODO: Stuck notes: Tools → MIDI: All Notes Off. Unplugging the keyboard
also releases everything it held. -->

<!-- TODO: Polyphony applies — live notes share the GBA's voices with the
song, so a big chord over a busy song can cut notes (link polyphony.md). -->

## Recording

<!-- TODO: Select the track, put the edit cursor a bar before where you
want to start, press Record (or R in the piano roll). From stopped it plays
from the edit cursor and records; while playing, Record punches in, and
pressing it again punches out and playback keeps going. Stop, Pause,
switching tabs or closing the song also end the take. -->

<!-- TODO: What you see: your notes appear right away as faded notes and
grow while you hold them; the ruler is tinted red over the recorded span.
When the take ends they become real notes. -->

<!-- TODO: What lands in the song: one undo step per take ("Record N
notes") — one Ctrl+Z removes the whole take. Notes are placed on the
nearest tick (no quantizing — the Note Tools example plugin's Quantize
can tidy them afterwards, see plugins.md). Only notes are recorded: pitch
bend and the mod wheel are heard while you play but not written into the
song (draw them in the automation lanes). The sustain pedal lengthens recorded notes. Takes are added on top
of what's there (overdub); where a new note overlaps an old one on the same
pitch, the new one wins, the same as drawing a note over another. -->

<!-- TODO: Loop recording: with Loop on, keep playing pass after pass —
every pass goes into the same take; a note held across the loop end stops
at the loop end. -->

<!-- TODO: The status bar says what's being recorded into and how many
notes were recorded. Clicking another track mid-take keeps recording (and
sounding) into the original track. Moving the track keeps it following;
deleting it stops the recording and drops the take. -->

## Recording offset (if notes land late)

<!-- TODO: Why: you hear the song a moment after porydaw plays it (audio
buffering + your sound hardware), so you play slightly late. Settings → MIDI
→ Recording offset moves recorded notes earlier. Auto (shows "Auto (N ms)")
covers porydaw's own buffer only; Windows/drivers/Bluetooth add more that
porydaw can't measure. If notes land late, set a fixed value — try 30–60 ms
on a typical PC (the developer's machine needed about 50 ms); Bluetooth
headphones need much more. Find it by recording steady quarter notes
against the beat. -->

## Things to know

<!-- TODO: Keys held when you press Play/Stop/seek go silent (the engine
resets sound on every transport change) — press them again. -->

<!-- TODO: Recording a pitch the track itself is playing at the same moment
can cut one of them short: the GBA engine can't tell the two apart. -->

<!-- TODO: Editing the song while recording is allowed, but each edit
silences the keys you're holding (press them again) and ends their
recorded notes at the edit; a tempo edit mid-take shifts where the take
lands. -->

<!-- TODO: Program change, aftertouch and other keyboard messages don't do
anything in porydaw itself — plugins can still see them (link plugins.md;
the MIDI Monitor example shows everything your keyboard sends). -->
