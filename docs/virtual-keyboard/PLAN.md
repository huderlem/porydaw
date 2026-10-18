# Virtual MIDI Keyboard — play and record without a MIDI device

Status: plan agreed with the user 2026-10-18; step 1 built 2026-10-18 (uncommitted, §4.1).
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
