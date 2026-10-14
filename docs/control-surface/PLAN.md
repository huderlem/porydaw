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
