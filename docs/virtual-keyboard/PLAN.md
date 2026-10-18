# Virtual MIDI Keyboard — play and record without a MIDI device

Status: plan agreed with the user 2026-10-18; step 1 built 2026-10-18 (§4.1);
step 2 built 2026-10-18 (uncommitted, §4.2).
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
| D1 | Window | A floating **tool window** owned by the main window (`Qt::Tool`, parent = MainWindow): above Porydaw's windows whether or not it is active, never above other applications, hidden with the app. View → Virtual MIDI Keyboard toggles it; keymap command `view.virtual_keyboard` (snake_case, like every keymap id), default `Alt+B` (Reaper's) if free. Position and visibility persist in QSettings and are restored at startup. |
| D2 | Span and layout | Two octaves from a base C, Reaper's layout. Bottom row = lower octave: whites `z x c v b n m ,` = C D E F G A B C, blacks `s d g h j` = C# D# F# G# A#. Top row = upper octave: whites `q w e r t y u i o` = C D E F G A B C D, blacks `2 3 5 6 7 9 0` = C# D# F# G# A# C# D#. Comma and `q` both play the upper C, as in Reaper. The drawn piano runs from the base C to the D# 27 semitones above it (16 white keys, 12 black). Each key shows its letter; each C shows its octave number (porydaw naming: key 60 = C4, `m4asemantics.cpp:141`). |
| D3 | Key matching | By **physical scan code** (`QKeyEvent::nativeScanCode` / `nativeVirtualKey` fallback, mapped from the US-layout positions), so AZERTY and QWERTZ get the same physical rows. Auto-repeat is ignored. Chords work; a key stays sounding until released. |
| D4 | Focus rule | **Only with focus.** While the window is active its keys play notes and the main window's commands do not see them. This does **not** come for free: the keymap bindings are WindowShortcut QActions on the main window (`keymap.cpp:715`), and a parent QMainWindow's WindowShortcut actions *do* fire from an active `Qt::Tool` child (verified 2026-10-18 review; the earlier "nothing else sees them" claim was wrong). So step 3 accepts `QEvent::ShortcutOverride` for every mapped scan code (D2's rows) and for `-` / `=`, which turns those keys into plain key presses for the window, so a typed note never also runs, say, a tool shortcut. Every other key passes through to the main window's shortcuts, deliberately: **Space** (play/stop) is wanted from inside the window, and so are Alt+B (closes it again), Ctrl+Z and the like. Modified chords (Ctrl/Alt/Meta + a mapped key) are not notes and pass through too. While not active it receives no key events. Losing activation (window deactivated, app deactivated, window hidden) releases every held key. |
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
   `QEvent::ShortcutOverride` accepted for the mapped keys and `-`/`=`
   only (D4: Space, Alt+B and modified chords still reach the main
   window), deactivation release (D4). **Per-key holder counting**: a note
   can be held by a typed key and by the mouse at once (C4 typed and C4
   clicked; `,` and `q` both play the upper C), so the window counts
   holders per key and sends the note on for the first holder and the note
   off only when the last one lets go. **Release everything**, the mouse
   key included, on window deactivation, `QEvent::ActivationChange`, a
   modal dialog opening over it, and hide. `--vkeycheck` extended: chord,
   repeat, shift while held, deactivate, out-of-range keys at C8, a typed
   and a clicked C4 overlapping (one note on, one note off at the last
   release), `,` + `q` together, a mapped key not firing the main window's
   shortcut for it while Space still does.
4. **Docs + final review.** D11, then a final Opus review of the whole
   branch and fixes. Also a docs/UX item (§4.1 hand-off vi): with only the
   virtual port open, a lane menu's Learn and Settings → MIDI Controls
   appear but can only time out or do nothing (the window sends no CCs);
   decide with the user whether to hide them for a virtual-only setup or
   just document it.

## 4. As built

### 4.1 Step 1 (2026-10-18, uncommitted)

**Built.** The Virtual Keyboard port in `MidiInput`, its Settings note, and
`--midicheck` coverage. No change at the Record (`mainwindow.cpp:441`,
`:6199`) or Learn (`:983`) gating sites: they follow `openKeyboardPorts()`
through `portsChanged`, as planned.

- `src/audio/midiinput.h`: `virtualPortName()` (:347), `setVirtualOpen` /
  `virtualOpen` (:354), `sendVirtual` (:361), `setNoteMonitorEnabled` /
  `noteMonitorEnabled`, signal `noteStateChanged(int key, bool on, bool
  fromVirtual)` (the third argument added by the review fixes below; the
  original `MidiMonitorRing` was replaced there by a 128-bit down set).
- `src/audio/midiinput.cpp`: the token is assigned in the constructor
  before any hardware port, so it is always 1 (:163); `openPorts` appends it
  last while open, `isPortOpen` true for it, `ports()` never lists it;
  `isPortEnabled` always true and `portRole` always Keyboard for the name
  (even against an edited settings file); `setPortEnabled` / `setPortRole`
  return at once for it (no persist, no signal). `setVirtualOpen` (:841)
  releases held keys through `sendVirtualNow` *before* flipping closed, then
  emits `portsChanged` (real changes only). `sendVirtualNow` (:865) forces
  channel 1, keeps the 128-bit held set (`m_virtualHeld`, from the parsed,
  normalised message, so a velocity-0 note on releases), calls `receive`
  with `arrivalNs = midiClockNs()`, the port token and `monitor=false`, then
  emits `noteStateChanged` synchronously. The note monitor update sits in
  `receive`'s live-play branch, so a surface's notes (swallowed earlier)
  and Learn-swallowed CCs never reach it; `deliverMonitor` reports it on
  one queued hop at a time, as `deliverTap` does.
- `src/ui/midisettingspage.{h,cpp}`: `settingsMidiVirtualNote` label at the
  top of the page's outer layout (:303), shown in `syncBackend` (:639) while
  the port is open. The "Listening to N port(s)" status leaves it out of N.
- `src/midicheck.cpp`: `checkVirtualPort()` (:711, standalone service) and
  `MainWindow::checkVirtualWindow()` (:1813; declared in `mainwindow.h` next
  to `checkSurfacePorts`), plus a file-comment paragraph.

**Judgment calls the plan didn't settle.**

1. *Hardware lit keys come from a new note monitor, not the tap or the
   activity hop.* The activity hop carries no data and the tap is on only
   while a plugin listens (and is drained by ScriptHost), so a separate
   small ring + `setNoteMonitorEnabled` (analogous to `setTapEnabled`) is
   used; off, a MIDI thread pays one atomic load. Step 2's window turns it
   on while visible and off when hidden. No timer anywhere.
2. *The window's own notes signal synchronously* from `sendVirtual` and
   bypass the monitor (no double signal, no hop latency), whether or not the
   monitor is enabled.
3. *Channel forced to 1* for every channel-voice message `sendVirtual` gets
   (D8 says the port is channel 1; it also makes the per-key held set's
   release on close always match).
4. *Closing emits no `portsClosed`.* It would call `liveAllNotesOff` and
   cut a hardware keyboard's held notes too; the explicit per-key note-offs
   release only what the virtual port held, down the same path (live play,
   a take, the tap and `noteStateChanged` all see them).
5. *`closeAll` leaves the virtual port alone*: it is used only at teardown,
   and the window owns the port's state.
6. *Settings note placement*: above both the controls and the no-backend
   view, since the Virtual Keyboard plays without a MIDI backend. Note for
   step 2/4: without a backend the page still hides Playing and recording
   (Fixed velocity, Recording offset) and Controls, which a Virtual-only
   user on such a machine can't reach. Not changed here (out of scope).
7. *Not handled*: a hardware device literally named "Virtual Keyboard"
   would collide with the built-in identity (the refusals and forced role
   apply to it). Judged too unlikely to special-case.
8. For step 2: lit hardware keys should be cleared on `portsClosed` (a
   closing port never sends its note-offs). (The ring-overflow concern
   noted here originally is gone: see the review fixes.)

**Mutation tests** (each applied alone, rebuilt, `--midicheck` run on a
fresh scratch; all 25 FAILED as they should, then restored):
M1 `sendVirtual` ignores closed; M2 `openPorts` omits it; M3 `arrivalNs` 0;
M4 channel not forced; M5 tap token 0; M6 no synchronous
`noteStateChanged`; M7 monitor never pushes; M8 virtual notes also go
through the monitor; M9 monitor ignores its enable; M10 monitor takes CCs;
M11 no release on close (also caught in the window by live play still
holding the key); M12 note-off keeps the held bit; M13 `portsChanged` on a
no-op open/close; M14 `setPortEnabled` not refused; M15 `setPortRole` not
refused; M16 `portRole` not forced; M17 `isPortEnabled` not forced; M18
`isPortOpen` omits it; M19 close emits `portsClosed`; M20/M21 Settings note
always / never shown; M22 velocity-0 note on counted as on; M23 monitor
on/off inverted; M24 `ports()` lists it; M25 monitor not emptied on
disable (first run NOT caught; a check for a note pending at disable was
added, then caught).

**Review fixes** (Opus review of step 1, 2026-10-18, uncommitted).

1. *Source on the signal*: `noteStateChanged(int key, bool on, bool
   fromVirtual)`. True from `sendVirtual` and the release in
   `setVirtualOpen(false)`, false from the note monitor, so step 2 can keep
   mouse/typed keys apart from hardware keys (a hardware release of C4
   must not unlight a mouse-held C4).
2. *Monitor drained before `portsClosed`*: every `emit portsClosed()`
   (`setPortRole` flip, `closeQuietly`, `closeAll`, `rescan`) is preceded by
   `flushMonitorForClose()`: `deliverMonitor()` synchronously, then a reset
   to every key up. The contract is on the signal's comment: everything
   before it has been reported, and a receiver may clear all hardware-lit
   keys there and stay in step (a key still held on another open port
   lights again on its next press). Not emitted for the virtual port, as
   before.
3. *Lossless monitor*: the 1024-entry ring is gone. Two
   `std::atomic<uint64_t>` words (`m_monitorDown`, keys 0–63 / 64–127) are
   set and cleared with `fetch_or` / `fetch_and` on the MIDI thread, which
   posts a hop only when a bit really changed (and the posted flag is
   clear). `deliverMonitor` snapshots both words, diffs them against
   `m_monitorReported` and emits the changed keys in ascending order; it
   stops if a receiver turns the monitor off mid-delivery. A press and
   release between two hops reports nothing. Enabling or disabling resets
   both words and the reported state (`resetMonitor`), so a re-enable
   starts clean.
4. *Settings note*: inset with the controls' page-layout margins (left,
   top, right; the view below gives the gap), and its text follows the
   backend: "…plays like a keyboard port." with one, "The Virtual Keyboard
   is open: it plays and records without a MIDI backend." without, so it
   reads with the "no MIDI" message rather than against it. Fixed velocity
   / Recording offset stay hidden without a backend (hand-off iii).
5. *Harness gap*: a second hardware batch after the first delivered one
   must arrive (catches a `deliverMonitor` that never clears the posted
   flag).
6. *Nits*: `m_virtualHeld = {}` in `setVirtualOpen(false)` removed — each
   released key's note off clears its own bit in `sendVirtualNow`, so the
   set already ends empty. `sendVirtual` `Q_ASSERT`s the UI thread and
   clamps both data bytes to 127 (never wrapped), so a note on keeps a
   1–127 velocity and a velocity-0 note on stays an off; contract in the
   header.

New `--midicheck` assertions: fromVirtual on every virtual and monitor
signal; a press/release between hops reports nothing; a second batch (and
an upper-word key) arrives; a 3000-message burst ends exactly right; a
re-enabled monitor lights a key that was down at disable and one that was
reported down; a hardware note on injected just before a role flip is
reported before `portsClosed`, and the same key lights again on its next
press afterwards; `sendVirtual` clamps key and velocity 200/255 → 127; the
Settings note is inset and its wording follows the backend.

**Hand-offs to step 2.**

i. `MainWindow` destroys `m_midiInput` (a `unique_ptr` member) before its
   Qt children, so the window must not call `MidiInput` from `hideEvent` or
   its destructor at teardown: delete or hide it first in `~MainWindow`, or
   hold the service through a `QPointer`. ASAN must cover quitting with the
   window open.
ii. `setVirtualOpen(false)` emits `noteStateChanged` synchronously, so a
   call from the window's `hideEvent` re-enters the window: its slot must
   tolerate that.
iii. Without a backend the Settings page hides Fixed velocity, Recording
   offset and Controls, yet the virtual keyboard records without a backend:
   show them when `virtualOpen()` too. Must land before step 4's docs.
iv. `porydaw.midi.ports()` does not list "Virtual Keyboard", though tap
   messages carry that port name: document it in step 4.
v. A hardware port literally named "Virtual Keyboard" collides with the
   built-in identity (deferred, unlikely).
vi. UX note for the user: a lane menu's Learn appears with only the virtual
   keyboard open, and can then only time out (the window sends no CCs).

### 4.2 Step 2 (2026-10-18, uncommitted)

**Built.** The window, its View action and shortcut, the Settings page
hand-off, and `--vkeycheck`.

- `src/ui/virtualkeyboardwindow.{h,cpp}`: `VirtualPianoWidget` (h:25) and
  `VirtualKeyboardWindow` (h:109). The window is `QWidget(parent, Qt::Tool)`
  owned by MainWindow, titled "Virtual MIDI Keyboard": octave ◀ / name /
  ▶ (`QToolButton` arrows, disabled at C-1 and C8), a Velocity slider 1–127
  with its value, the track-and-voice line, then the piano. `showEvent` →
  `openPort` (cpp:502: `setVirtualOpen(true)`, then
  `setNoteMonitorEnabled(true)`); `hideEvent` → geometry saved, `closePort`
  (cpp:513: mouse key released, port and monitor off, both lit sets
  cleared). `closeEvent` emits `closeRequested` (the action unchecks).
  `noteState` (cpp:543) splits `noteStateChanged` by `fromVirtual` into the
  piano's `virtualHeld` / `hardwareDown` bitsets; `portsClosed` is wired to
  `clearHardwareDown` only. Settings: `virtualKeyboard/base` (rounded down to
  a C, clamped 0–108), `virtualKeyboard/velocity`, `virtualKeyboard/geometry`
  (written by the window), `virtualKeyboard/visible` (written by MainWindow's
  action).
- Piano (D2/D5/D7/D12): 16 whites share the width less half a black key, so
  the top D# is drawn whole at the right edge; blacks are 0.6 × a white,
  0.62 of the height. Rects (`layoutKeys`, cpp:188) are recomputed on resize
  and font change only; labels (`relabel`) on a base change. Letters are
  uppercase keycaps, "`, Q`" on the upper C, and each C shows its octave
  (`midiKeyName`). Keys above 127 are filled with the window background
  (whites) / separator (blacks), labelled in `disabled_text`, and are inert.
  Colours are the roll keyboard's roles (`song_view_piano_keyboard_*`), black
  key labels in the natural-key colour. `paintEvent` (cpp:236) draws only the
  keys meeting the dirty rect, from cached rects/strings; `repaintIfChanged`
  (cpp:167) calls `update(keyRect)` only when a key's lit state changes. No
  timers, no mouse tracking.
- `src/mainwindow.cpp`: the window and View → Virtual MIDI &Keyboard
  (`viewVirtualKeyboardAction`, :1352, after the Script Console/Plugin
  Panels group); `updateVirtualKeyboardTarget` (:5761) called from
  `publishLiveTarget` (:5758: tab switch, selection, rebuild) and the
  `editCursorMoved` connect (:1740); `restoreVirtualKeyboard` (:5787) called
  from `main.cpp` (:323) after `showCoveredWhileRestoring`, so harnesses
  never restore it. Hand-off i: `~MainWindow` deletes the window first
  (:682) under a `QSignalBlocker` on MidiInput; the window's destructor runs
  `detach()` (`~QWidget` never reaches our `hideEvent`), and it also holds
  MidiInput through a `QPointer`.
- `src/ui/keymap.cpp:89`: `view.virtual_keyboard`, Global, default
  **Alt+B** (free: no command, and no `&B` menu-bar mnemonic).
- Hand-off iii, `src/ui/midisettingspage.cpp:646`: with the Virtual Keyboard
  open and no backend, the controls scroll shows (Playing and recording,
  Controls) under the "no MIDI" message, the Input ports group
  (`settingsMidiPortsGroup`) stays hidden, and the message stops stretching.
  `--midicheck` `checkVirtualWindow` asserts it (:1909) and that it goes
  again on close.
- `src/vkeycheck.cpp` (`MainWindow::runVirtualKeyboardCheck`), wired in
  `CMakeLists.txt`, `src/main.cpp` (:251) and `tools/run_checks.sh` after
  midicheck. Audio device parked so the ring keeps the window's notes. It
  ends with the window shown and a mouse key held, so MainWindow's teardown
  runs with it open (ASAN sweep).

**Judgment calls.**

1. *Command id `view.virtual_keyboard`*, not `view.virtualKeyboard`: every
   other keymap id is snake_case, and the ids are persisted.
2. *Dragging off the piano (or onto a gap or a faded key) keeps the last note
   sounding*; only another playable key glides, and the release stops it
   wherever it lands. A drag that overshoots the edge doesn't stutter, and
   "release outside sends the off" holds.
3. *A second press with a mouse key still held* (a lost release) releases
   it first: one mouse note at a time.
4. *An octave shift with the mouse holding a key* releases it and does not
   sound the key now under the pointer.
5. *Track line* is "N · name — 042 voice (type)", the header's title and
   the voice in effect **at the edit cursor** (`SongView::instrumentLabelAt`
   with `editCursorTick()`, one pass over the voice changes), for the
   take's track while recording, else the selected one. Not the
   playhead's during playback (that would mean per-frame work; D12), and
   no longer the header's `instrumentLabel`, which uses `displayTick()`
   (the playhead while playing) and so showed the playhead's voice
   whenever a selection or cursor move refreshed the line mid-playback,
   then kept it after the stop (review fix 4). "No song open" without a
   tab.
6. *The controls take no focus* (`Qt::NoFocus` on the buttons and slider),
   so key events stay with the window for step 3. Key events are not
   handled yet (QWidget default: ignored).
7. *Visibility persists on the action's toggle*, not on hide, so teardown
   and a minimised main window never record "hidden". The close button
   unchecks the action only for a spontaneous close while the main window
   shows (review fix 1: a normal quit did persist "hidden"). Still worth a
   look on the Windows/macOS pass: macOS Cmd+Q.
8. *Spontaneous hides* (the owner minimised) close the port and release
   notes like any hide; the action stays checked and the next show reopens.
9. *Showing from the View action activates the window* (`raise` +
   `activateWindow`), ready for typing in step 3; the startup restore shows
   it without activating (`Qt::WA_ShowWithoutActivating`, review fix 5), so
   the main window keeps the keys.
10. Alt+B is a main-window WindowShortcut, and those **do** fire while the
    keyboard window (a `Qt::Tool` child) is active, so Alt+B already
    closes it from inside (corrected in review: this said the opposite).
    Step 3 keeps it working by not accepting ShortcutOverride for it (D4).

**D12 measurements** (normal build, WSL, offscreen, null audio device
running, `PORYDAW_VKEY_CPU=1 --vkeycheck`, process CPU via `clock()` over
10 s of idle event loop): window hidden 0.043 s, shown idle 0.040 s, shown
with a held chord (three hardware keys + one mouse key) 0.050 s, i.e. about
0.4–0.5% of one core in every state, all of it the main window and audio
thread; the window adds nothing measurable. The harness asserts the
repaint of a lit key is within that key's rect.

**Mutation tests** (each applied alone, rebuilt, fresh scratch; all CAUGHT
unless noted, then restored): V1 show doesn't open the port; V2 monitor not
enabled; V3 port left open on hide; V4 monitor left on; V5 note on at a
fixed 100; V6 whites hit-tested before blacks; V7 glide doesn't release the
previous key; V8 dragging off the piano releases; V9 release only inside
the widget; V10 a press stacks on a held key; V11/V12 base clamp past
C8/C-1; V13 keys above 127 playable; V14 every note treated as hardware;
V15 `portsClosed` not wired; V16 `portsClosed` clears the mouse keys too;
V17 whole-piano repaint; V18 no mouse release in `closePort` — NOT caught,
equivalent (setVirtualOpen(false) releases the key and noteState forgets
it), V18b the same plus no `forgetMouseKey` — caught; V19 octave shift
keeps the mouse key; V20/V21 velocity/base not persisted; V22 geometry not
saved; V23 no `closeRequested`; V24 visibility not persisted; V25
`restoreVirtualKeyboard` no-op; V26 target not updated from
`publishLiveTarget`; V27 nor from the edit cursor; V28 no Alt+B default; V29
`Qt::Window` instead of `Qt::Tool`; V30 hide leaves the monitor on and the
keys lit; V31 hide doesn't clear hardware keys; V32 Settings page ignores
the virtual port (midicheck); V33 Input ports group shown without a backend
(midicheck); V34 `setBase` doesn't round to a C. Under ASAN: V35 no early delete in `~MainWindow` and a raw `MidiInput *` in the window — heap-use-after-free in `sendVirtual` from the teardown release (caught; either guard alone suffices).

**Left for step 3.** Typing (scan codes, repeat, `-`/`=`, ShortcutOverride
per D4, holder counting, releasing everything including the mouse key on
deactivation / ActivationChange / modal dialogs: §3 step 3); the label does
not refresh when a voicegroup edit renames the voice without a timeline
rebuild.

**Review fixes** (Opus review of step 2, 2026-10-18, uncommitted).

1. *Visibility after a normal quit* (high). Qt 6's last-window-closed quit
   calls `close()` on the still-visible tool window after the main window
   has gone; that non-spontaneous close fired `closeRequested`, the action
   unchecked and `virtualKeyboard/visible=false` was saved, so the window
   never came back. `closeEvent` now emits `closeRequested` only for an
   accepted **spontaneous** close (the title bar's button) **while the
   parent is visible**. `--vkeycheck` drives the title-bar close with a
   `QCloseEvent` sent to the `QWindow` (QWidgetWindow turns that into a
   spontaneous close, as for a window-system close; the reviewer's
   `QWindowSystemInterface::handleCloseEvent` needs Qt's private headers,
   which this machine lacks) and asserts the action unchecks and "hidden"
   is saved; then that a `close()` from code with the main window shown,
   a `close()` after hiding the main window (the quit path), and a
   window-system close after hiding it all keep "shown".
2. *CI geometry* (medium). The harness window is 560×180 at (40, 60), well
   inside offscreen's 800×600 screen, so Qt 6.9's restoreGeometry
   adjustment cannot change the exact pos/size comparison (comment points
   at sessioncheck's `resize(777, 505)`). Not provable locally (Qt 6.2.4
   here does not adjust).
3. *Plan text*: D4 rewritten (WindowShortcut actions do fire from the tool
   window; step 3 accepts ShortcutOverride for the mapped keys and `-`/`=`
   only, Space and Alt+B pass through); judgment call 10 corrected; D1's
   id is `view.virtual_keyboard`; step 3 scope gains holder counting and
   release-everything on deactivation / ActivationChange / modal dialogs;
   step 4 gains the Learn/Controls-with-only-the-virtual-port UX item.
4. *Track line voice* (low): `SongView::instrumentLabelAt(track, tick)`
   (`instrumentLabel` now forwards to it at `displayTick()`), and the line
   uses it at `editCursorTick()`. No refresh on stop is needed: nothing in
   the line depends on the playhead any more. `--vkeycheck` puts the view
   in a playing state past a voice change with the cursor at 0 and asserts
   the line keeps the cursor's voice.
5. *Restore steals focus* (low): `restoreVirtualKeyboard` sets
   `m_restoringVirtualKeyboard` around `setChecked(true)`; the toggled
   handler then sets `Qt::WA_ShowWithoutActivating` and skips
   `activateWindow`. Offscreen activates every window it shows, whatever
   the attribute, so the harness asserts on the path: a counter
   (`m_virtualKeyboardActivations`) does not move on restore and the
   attribute is set; a user toggle activates (counter +1, attribute clear).
6. *Nits*: the constructor rounds the saved base with the same helper as
   `setBase` (`roundBase`; no observable change, since both clamp to ≥ 0);
   the velocity is saved when it settles (on `sliderReleased`, on a change
   while the slider isn't down, and in `hideEvent`), only if it changed,
   not on every drag step; `openPort` clears the window's own lit bits
   first; the piano's minimum height is now enough for a C's two label
   lines to sit below its black key (`minimumPianoHeight`, from the label
   font; `labelRect` shared by painting and the harness). Skipped as
   asked: QStaticText, font-change minimum widths.

New `--vkeycheck` assertions: least-height octave label clears the C#;
playing past a voice change keeps the cursor's voice; setVelocity persists;
a drag saves only on release; a hide mid-drag saves; a stale virtual bit is
cleared by a show; title-bar close unchecks and saves "hidden"; close()
from code, the quit path's close(), and a window-system close with the main
window hidden keep "shown"; restore does not activate, a toggle does.

Review-fix mutations (each applied alone, rebuilt, fresh scratch; all
CAUGHT, then restored): W1 no `spontaneous()` check; W2 no parent-visible
guard; W3 `closeRequested` never emitted; W5 the line back on
`instrumentLabel`; W6 restore activates; W7 a toggle never activates; W9
velocity saved at every drag step; W10 no save on `sliderReleased`; W11 no
save in `hideEvent`; W12 no clear of the window's lit bits on show; W13
the old `fontPx(5)` minimum height. Not mutation-tested: the constructor's
`roundBase` (equivalent to the old rounding after the clamp) and the CI
geometry change (Qt 6.2.4 here never adjusts).

