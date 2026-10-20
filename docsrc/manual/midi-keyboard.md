# Playing and Recording with a MIDI Keyboard

<!-- For musicians plugging in a keyboard for the first time. Symptom-first:
"I plugged it in, now what?" -->

No MIDI keyboard? See [The Virtual MIDI Keyboard](virtual-keyboard.md): play
and record with the mouse and the computer keyboard.

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

## Knobs, sliders and program buttons

<!-- TODO: Routing: Settings → MIDI → Controls says which knob (MIDI CC)
drives MOD, VOL, PAN, BENDR and LFOS. Defaults: CC1 (mod wheel) → MOD, CC7 →
VOL, CC10 → PAN; BENDR and LFOS none. Pick a CC number, None, or Learn: click
Learn and move the knob within 10 seconds (Esc cancels). Or right-click a
lane's name → "Learn MIDI control…" (the menu shows its current knob, e.g.
"MIDI: CC 74"; only while a keyboard is connected). One knob per lane: learning
a knob another lane has moves it there (the message says which). Routes apply
to every song and every keyboard channel. The sustain pedal, bank select,
data entry and channel mode messages can't be routed. -->

<!-- TODO: What a full turn reaches: VOL, PAN and LFOS 0..127 as the knob
sends it; BENDR 0..24 (the BENDR lane shows 0..24 by default); MOD follows
the MOD lane's visible range, like the mod wheel. A knob moves the whole
selected track, song notes too, like pitch bend. -->

<!-- TODO: Program change buttons switch the selected track's voice (program
N = voicegroup slot N); a program past the voicegroup's last voice is
ignored, and the status bar says so once. -->

<!-- TODO: While recording, once you move a control the song's own events
for that lane on the track stop playing (so the song doesn't fight your
knob) until you stop, the loop wraps or you seek. Switching tracks hands the
old track's lanes back to the song. A "reset all controllers" message (some
keyboards send it from a panic button) resets every routed lane. -->

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
Pitch bend and mod-wheel moves show the same way in the BEND and MOD lanes:
the stretch the take rewrites is drawn faded (a BEND or MOD row appears for
the take if the track didn't have one). When the take ends they become real
notes and points. -->

<!-- TODO: What lands in the song: one undo step per take ("Record N
notes") — one Ctrl+Z removes the whole take. Notes are placed on the
nearest tick (no quantizing — the Note Tools example plugin's Quantize
can tidy them afterwards, see plugins.md). Pitch bend and the mod wheel
are recorded too, onto the track's BEND and MOD lanes (MOD as heard: the
wheel scaled to the lane's range) — they replace the lane's points from
your first move to where the take ends (Stop, or a loop wrap), and there
the lane goes back to the value the song had, so a wheel left up doesn't
carry on through the rest of the song. Knobs routed to VOL, PAN, BENDR or LFOS
(Settings → MIDI → Controls) record onto those lanes the same way, and program
changes onto the Voice row (the song's voice comes back where the take ends). The sustain pedal lengthens recorded notes. Takes are added on top
of what's there (overdub); where a new note overlaps an old one on the same
pitch, the new one wins, the same as drawing a note over another. -->

<!-- TODO: Loop recording: with Loop on, keep playing pass after pass —
every pass goes into the same take; a note held across the loop end stops
at the loop end. -->

<!-- TODO: The status bar says what's being recorded into and how many
notes were recorded. Clicking another track mid-take keeps recording (and
sounding) into the original track. Moving the track keeps it following;
deleting it stops the recording and drops the take. -->

## Metronome and count-in

<!-- TODO: The Metronome button on the transport toolbar (or C in the piano
roll) clicks every beat while the song plays, recording or not; the first
beat of each bar is higher. It follows the song's tempo and time signature
(6/8 clicks eighth notes, 4/4 quarter notes), including changes partway
through. On/off is remembered. Volume: Settings → Audio → Metronome (scaled
by Output level too). The click is only for you: it's never in exported
WAVs, and plugins don't hear it. With Loop on it keeps the song's beat
grid: when playback wraps back to the loop start, the loop start only
clicks if it falls on a beat. -->

<!-- TODO: Count-in: pressing Record while stopped clicks 1 bar (default)
before the song starts at the edit cursor — even with the metronome off.
Settings → Audio → Count-in when recording: Off / 1 bar / 2 bars. Not for
Play or for punching in while playing. In punch mode the same setting is
the pre-roll instead (see Punch recording below). It counts at the tempo and time
signature at the edit cursor, lined up so the beat carries straight into
the song (the cursor needn't be on a bar line). With Loop on and the edit
cursor at or past the loop end, the count-in leads into the loop start and
the song starts there. Notes played during the count-in are heard; a key
still sounding when the song starts is recorded from where it starts — held
down, or released while the sustain pedal holds it — and one that has
stopped sounding by then isn't. Keys already held when you press Record are
cut when playback starts and aren't carried in. Pitch bend, knob and voice
(program change) moves during the count-in are heard but not recorded.
Record, Stop or Pause (and the control surface's Play/Pause) during the
count-in cancels it and records nothing. -->

## Punch recording

<!-- TODO: What it's for: re-record just a few bars of a track, replacing
what's there. -->

<!-- TODO: Outline:
- The Punch toggle on the transport toolbar (beside Record), the
  `transport.punch` shortcut (no key by default), or the keyboard's Punch
  button (see Transport buttons below). Remembered. Shown with Record,
  only while a keyboard is connected.
- Sweep a time selection on the ruler, then pick the track (switching
  tracks keeps a time selection made on the ruler or the roll; one made
  on a track's automation lanes is still cleared). With Punch on, the
  ruler tints the selection red: that's the punch range.
- Record needs a time selection while Punch is on (greyed out, tooltip says
  why).
- Record from stopped: plays from the pre-roll with the song audible —
  the Count-in setting's bars (Settings → Audio → Count-in when recording:
  Off / 1 bar / 2 bars, default 1 bar) before the range, in the meter at
  punch-in, never before the song's start; Off starts at the range.
  No count-in clicks; the metronome clicks if it's on.
- Only what you play inside the range is kept (keys held across its edges
  are clipped to it). Knobs, bend, mod wheel, voice: only moves inside it.
- During the take the track's old notes in the range are silent and drawn
  dimmed; a note sounding into the range stops there.
- Keys you hold on the keyboard across punch-in keep sounding; only the
  old part's note is cut there.
- Playback keeps going after the range until Stop; the take lands at Stop
  (one undo step). It replaces the track's notes in the range (a note
  sounding into it is shortened); playing nothing clears the range.
  Stopping inside the range replaces only up to there.
- Record while playing arms it: recording starts when the playhead reaches
  the range. Loop: each pass through the range records, later wins.
- Record while playing inside the range: replaces from where recording
  started, not from the range's start.
- Record from Pause: resumes in place, no pre-roll.
- With Loop on, if the pre-roll start or the range lies past the loop end,
  playback never reaches the range ("Nothing was recorded.").
- Changing the selection or the Punch toggle mid-take doesn't change the
  take in progress. -->

## Recording offset (if notes land late)

<!-- TODO: Why: you hear the song a moment after porydaw plays it (audio
buffering + your sound hardware), so you play slightly late. Settings → MIDI
→ Recording offset moves recorded notes earlier. Auto (shows "Auto (N ms)")
covers porydaw's own buffer only; Windows/drivers/Bluetooth add more that
porydaw can't measure. If notes land late, set a fixed value — try 30–60 ms
on a typical PC (the developer's machine needed about 50 ms); Bluetooth
headphones need much more. Find it by recording steady quarter notes
against the beat. -->

## Transport buttons

<!-- TODO: Many keyboards (e.g. Arturia KeyLab) have Play, Stop, Record,
Loop and Rewind/Fast-forward buttons. They're sent on a second "DAW" port
using Mackie Control (MCU). Settings → MIDI gives each port a role,
Keyboard or Control surface; ports named DAW/MCU/Mackie, and a KeyLab's
"MIDIIN2 (...)" port, start as Control surface. The keyboard must be in its
Mackie/MCU DAW mode. A control-surface port never plays notes, records or
drives Learn. -->

<!-- TODO: What each button does: Play/Pause plays from the edit cursor,
pauses, resumes. Stop stops; Stop again goes to the start of the song.
Record = the Record button (needs a Keyboard port enabled). Loop toggles
Loop. Click toggles the metronome (see Metronome and count-in above). Punch
toggles Punch (see Punch recording above; one press, though the KeyLab
sends two notes for it). Rewind/Fast-forward move one bar line per press (from mid-bar,
Rewind goes to that bar's start); hold to repeat, speeding up to two bars
a step; they stop at the song's start/end. While playing, playback jumps
from where it is; during a take, the take carries on from the new bar.
Jog wheel: one beat per click. Save and Undo buttons save the song and
undo (Undo does nothing while you're typing in a text box). -->

<!-- TODO: The buttons work while porydaw is in the background, but not
while a dialog (e.g. Song Settings) is open. The button lights on the
keyboard don't light up (Click's and Punch's included). Mixer controls (faders, pan knobs, track buttons)
aren't supported. Plugins see a control surface's messages too
(`porydaw.midi.ports()` reports each port's role). -->

## Things to know

<!-- TODO: Keys held when you press Play/Stop/seek go silent (the engine
resets sound on every transport change) — press them again. -->

<!-- TODO: Recording a pitch the track itself is playing at the same moment
can cut one of them short: the GBA engine can't tell the two apart. -->

<!-- TODO: Editing the song while recording is allowed, but each edit
silences the keys you're holding (press them again) and ends their
recorded notes at the edit; a tempo edit mid-take shifts where the take
lands. -->

<!-- TODO: Aftertouch and other keyboard messages don't do
anything in porydaw itself — plugins can still see them (link plugins.md;
the MIDI Monitor example shows everything your keyboard sends). -->
