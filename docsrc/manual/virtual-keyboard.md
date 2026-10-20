# The Virtual MIDI Keyboard

<!-- For people with no MIDI keyboard: play and record with the mouse and the
computer keyboard. Modelled on Reaper's virtual MIDI keyboard. Everything on
[Playing & Recording with a MIDI Keyboard](midi-keyboard.md) applies; this
page covers only what's different. -->

## Opening the window

<!-- TODO: View → Virtual MIDI Keyboard, or Alt+B (rebindable: Keyboard
Shortcuts, "Virtual MIDI Keyboard"); Alt+B again (or the window's close
button) puts it away. It floats above Porydaw's windows (not above other
programs) and hides when Porydaw is minimised. Porydaw remembers where it
was, its size, and whether it was open, and reopens it at startup. While
it's open the Record button appears, as it does for a plugged-in keyboard,
and Settings → MIDI says the Virtual Keyboard is open (with no MIDI system
at all, e.g. WSL, it still plays and records, and Fixed velocity, Recording
offset and Controls show there). -->

<!-- TODO: What the window shows: octave arrows and the lowest key's note
name, the Velocity slider, the track it plays into ("N · name — voice",
the voice at the edit cursor; the take's track while recording), then the
piano: two octaves and a bit (C up to the D# 27 semitones above), each key
labelled with the computer key that plays it and each C with its octave
(C4 = middle C, key 60). -->

## Playing with the mouse

<!-- TODO: Click a key to play it; it sounds until you let go. Drag across
the keys to glide (the old note stops, the new one starts); dragging off
the piano keeps the last note going, and letting go anywhere stops it. One
mouse note at a time. The Velocity slider (1–127, default 100, remembered)
sets how hard clicked and typed notes are struck; Settings → MIDI → Fixed
velocity overrides it, as it does for any keyboard. -->

## Playing with the computer keyboard

<!-- TODO: Click the window (or open it with Alt+B) and type: the keys play
only while the window is the active window. Two rows, two octaves, laid out
like a piano (the same as Reaper's). Hold several for a chord; holding a key
doesn't repeat it. -->

<!-- TODO: Prose around the table below (the user may redraw it as a
picture of the keyboard). -->

| Row | Black keys | White keys | Plays |
|---|---|---|---|
| Upper (number row and Q row) | `2 3` `5 6 7` `9 0` | `Q W E R T Y U I O` | C D E F G A B C D, one octave up |
| Lower (A row and Z row) | `S D` `G H J` | `Z X C V B N M ,` | C D E F G A B C, from the octave shown |

<!-- TODO: Notes for the table: comma (,) and Q both play the same upper C.
The black keys sit between their white keys as on a piano (S = C#, D = D#,
G = F#, H = G#, J = A#; 2 = C#, 3 = D#, 5 = F#, 6 = G#, 7 = A#, 9 = C#,
0 = D#). -->

<!-- TODO: It goes by where the key is, not by its letter: on AZERTY,
QWERTZ and other layouts the same physical rows play (on a French keyboard
the key labelled W plays C). Shift+key plays the plain note. -->

<!-- TODO: Which keys still reach Porydaw from the window: Space (play /
stop), Alt+B (closes the window), and any Ctrl / Alt chord (Ctrl+Z, Ctrl+S,
menu keys like Alt+V). The note keys never run their own shortcuts from
here: the piano roll's R (Record), C (Metronome), M, S and so on are notes
while the window is active, so use the transport buttons, or click the
main window first. -->

## Changing octave

<!-- TODO: The octave arrows, or - and = on the keyboard. The name between
the arrows is the lowest key (default C3, so the two octaves sit around
middle C; remembered). Range C-1 to C8. At C8 the keys above G9 (the top of
MIDI) are greyed out and play nothing. Changing octave lets go of every
note you're holding; press the keys again to play in the new octave. -->

## Recording

<!-- TODO: Record and punch recording work exactly as with a hardware
keyboard (link midi-keyboard.md#recording and #punch-recording): select the
track, press Record on the transport toolbar. Fixed velocity and the
Recording offset (Settings → MIDI) apply to it too. Closing the window
mid-take doesn't stop the take; Stop does. -->

## Lit keys

<!-- TODO: Keys light while they sound: the ones you click or type, and the
ones you play on a connected MIDI keyboard too, so the window doubles as a
note monitor for a hardware keyboard. -->

## When held notes stop

<!-- TODO: Everything you're holding is let go (and stays silent until you
press it again) when the window stops being the active one (clicking
Porydaw's main window, Alt+Tab), when a dialog opens over it (Settings, a
save prompt), when a menu opens (Alt+V and friends), when you change
octave, and when the window closes or Porydaw is minimised. Play, Stop and
seeking silence held notes too, as for any keyboard (link
midi-keyboard.md#things-to-know). -->

## Things to know

<!-- TODO: Plugins: messages from the window reach `porydaw.midi`'s
on("message") with port "Virtual Keyboard", but `porydaw.midi.ports()`
lists only hardware ports, so the Virtual Keyboard never appears there
(link reference/scripting.md). -->

<!-- TODO: With only the Virtual Keyboard open, a lane's right-click
"Learn MIDI control…" and Settings → MIDI → Controls' Learn still appear,
but the window has no knobs: Learn can only time out (10 s) or be
cancelled. -->

<!-- TODO: Linux: on an X server without evdev keycodes (XQuartz, the old
"kbd" driver) typing does nothing; the mouse still plays. -->

<!-- TODO: macOS: holding a note key, pressing a Cmd shortcut and letting
the note key go first can leave the note sounding (macOS may not report
that key's release); press the key again to stop it. -->
