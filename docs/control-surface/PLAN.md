# Control Surface — keyboard transport buttons via Mackie Control

Status: **steps 0–3 done 2026-10-14** (plan confirmed 2026-10-13: D1, D2, D4, D5 and J1–J7 approved by the user; D3 LEDs dropped). Branch unpushed, awaiting the merge decision.
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

### Step 1 — port roles + surface ring (2026-10-13, uncommitted)

**What landed**

- **`MidiInput::PortRole { Keyboard, Surface }`** with `portRole(name)` /
  `setPortRole(name, role)` and the pure `defaultRole(name)`. The user's
  choices persist in `midi/surfacePorts` and `midi/keyboardPorts`; both
  hold *explicit* choices (choosing a port's default role pins it too), so
  a name default never comes back to a port the user turned around, and a
  later change to the default rules can't flip a choice. An identity in
  both lists (a hand-edited file) reads as Surface, the safe side. A real
  change emits `settingsChanged` and `portsChanged`; re-choosing the same
  role signals nothing.
- **Name default (D1):** `DAW`, `MCU` or `Mackie` as a word of its own,
  any case, where a "word" means no *letter* on either side (digits and
  punctuation are boundaries): "KeyLab Essential 61 DAW", "LKMK3_DAW",
  "X-Touch MCU1" are surfaces; "Dawson Keys", "MCUxyz", "MackieControl"
  aren't. Plus Windows' KeyLab second port: starts with `MIDIIN2 (` and
  contains `KeyLab` (both any case). `MIDIIN3 (KeyLab…)` and
  `MIDIIN2 (<anything else>)` stay keyboards.
- **`OpenPort::surface`**, set in `openQuietly` before the port opens and
  passed by `onMessage` to `receive()`. `setPortRole` on an open port runs
  `closeQuietly` + `openQuietly` (so `portsClosed` releases anything it held
  as a keyboard, and the old callback is gone before the new role exists).
- **`receive(…, surface)`**: a Surface port's message is parsed with
  `parseTap` (velocity-0 note on stays 0x9n), stamped with the port token
  and pushed to **`m_surface`** (`MidiSurfaceRing`, `MidiRing<MidiTapMsg,
  256>`), with one queued hop in flight (`m_surfacePosted`,
  `deliverSurface` → **`surfaceReady()`**, same exchange-before-emit as the
  tap). It skips Learn, `parse`→input ring, `receivedCount` and the
  activity hop (so the roll's keyboard poll never starts for it). The
  script tap still sees it, with its token (J1). `popSurface`,
  `surfaceDropped`.
- **The injectors honour the named port's role** (`portRole(port)`, open
  or not); an unnamed injection is a keyboard.
- **`openKeyboardPorts()`**: `openPorts()` filtered to Keyboard role (test
  ports from `setOpenPortsForTest` included).
- **Settings → MIDI:** each port row has a role box (Keyboard / Control
  surface) on its right, a persistent editor of `PortDelegate`, which now
  paints the check and name short of it (the name elides there) and makes
  rows as tall as the box. The box commits through the model
  (`kPortRoleRole` → `itemChanged` → `setPortRole`). Like the CC boxes, the
  wheel changes it only with focus. Under the list: "Control surface ports
  drive the transport (Mackie Control) and never play notes."
- Nothing drains the surface ring yet (step 2 connects `surfaceReady`); until
  then a surface's messages fill it and are dropped and counted, harmlessly.

**Call-site audit (`openPorts()`)**

| Site | Decision |
|---|---|
| `mainwindow.cpp` `midiControlsOf` (`controls.keyboard`, lane-menu Learn) | → `openKeyboardPorts`: a surface is never learned from. |
| `mainwindow.cpp` portsChanged → cancel a lane learn | → `openKeyboardPorts`: the last *keyboard* leaving stops it. |
| `mainwindow.cpp` `recordUnavailableReason` | → `openKeyboardPorts` ("Connect a MIDI keyboard to record."), so `toggleRecord` refuses with a surface alone. |
| `mainwindow.cpp` `syncRecordAction` visibility | → `openKeyboardPorts` (J4). |
| `reccheck.cpp` no-keyboard SKIP guard | → `openKeyboardPorts`: what Record depends on. |
| `scripthost.cpp` `midiPorts()` (`porydaw.midi.ports()`) | **kept: all ports.** It reports each port's open/enabled state; a surface is open. (No `role` field added; see open items.) |
| `midisettingspage.cpp` "Listening to N port(s)" | **kept: all open ports.** A surface port is listened to; its row shows its role. |
| `midicheck.cpp` `checkService` plumbing | kept: port consistency, not keyboards. |

**Judgment calls**

- **Explicit choices are stored even when they equal the default** (both
  lists are "the user said so"), rather than storing only overrides.
- **Word boundary = letters only** (see above), so vendor spellings with
  digits or underscores still match.
- **An unchecked port's role box stays enabled:** marking a DAW port a
  control surface *before* enabling it means it never plays a note.
- **The surface ring has no enable gate** (unlike the tap): the window will
  always listen from step 2.
- **Role-change reopen goes through `closeQuietly`/`openQuietly`**, not
  `refresh()`, so only that port reopens. `portsClosed` fires for it.
- **Port-list resize fix:** `QListView::Adjust` only relays a top-to-bottom
  list out when its *height* changes, so a width change left the role boxes
  where the old width put them (seen in the harness: box past the right
  edge). The page now relays the rows out on a viewport width change.

**Verification**

- `--midicheck` additions: `checkPortRoles` (default table both ways;
  fresh-start defaults with nothing written; Keyboard override on a DAW
  port persists, signals once, sticks across a fresh `MidiInput`; re-set is
  silent; lists move; default pinning; both-lists reads Surface; surface
  injection → nothing in the input ring or `receivedCount`, one async
  `surfaceReady` hop for three messages, messages as sent with the right
  token, tap sees them, no activity; burst = one hop, re-armed after drain;
  overflow counted; keyboard port beside it unaffected; Learn stays armed
  through a surface CC and a keyboard CC answers it; role flip makes the
  same port play; `openKeyboardPorts` + role change; real-port reopen when
  a port is open, else SKIP), the Settings page (role boxes show defaults,
  sit at the row's right end and follow resizes, choice applies + persists
  + survives the rebuild, unchecked port settable and stays disabled, the
  wheel needs focus, the note line), and `MainWindow::checkSurfacePorts`
  (Record + lane Learn shown for a keyboard and not for a surface alone,
  `toggleRecord` refuses and the reason names the keyboard, a lane learn
  cancelled when only a surface remains, role flips both ways, and a
  surface's note 94 neither held by live play nor lit on the roll while the
  keyboard's is).
- Mutation-tested, each FAILed then restored by hand: regex lookahead
  dropped; KeyLab rule dropped; both-lists precedence flipped; no save; no
  same-role early return; no `settingsChanged`; no `portsChanged`; no
  default pinning; surface to the input ring; no surface push; hop per
  message; synchronous hop; token 0; surface learns; surface posts activity;
  injector ignores the role; `openKeyboardPorts` = all; no persistent
  editors; page ignores the role; unguarded wheel; note line hidden; no
  width relayout; and each of the four `mainwindow.cpp` sites reverted to
  `openPorts()`.
- Not covered in WSL: a real port's close-and-reopen on a role change (the
  check runs when a port is open; Windows checks it by hand below).
- `tools/run_checks.sh` normal and ASAN: PASS. clang-format 22 clean.

**Windows manual check (user)**

1. Settings → MIDI with the KeyLab plugged in: `MIDIIN2 (KeyLab Essential
   61)` shows **Control surface**, the main port **Keyboard**.
2. Select a track, press the KeyLab's **Play**: no A#6 sounds and the roll's
   keyboard doesn't light (no transport action yet; that's step 2).
3. Set MIDIIN2 to **Keyboard**: Play now sounds A#6. Set it back to Control
   surface: silent again. Restart Porydaw: the choice is kept.
4. Uncheck the main KeyLab port, leaving only MIDIIN2 (Control surface)
   checked: the **Record** button disappears and lane menus offer no Learn.
   Re-check the main port: Record returns.
5. Optional, with `midi-monitor` running: MIDIIN2's button presses still
   show there.

**Open**

- `porydaw.midi.ports()` has no `role` field; a plugin can't tell a
  surface port from a keyboard except by name. Easy to add (with docs) if
  wanted.
- With the window at its minimum width the role box leaves a long port name
  little room (it elides in the middle; the tooltip has it whole).

### Step 2 — MCU decoder + transport dispatch (2026-10-13, uncommitted)

**What landed**

- **`src/audio/mcu.{h,cpp}`** (pure): `struct SurfaceEvent { Kind kind;
  bool pressed; int delta; }` (Kind: Play, Stop, Record, Cycle, Rewind,
  FastForward, Save, Undo, Jog) and `std::optional<SurfaceEvent>
  mcu::decode(const MidiTapMsg &)`, shaped as §3.2. Channel 1 only. Notes
  0x5B Rewind, 0x5C Fast-forward, 0x5D Stop, 0x5E Play, 0x5F Record, 0x56
  Cycle, 0x50 Save, 0x51 Undo: a note on with velocity > 0 is a press, a
  note off (any velocity) or a velocity-0 note on is a release. CC 0x3C is
  the jog wheel (bit 6 = backward, low 6 bits = detents; `delta` signed,
  a zero-detent message ignored). Everything else (other channels,
  unmapped notes, other CCs, faders, aftertouch, program change) is
  nothing (J7). The note numbers are named constants (`mcu::kPlay`, …).
- **`MidiTimeline::stepGridLines(tick, steps, bars)`**: the tick |steps|
  bar (or beat) lines away, on the grid `timeSigSegments` describes, i.e.
  the ruler's: lines restart at every meter change (whose tick is always a
  line), never below 0.
- **MainWindow dispatch** (§3.3): `surfaceReady` → `drainSurface` (pops
  everything, decodes, `onSurfaceEvent`). Presses go through the toolbar's
  own actions and only when enabled (J4):
  - **Play** = Pause while playing, else Play (`m_playAction`:
    `startPlayback()` seeks to the edit cursor when stopped and resumes in
    place when paused; checked in the harness).
  - **Stop** = Stop; while stopped (Stop is disabled then) Go to Start.
  - **Record** = `m_recordAction` when enabled (with no keyboard port it
    is hidden *and* disabled, so a surface alone never records).
  - **Loop** = `m_loopAction`; **Save** = `m_saveAction`; **Undo** = the undo
    group's action (now kept as `m_undoAction`).
  - **Rewind / Fast-forward** = one bar line back/forward via
    `surfaceStep` → `SongView::commitEditCursor` (seeks when not stopped;
    a Boundary in a take), then `ensureTickVisible` unless playing.
    Hold-to-repeat on one `QTimer` (`m_surfaceRepeat`): first repeat at
    400 ms, then every 150 ms, two bars per step from the 5th repeat
    (400 + 4 × 150 = 1 s held). Ended by that button's release, any
    `portsClosed` (unplug, disable, Refresh, a role change's reopen) and
    `activateSession` (song switch/close). No other safeguards (user).
  - **Jog** = `delta` beat lines.
  - Under a modal dialog (`QApplication::activeModalWidget()`) presses do
    nothing and a held repeat skips its steps, but a release still ends
    the hold (J5). The window's focus/activation is never consulted, so
    the buttons work with Porydaw in the background.
- **Settings → MIDI:** under the step-1 note, a second read-only line:
  "Control surface buttons: Play/Pause, Stop, Record, Loop, Rewind,
  Fast-forward, the jog wheel, Save and Undo." (`settingsMidiSurfaceButtons`,
  same style as the note above it; the page layout is otherwise untouched.)

**Judgment calls**

- **Rewind/FF/jog step from where playback is**: the edit cursor while
  stopped, the playhead while playing or paused. While playing, the edit
  cursor sits where playback started, so stepping from it would make FF
  jump *back* after a few bars. The playhead read is the view's
  (`SongView::playheadTick`), which shows a seek's target at once, so a
  held repeat chains from its own last seek rather than the engine's
  not-yet-landed position. The edit cursor still moves (via
  `commitEditCursor`), so the seek and the take's Boundary are the
  existing path.
- **Steps land on grid lines, not ± a bar's length**: FF goes to the next
  bar line, Rewind to the previous one (so from mid-bar, Rewind goes to the
  bar's start, as in most DAWs; on a line it is exactly one bar). This
  matches the ruler, including songs that restate their meter mid-bar
  (`mus_abandoned_ship` has a 4/4 at tick 36, so its bar lines are 0, 36,
  132, …).
- **The fast phase counts repeats, not wall time** (5th repeat on = held
  ≥ 1 s at the nominal timing), so the harness can fire the timer by hand.
- **A repeat under a modal dialog waits** (skips its step but keeps the
  hold) rather than ending it; the release ends it as always.
- **Only the held button's release ends a hold**: Rewind's release doesn't
  stop Fast-forward's repeat. A new Rewind/FF press replaces the hold.
- **Not scrolling while playing**: a step while playing leaves the camera
  to Follow Playhead (or to the user, with it off).
- **Save in the harness** is counted with `m_saveInProgress` set, so
  `saveSession` returns before writing anything.

**Verification**

- `--midicheck`: `checkMcuDecoder` (every mapped note: press at 127 and 1,
  release as note off at 0 and 64 and as velocity-0 note on; the same notes
  on channels 2 and 16 and in aftertouch/CC/channel pressure ignored; ten
  unmapped notes; jog ±1, ±5, ±63, zero detents both ways, other channels
  and neighbouring CCs, a fader and a program change ignored),
  `checkGridSteps` (a synthetic 4/4 → 3/4 → 6/8 timeline: forward and back
  bars and beats across both changes, partial bar before a change, clamp at
  0, zero steps, no meter = 4/4), the Settings line (text, visible, under
  the note), and `MainWindow::checkSurfaceTransport` through
  `injectForTest` on a Surface-role port: Play from stopped (from the edit
  cursor), Play's release inert, Pause while playing, resume from paused at
  the paused sample, a velocity-0 release; Stop while paused and playing
  (cursor kept), Stop when stopped → cursor 0; Loop toggles on press only;
  Record refused with only the surface open, a take with a keyboard, Stop
  ends it; Save triggers Save Song once enabled, not when disabled; Undo
  undoes; Rewind/FF stopped (bar lines, mid-bar, clamp at 0, the cursor
  scrolled into view); the repeat (400 ms armed, 150 ms after the first,
  one bar ×4 then two bars, another button's release ignored, its own
  release stops it and a late tick doesn't move, Rewind held to 0 and a
  velocity-0 release); a port closing and a song switch ending a hold; jog
  ±1, +3/−5, −63 clamped; while playing FF from the playhead (not the
  cursor), a hand-fired repeat chaining from the seek's target, the engine
  landing there, Rewind back; while paused FF from the paused playhead; a
  modal dialog: a repeat waits, the release still ends the hold, Play /
  Loop / FF / jog do nothing, Play works once it closes; non-MCU traffic
  inert; no surface ring drops.
- `--reccheck`: a surface Fast-forward during a take moves the cursor to
  the next bar line past the playhead, the engine seeks there, the take
  counts one new segment, and a note held across it ends exactly where the
  seek happened (one undo step, undo restores).
- Mutation-tested (the agent was stopped before this; done by hand
  2026-10-14, each restored from a saved copy): `--midicheck` caught the
  channel filter dropped, a velocity-0 note on read as a press, the jog
  sign flipped, Stop-when-stopped not going to start, Play never pausing,
  the modal-dialog ignore dropped, a release not ending the hold, no fast
  repeat phase, and stepping from the edit cursor while playing.
  `--reccheck` caught Fast-forward not seeking in a take; it did NOT catch
  stepping from the edit cursor in its take (the playhead hasn't passed a
  bar line there, so both give the same bar), which `--midicheck` covers.
- `tools/run_checks.sh` normal and ASAN: PASS (2026-10-14). clang-format 22
  clean.

**Windows manual check (user; covers steps 1 and 2)**

Setup: KeyLab plugged in (in its Mackie/MCU DAW mode), a song open.

1. Settings → MIDI: `MIDIIN2 (KeyLab Essential 61)` shows **Control
   surface**, the main port **Keyboard**. Under the port list: the
   control-surface note and the line "Control surface buttons: Play/Pause,
   Stop, Record, Loop, Rewind, Fast-forward, the jog wheel, Save and Undo."
2. Select a track and press **Play/Pause**: the song plays from the edit
   cursor and no A#6 sounds (the roll's keyboard doesn't light). Press it
   again: pauses. Again: resumes from where it paused.
3. **Stop** while playing: stops, the playhead returns to the edit cursor.
   **Stop** again: the edit cursor goes to the start of the song.
4. **Seek right** (Fast-forward) while stopped: the edit cursor moves one
   bar per press, landing on bar lines, and stops at the song's end.
   **Seek left**: one bar back; from mid-bar it goes to that bar's start;
   it stops at the song start.
5. **Hold** Seek right: one bar, then after a short pause it repeats, and
   after about a second it speeds up (two bars a step). Let go: it stops at
   once. Same holding Seek left, which stops at the start.
6. Seek right/left **while playing**: playback jumps a bar from where it
   is (not from where it started); holding it keeps jumping. Holding (or
   quickly double-pressing) Seek left keeps going back a bar each step; it
   never replays the same bar's start over and over. While paused:
   the paused position moves a bar.
7. **Loop**: the toolbar's Loop button toggles with each press.
8. **Record**: a take starts (Record lit), play a few notes on the keys,
   **Stop** ends it and the notes land. Seek right during a take: the take
   carries on from the new bar, the notes before the jump are kept.
9. **Background**: click another application so Porydaw isn't focused (keep
   its window visible). Play/Pause, Stop, Seek and Loop still work.
10. Open a modal dialog (Edit → Song Settings…): the buttons do nothing
    while it is open; close it and they work again. (The Settings window
    isn't modal: the buttons keep working while it's open.)
11. Set MIDIIN2 to **Keyboard** in Settings → MIDI: Play now sounds A#6 and
    drives nothing. Set it back to **Control surface**: silent, and the
    buttons work again. Restart Porydaw: the choice is kept.
12. Uncheck the main KeyLab port, leaving only MIDIIN2 checked: the
    **Record** button disappears, lane menus offer no Learn, and the
    KeyLab's Record button does nothing (Play/Stop still work). Re-check the
    main port: Record returns.
13. Optional, with `midi-monitor` running: MIDIIN2's presses still show
    there.

**Open**

- Jog, Save and Undo are harness-tested only (the KeyLab sends none of
  them on MIDIIN2).
- ~~`porydaw.midi.ports()` has no `role` field~~: added 2026-10-14 (user asked), see below.

### `porydaw.midi.ports()` role (2026-10-14)

User asked for it after step 1. Each port now carries `role`:
`"keyboard"` or `"surface"`. A role change counts as a change, so
`on("ports")` fires. Documented in `docs/scripting/API.md` (and the
generated `docsrc/reference/scripting.md`). `--scriptcheck` checks the
field and that `on("ports")` fires both ways. Mutation tested: a role that
always reads keyboard FAILs. Sweeps normal and ASAN: PASS.

### Review of steps 1–2 + port role (2026-10-14)

`/code-review high 5ff6be0..HEAD`: 8 findings, no crash or data loss.

**Fixed** (each mutation-tested in `--midicheck`):

- **Rewind while playing landed on the same bar line again and again**
  (the playhead moves past the line a step lands on). Within 1 s of a
  step, Rewind now steps back from where that step landed
  (`m_surfaceLastTo`, per session).
- **Fast-forward and jog had no end**: forward steps stop at the song's
  `lengthTicks`, and a cursor already past it stays put.
- **A role change closed and reopened the port.** A failed reopen left it
  dead until Refresh. `OpenPort::surface` is now an atomic the callback
  reads per message, so the role flips in place. An open port still reports
  `portsClosed`, so keys or a surface button it held are let go.
- **Arrow keys in a focused role box lost focus** after one press (the
  rebuild deleted the box). `rebuildPorts` now updates the same port list
  in place.
- **Undo acted on the document under a focused text field**: it's skipped
  while a `QLineEdit` or spin box has focus, as Ctrl+Z would be.

**Skipped**:

- jog two's-complement decoding (non-standard MCU; the user doesn't
  support non-standard devices);
- a surface message parsed twice when the script tap is on (negligible).

Sweeps normal and ASAN: PASS.

### Windows check of steps 1–2 (user, 2026-10-14)

Everything passed except the port checkboxes, which couldn't be clicked: a
row with an open persistent editor (its role box) hands the view's clicks to
that editor, so the delegate never saw them. Fixed in 565d72b (the page's
viewport filter toggles a click on the checkbox; midicheck sends a real
click). User re-check: PASS.

### Step 3 — docs (2026-10-14)

CHANGELOG line under MIDI keyboard support; a "Transport buttons" TODO
outline in `docsrc/manual/midi-keyboard.md` (user writes the prose); the
Settings → MIDI stub mentions port roles. `mkdocs --strict`: PASS.

**Final branch review** (Opus, `main...HEAD`): no high or medium findings.
Three low ones fixed, each with a midicheck case that catches its revert:

- Surface Undo under a text field while porydaw is in the background: the
  app has no focus widget then. It now checks each visible window's own
  focus widget.
- Rewind's chain undid a seek made some other way (FF to bar 5, a ruler
  click at bar 40, Rewind landed on bar 4). It chains only while the
  playhead is within a bar of the last step; a tab switch also clears it.
- A press still queued when its port closed (or flipped role) started a
  hold whose release could never come. `portsClosed` drains the surface
  ring before ending the hold.

Not changed: a note-on racing a Surface→Keyboard flip can leave that one
note held (milliseconds wide; All Notes Off clears it).

Sweeps normal and ASAN: PASS.
