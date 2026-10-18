# Virtual MIDI Keyboard — play and record without a MIDI device

Status: plan agreed with the user 2026-10-18; no steps started.
Work the steps **in order, one Opus subagent per step**; each step must
leave the branch green (`tools/run_checks.sh`, normal + ASAN) and ends
with an "As built" entry at the bottom of this file. Workflow per step:
Opus implements → Opus reviews → Opus fixes worthwhile findings → commit
(no Claude attribution) → user Windows check (after steps 2 and 3).

## 0. Problem and goal

Porydaw records from MIDI keyboards (docs/midi-input/PLAN.md, overdub and
docs/punch/PLAN.md punch-in). Users without a MIDI device can only audition
one note at a time by clicking the roll's key column and cannot record.

**Goal:** a virtual MIDI keyboard modelled on Reaper's: a floating window,
always above Porydaw's other windows, playable with the mouse and with the
computer keyboard while it has focus. It feeds the same input path as a
hardware keyboard, so live play, overdub recording, punch-in, the sustain
latch, ghost notes, fixed velocity and the `porydaw.midi` tap all work
unchanged.

## 1. Decisions (all confirmed by the user 2026-10-18)

| # | Question | Decision |
|---|---|---|
| D1 | Window | A floating **tool window** owned by the main window (`Qt::Tool`, parent = MainWindow): above Porydaw's windows whether or not it is active, never above other applications, hidden with the app. View → Virtual MIDI Keyboard toggles it; keymap command `view.virtualKeyboard`, default `Alt+B` (Reaper's) if free. Position and visibility persist in QSettings and are restored at startup. |
| D2 | Span and layout | Two octaves from a base C, Reaper's layout. Bottom row = lower octave: whites `z x c v b n m ,` = C D E F G A B C, blacks `s d g h j` = C# D# F# G# A#. Top row = upper octave: whites `q w e r t y u i o` = C D E F G A B C D, blacks `2 3 5 6 7 9 0` = C# D# F# G# A# C# D#. Comma and `q` both play the upper C, as in Reaper. The drawn piano runs from the base C to the D# 27 semitones above it (16 white keys, 12 black). Each key shows its letter; each C shows its octave number (porydaw naming: key 60 = C4, `m4asemantics.cpp:141`). |
| D3 | Key matching | By **physical scan code** (`QKeyEvent::nativeScanCode` / `nativeVirtualKey` fallback, mapped from the US-layout positions), so AZERTY and QWERTZ get the same physical rows. Auto-repeat is ignored. Chords work; a key stays sounding until released. |
| D4 | Focus rule | **Only with focus.** While the window is active its keys play notes and nothing else sees them (verified: keymap bindings are QAction shortcuts on the main window with the default WindowShortcut context, `keymap.cpp:715`; step 3 asserts none is application-wide). While not active it receives no key events. Losing activation (window deactivated, app deactivated, window hidden) releases every held key. |
| D5 | Octave range | MIDI keys are 0–127 (C-1 to G9). The base C moves by octave with `-` / `=` and arrow buttons in the window, from **C-1 (key 0) to C8 (key 108)**. At base C8 the keys above 127 (G#9 and up) are drawn disabled and play nothing; every other base is fully in range (C7 + 27 = 123 ≤ 127). Default base **C3 (key 48)**, so the two octaves bracket middle C. Persisted. |
| D6 | Velocity | A slider in the window (1–127, default 100, persisted) sets the velocity of typed and clicked notes. Settings → MIDI *Fixed velocity* still overrides, as for any port. |
| D7 | Mouse play | Press a key to sound it, drag across keys to glide (release the previous, sound the next), release anywhere to stop. One mouse note at a time; typed keys are independent. |
| D8 | What it feeds | A built-in port **"Virtual Keyboard"** in `MidiInput`: role Keyboard, channel 1, cannot be disabled or given the Surface role, not listed as hardware but shown on Settings → MIDI as present while open. Its messages take the same path as a hardware port's callback (Learn, parse, ring push, tap, activity). It is **open while the window is visible**, so Record (`mainwindow.cpp:441`) and lane Learn (`:983`) appear. |
| D9 | Visual feedback | Keys light when played by typing, the mouse, **or any hardware Keyboard-role port**, so the window doubles as a note monitor. The selected track's name and voice show in the window. Cheap: the window observes note on/off on the UI thread from the existing activity/tap hop, never the audio thread. |
| D10 | Scripting | Nothing new. Plugins see virtual notes via `porydaw.midi` with port `"Virtual Keyboard"`. |
| D11 | Docs | Manual page stub `docsrc/manual/virtual-keyboard.md` (TODO outline, user writes prose), mkdocs nav entry, cross-link from `midi-keyboard.md`, CHANGELOG line. |
| D12 | Performance | The window must not cost CPU while idle: no timers while hidden or with nothing held, repaint only the keys whose state changed (`update(rect)`), cached key geometry, no per-frame allocation. Step 2 measures idle and held-chord CPU in the harness notes. |

## 2. Design stances

1. **One more producer, nothing new on the audio thread.** The window
   calls a UI-thread `MidiInput::sendVirtual(const MidiMsg&)` (the
   non-test twin of `injectForTest`, port = "Virtual Keyboard",
   `arrivalNs = midiClockNs()`). Latency compensation (output latency
   offset) applies as for hardware, because you still hear the song late.
2. **The port is a state, not a device.** `openKeyboardPorts()` includes
   "Virtual Keyboard" while `setVirtualOpen(true)`; `ports()` does not list
   it among hardware; Settings → MIDI shows a one-line note. Role setters
   and enable toggles refuse the name.
3. **Lit keys come from the UI side.** A small `noteStateChanged(key, on)`
   signal on MidiInput is emitted on the UI thread when the tap/activity
   hop drains (hardware) or when the window itself sends. No audio-thread
   state is read.
4. **The window owns no audio state.** Held-key bookkeeping (typed set,
   mouse key) lives in the widget; every path that can lose a release
   (deactivate, hide, octave shift while held, scan-code unknown) sends
   note-offs for everything held, then clears.

## 3. Steps

1. **Virtual port in MidiInput.** `sendVirtual`, `setVirtualOpen`,
   `virtualOpen`, the "Virtual Keyboard" identity in `openKeyboardPorts`,
   tap port token, refusal in enable/role setters, `noteStateChanged`
   hop. Record and Learn gating follow with no change. Settings → MIDI
   note line. `--midicheck` coverage: open → Record visible; message
   reaches the ring and tap with the right port; close → Record hidden;
   held notes on close are released; role/enable refusal. Mutation-test
   every new assertion.
2. **The window.** `src/ui/virtualkeyboardwindow.{h,cpp}`: piano widget
   (D2 drawing, D5 range and disabled keys, D6 slider, octave buttons,
   track/voice label, D9 lighting), mouse play (D7), View menu action +
   keymap command (D1), QSettings persistence, Qt::Tool stacking. New
   offscreen harness `--vkeycheck <scratch> <song>` driving clicks and
   glides and asserting ring contents, lit keys, range clamps, and that
   hiding releases held notes. D12 measurements in the As-built entry.
3. **Typing.** Scan-code table (D3), key press/release → note on/off,
   auto-repeat ignored, `-`/`=` octave shift releasing held keys first,
   deactivation release (D4), assertion that no keymap action is
   application-scoped. `--vkeycheck` extended: chord, repeat, shift while
   held, deactivate, out-of-range keys at C8.
4. **Docs + final review.** D11, then a final Opus review of the whole
   branch and fixes.

## 4. As built
