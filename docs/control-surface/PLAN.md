# Control Surface — keyboard transport buttons via Mackie Control

Status: **plan confirmed 2026-10-13** (D1, D2, D4, D5 and J1–J7 approved by the user; D3 LEDs dropped).
Branch `control-surface` off `main` at `5ff6be0`. As on `midi-controls`:
steps run **in order, one agent per step**; each step leaves the branch
green (`tools/run_checks.sh`, normal + ASAN) and ends with a progress entry
at the bottom of this file; an Opus review follows each code step. Windows
manual checkpoints are marked (WSL has no MIDI device, so harnesses use
`MidiInput::injectForTest`).

## 0. Problem and goal

Many keyboards expose a **second USB MIDI port** for their DAW buttons. The
user's Arturia KeyLab Essential 61 does: the keys, wheels and knobs arrive
on the main port, and the transport section (rewind, fast-forward, stop,
play, record, loop) plus Save/Undo-style buttons and the jog wheel arrive on
`MIDIIN2 (KeyLab Essential 61)` (Windows' generic name for a device's
second port; macOS/Linux usually call it "… DAW").

That port speaks **Mackie Control (MCU)**, which encodes buttons as Note
On/Off on channel 1 at fixed note numbers (velocity 127 press, 0 release)
and the jog wheel as a relative CC 60. Porydaw treats every open port as a
keyboard, so today:

- the buttons do nothing useful;
- worse, with MIDIIN2 enabled, **Play sounds A#6** (note 94) through live
  play, can be recorded into a take, and can be grabbed by Learn.

**Goal:** a port can be marked as a **control surface**. Its messages never
reach live play, recording or Learn; instead the standard MCU transport
buttons and jog wheel drive Porydaw's transport, exactly like the toolbar
buttons.

**Out of scope:**

- **HUI.** Older and messier (CC zone/port pairs). Users set the KeyLab to
  MCU in its DAW settings / MIDI Control Center.
- **Mixer section:** MCU faders (pitch bend per channel), V-pots, fader
  touch, track select/mute/solo/arm buttons, bank/channel paging (D4).
- **Button LEDs** (D3, dropped by the user: needs MIDI output, port
  pairing and hot-plug handling for little gain), and therefore any MIDI
  output at all.
- **MCU displays** (LCD/timecode sysex) and the Mackie handshake/ping.
- **Remapping buttons** to other commands (the MCU map is fixed; see D5).
- **Arturia's proprietary DAW-integration sysex** (the "Arturia" DAW
  scripts for Ableton/Bitwig etc.).

## 1. Decisions (confirmed by the user 2026-10-13)

| # | Question | Proposal |
|---|---|---|
| D1 | How a port becomes a control surface | **A per-port role in Settings → MIDI's port list: Keyboard / Control surface (Mackie)**, persisted as QSettings `midi/surfacePorts` (identity list, like `midi/disabledPorts`). **Default by name:** a port whose identity contains `DAW`, `MCU` or `Mackie`, or is a KeyLab's second port (`MIDIIN2 (…KeyLab…)`), starts as Control surface; everything else as Keyboard. The user's choice always wins. Alternative: no name guessing, every port defaults to Keyboard. |
| D2 | What the buttons do | **Play/Pause** (the KeyLab labels it so) = the toolbar Play when stopped or paused (from the edit cursor when stopped, resumes when paused), Pause while playing. **Stop** = Stop; Stop while already stopped = Go to Start (common DAW convention). **Record** = the toolbar Record button. **Loop/Cycle** = Toggle Loop. **Rewind / Fast-forward** = move the edit cursor one bar back/forward per press, repeating while held (accelerating after ~1 s); while playing this seeks, as dragging the edit cursor does now. **Jog wheel** = one beat per detent (standard MCU; the KeyLab's wheel sends nothing on MIDIIN2, so this is untested on the user's device). **Save** = Save Song, **Undo** = Undo (standard MCU; the KeyLab has no such buttons on MIDIIN2, untested). Anything else is ignored. |
| D3 | Button LEDs | ❌ **Dropped** (user, 2026-10-13): not worth the complexity of the first MIDI output path. |
| D4 | Mixer section (faders, V-pots, track buttons) | **Out of scope for now.** Your KeyLab's faders/knobs already work as plain CCs on the main port, so routing them through MCU adds little. Could be a follow-up. |
| D5 | Fixed MCU map vs. learnable buttons | **Fixed MCU map**, shown read-only in Settings → MIDI ("Control surface buttons: Play, Stop, …"). MIDI-learn for arbitrary commands is a bigger feature (it'd touch the keymap registry) and isn't needed for an MCU device. |

## 2. Judgment calls (approved by the user 2026-10-13)

| # | Call | Proposal |
|---|---|---|
| J1 | What a surface port's messages reach | **Not** the live input ring, recording, Learn or the activity indicator's "notes" path. They **do** still reach the `porydaw.midi` script tap (with their port token), so plugins can see and extend them. |
| J2 | Thread hop | The MIDI threads push surface messages into a separate small ring, drained on the UI thread through one queued hop (the script tap's pattern). The decoder and all actions run on the UI thread; a hop of a few ms is fine for buttons. |
| J3 | Press vs release | Actions fire on press (Note On, velocity > 0). Release only ends Rewind/FF repeat. Repeated presses without releases (a stuck surface) are harmless. |
| J4 | Enabled state | Buttons go through the same `QAction::trigger()` as the toolbar, so they obey its enabled state (nothing loaded → nothing happens). Record also obeys its rules (e.g. hidden with no keyboard port open: a surface-only port doesn't count as a keyboard). |
| J5 | Focus and dialogs | Buttons work while Porydaw is in the background (the point of a hardware transport). They're ignored while a modal dialog is open (`QApplication::activeModalWidget()`), as keyboard shortcuts are. |
| J6 | Jog wheel encoding | MCU relative: bit 6 = direction (0 clockwise/forward), low 6 bits = detent count, so fast spins move several beats per message. |
| J7 | Non-MCU traffic on a surface port | Ignored (still visible to the tap). No warning. |

## 3. Design

### 3.1 Port role (MidiInput)

- `MidiInput::portRole(name)` / `setPortRole(name, role)`; `enum class
  PortRole { Keyboard, Surface }`. Persisted in `midi/surfacePorts`
  (explicit Surface) plus `midi/keyboardPorts` (explicit Keyboard
  overriding a name-default), so D1's guess never sticks to a port the
  user turned back.
- `OpenPort` carries the role, set before the port opens, read by
  `receive()` on the MIDI thread. Changing a role closes and reopens the
  port (the existing reopen path), so no role flips under a live callback.
- In `receive()`, a Surface port's bytes skip Learn, `parse`→input ring
  and the held-key path; they go to `m_surface` (`MidiRing<MidiTapMsg,
  256>`) and the tap. A new `surfaceReady()` signal (one hop in flight,
  as `tapReady`) and `popSurface()`.
- "Keyboard connected" (Record visibility, lane-menu Learn, `controls.keyboard`)
  counts only Keyboard-role open ports. New `openKeyboardPorts()`; audit
  every `openPorts().isEmpty()` call site (mainwindow 342, 840, 5656, 5683;
  reccheck 1150; scripthost 1008 keeps all ports).

### 3.2 MCU decoder (pure, testable)

- `src/audio/mcu.{h,cpp}`: `struct SurfaceEvent { enum Kind { Play, Stop,
  Record, Cycle, Rewind, FastForward, Save, Undo, Jog }; bool pressed; int
  delta; }` and `std::optional<SurfaceEvent> mcu::decode(const MidiTapMsg&)`.
- Map (standard MCU, channel 1 only): 0x5B Rewind, 0x5C Fast-forward,
  0x5D Stop, 0x5E Play, 0x5F Record, 0x56 Cycle, 0x50 Save, 0x51 Undo;
  CC 0x3C Jog. Step 0 confirmed the six transport notes on the KeyLab
  (exact MCU numbers); Save, Undo and Jog are kept from the standard map
  but only harness-tested.

### 3.3 Dispatch (MainWindow)

- `MainWindow::onSurfaceEvent` maps events to the existing actions
  (`m_playAction`, `m_stopAction`/`m_goToStartAction`, `m_recordAction`,
  `m_loopAction`, `m_saveAction`, the undo group's action). Rewind/FF/Jog
  move `SongView::commitEditCursor(tick ± bar/beat)`, clamped to ≥ 0,
  which already seeks playback when not stopped (and is a Boundary in a
  take). Bar/beat lengths from the song's time signature/ticks per beat
  as the ruler uses them.
- One `QTimer` for Rewind/FF repeat: first repeat after 400 ms, then every
  150 ms, two bars per step after 1 s held. Released or port closed → stop.

### 3.4 Settings → MIDI

- Each port row gets a role combo (Keyboard / Control surface). A short
  line under the list explains: "Control surface ports drive the
  transport (Mackie Control) and never play notes."
- A read-only line listing the supported buttons (D5).

## 4. Steps

0. **Capture (user, Windows, no code).** With MIDIIN2 enabled and the
   `midi-monitor` example plugin running, press each DAW-section button
   (and turn the jog wheel both ways, slow and fast) and report what it
   shows. Confirms MCU (not HUI) and the note numbers. Also note the
   KeyLab's DAW-mode setting if it offers MCU/HUI.
1. **Port roles + surface ring.** §3.1 and the Settings combo (§3.4,
   without the button list). `--midicheck`: role persistence and
   name-defaults (incl. user override sticking), surface-port notes never
   reaching the input ring / Learn / recording, still reaching the tap,
   role change reopening the port, `openKeyboardPorts` driving Record
   visibility. Mutation-tested.
2. **MCU decoder + transport dispatch.** §3.2, §3.3, the button list
   line. Harness: decoder table unit checks; inject on a Surface port and
   assert transport state, edit cursor moves (bar/beat, clamping, repeat
   timer driven by hand), Stop-when-stopped → start, modal-dialog ignore.
   **Windows checkpoint:** every button and the jog wheel by hand.
3. **Docs.** CHANGELOG (simple), a TODO-outline section in
   `docsrc/manual/midi-keyboard.md` (user writes prose), settings.md stub;
   `mkdocs --strict`. Final Opus branch review.

## 5. Risks

- **Not every "MCU" keyboard is plain MCU.** Some (newer Arturia, Novation)
  only enter DAW mode after a vendor handshake, or need "Mackie" picked on
  the device. Step 0 tells us what the KeyLab needs; others are best effort.
- **Name-based defaults (D1) can misfire:** e.g. a multi-port interface's
  second DIN input named `MIDIIN2 (…)`. That's why the KeyLab rule is
  narrow and the user's choice is persisted.
- **Seeking during a take** via Rewind/FF/Jog takes the existing Boundary
  path; reccheck already covers seeks, but step 2 adds one via the surface.

## Progress

### Step 0 — capture (user, Windows, 2026-10-13)

`midi-monitor` on `MIDIIN2 (KeyLab Essential 61)`, Note On velocity 127
on press (names in Porydaw's C4 = 60 convention):

| Button | Note | MCU |
|---|---|---|
| Loop | D6 = 86 (0x56) | Cycle |
| Seek left | G6 = 91 (0x5B) | Rewind |
| Seek right | G#6 = 92 (0x5C) | Fast forward |
| Stop | A6 = 93 (0x5D) | Stop |
| Play/Pause | A#6 = 94 (0x5E) | Play |
| Record | B6 = 95 (0x5F) | Record |

Exactly standard MCU, so the device is in Mackie mode. The jog wheel sent
nothing on MIDIIN2. Releases (velocity-0 Note On / Note Off) weren't
reported at first; the user confirmed every button sends a Note Off on
release, so Rewind/FF hold-to-repeat (J3) works as planned.
