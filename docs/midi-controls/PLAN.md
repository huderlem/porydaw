# MIDI Controls — route keyboard knobs to every automation lane

Status: **plan confirmed 2026-10-12** (D1–D4 and J1–J7 approved by the user).
Branch `midi-controls` off `main` at `1131fa5`. Work the
steps **in order, one agent per step**. Each step must leave the branch
green (`tools/run_checks.sh`, normal + ASAN) and ends with a progress entry
at the bottom of this file. Windows manual checkpoints follow steps 2, 3
and 5 (the user tests on Windows; WSL has no MIDI device, so harnesses use
the injector as in `docs/midi-input/PLAN.md`).

## 0. Problem and goal

MIDI input (`docs/midi-input/PLAN.md`, bend/mod recording `adf10ed`, lane
ghosts `2de09d8`) hard-codes the keyboard's controls in
`AudioEngine::handleLiveMessage` (`src/audio/audioengine.cpp:749`):

- CC1 sets MOD, scaled to the lane's visible range;
- CC64 is the sustain pedal;
- CC120/121/123 are resets and panic;
- pitch bend sets BEND.

Every other CC is dropped. Plugins still see it raw through `porydaw.midi`.

The other audible lanes in **+ Add lane** (`SongView` `showAddLaneMenu`,
`kAudibleCcs`) are VOL (CC7), PAN (CC10), BENDR (CC20) and LFOS (CC21).
None of them can be played or recorded from a keyboard. Keyboard knobs send
whatever CC their maker chose (often 21–28 or 70–77), so a fixed mapping
can't reach them.

**Goal:** any knob, slider or button on the keyboard can drive MOD, VOL,
PAN, BENDR or LFOS on the target track. Program-change buttons drive the
Voice lane. These controls are heard live and recorded into the lane
exactly as bend and mod are now: the same capture ring, one undo step, lane
ghosts while recording, and the song's prior value restored at the end of
the take.

**Out of scope:**

- **Tempo.** It is global, not per track, and changing it live would move
  the tick↔sample map that the take itself relies on.
- **Advanced or imported-only CCs** (TUNE, MODT, LFODL, PORTAMENTO, …).
- **Relative (endless) encoders.** Every route is absolute; the user's
  keyboard has none (user, 2026-10-12).
- **Per-song or per-track routing.**
- **A min/max range per route** (D2).
- **Soft takeover.**
- **A `porydaw.midi` routes API.**

## 1. Decisions (D1–D4 confirmed by the user 2026-10-12)

| # | Question | Decision |
|---|---|---|
| D1 | Routing model | ✅ **A routing table with MIDI Learn.** Settings → MIDI gets a "Controls" table: one row per lane (MOD, VOL, PAN, BENDR, LFOS), each with a CC number or None, and a Learn button. A lane's right-click menu offers **Learn MIDI control…**, which edits the same table. Routing is global (QSettings `midi/routes`) and omni: the channel is ignored, as for notes. One CC drives one lane. CC64 and CC120–127 can't be assigned. Defaults: CC1 → MOD, CC7 → VOL, CC10 → PAN, the rest None. |
| D2 | Scaling | ✅ **Fixed per lane.** VOL, PAN and LFOS take the full 0–127. BENDR maps to 0–24 semitones. MOD keeps following the lane's visible range (today's `m_liveModMax`). |
| D3 | Song events in a touched lane while recording | ✅ **Latch.** Once a control is touched during a take, the song's own events in that lane on the target track stop playing until the take ends, so what you hear is what gets committed. This applies to BEND and MOD too. |
| D4 | Program change → Voice | ✅ **In scope.** Program-change messages (0xC0) need no routing. They change the target track's voice live and record Voice lane points. |

## 2. Judgment calls (approved by the user 2026-10-12)

| # | Call | Proposal |
|---|---|---|
| J1 | What Learn listens for | The first routable CC to arrive. It ignores CC64, CC120–127 and bank select (CC0/32), and CC38/CC6 (data entry, which RPN bend-range messages use). Learning a CC that already drives another lane **moves** it there; the old row goes to None. Esc or a second click cancels. Learn times out after 10 s. |
| J2 | Latch scope | Latch applies only while a take is armed **and** the control sounds (live play is on). With live play off, nothing of the knob is heard, so the song plays on untouched. The latch clears at a segment boundary (loop wrap or seek), and the lane is chased back to the song. On pass 2, before you touch the knob again, you hear the song's original events, not pass 1's take, because the timeline can't be rebuilt mid-take. This is a known mismatch with what gets committed there. |
| J3 | Program numbers | The data byte is used as the voice slot directly (0–127, no bank select). A program change past the voicegroup's last voice is ignored and shown once in the status bar. |
| J4 | Voice writes at tick 0 | A take that changes voice at tick 0 replaces the tick-0 program change; it never deletes it. The track's keep-alive seed (`appendTrackKeepAliveOps`) still holds. Restore-after-take puts the song's voice back at the segment end, just as it does for other lanes. |
| J5 | CC121 (reset all controllers) | It resets every routed lane to the engine's track-init default (the table in `TimelinePlayer::chase`) and leaves the voice alone. In a take, that reset is recorded as moves, as it is for bend and mod today. |
| J6 | The route table's MOD row | MOD becomes an ordinary row: its default is CC1, and it can be remapped or set to None. Bend stays on pitch-bend messages and can't be routed. |
| J7 | Channel/omni | Routes match the CC number on any channel, consistent with notes (MIDI input D2). |

## 3. Design

### 3.1 Routes (UI thread → audio thread)

- **Model:** a `MidiRoutes` value type in `src/audio/midiroutes.h`. For each
  CC number (0–127) it stores a target lane (or none). Two encodings: a
  compact table for the audio thread, and a list for the Settings page and
  QSettings.
- **Ownership:** `MidiInput` owns persistence, as it already does for the
  `midi/*` keys.
- **Audio-thread handoff:** `AudioEngine::setLiveRoutes` publishes an
  immutable table through an atomic pointer swap. The old table is retired
  on the UI thread one callback later, the same way the ring pointer is
  handled. No locks on the audio thread.
- **Learn** doesn't go through the audio thread. `MidiInput` already parses
  on the MIDI threads, so it gets a "learn" tap: a `std::atomic<bool>`
  armed flag plus a tiny SPSC slot. The Settings page or lane menu polls it
  on a timer, like the held-key poll. While learn is armed, a learned CC is
  **also** swallowed from live play, so turning the knob doesn't change the
  song.

### 3.2 Live play

- **Dispatch:** in `handleLiveMessage`, the 0xB0 branch looks the CC up in
  the route table. Reserved CCs (64, 120–127) keep their fixed handling.
  The two per-lane appliers, `applyLiveBend` and `applyLiveMod`, become
  one `applyLiveControl(cc, value)` for CC lanes; bend keeps its own path.
- **Absolute scaling** is per lane (D2): MOD uses `m_liveModMax`, BENDR is
  `round(v·24/127)`, everything else is `v`.
- **Program change:** a new 0xC0 case calls `m4a_engine_program_change` on
  the target track and marks the voice as touched.
- **Target change:** `m_liveBent` becomes a touched mask (bend bit + CC
  bits + program bit). `TimelinePlayer::chaseBendMod` generalises to
  `chaseTouched(engine, timeline, pos, track, mask)`, which hands back
  exactly the lanes and voice the keyboard touched, so the rest of the
  song plays on undisturbed.

### 3.3 Recording

- **Capture:** `CaptureEvent::Mod` becomes `CaptureEvent::Control`, which
  carries `cc` (in `key`) and `value`. Add `CaptureEvent::Program`. Bend is
  unchanged.
- **Take:** `RecordingTake::Control.cc` already supports any lane. To
  generalise:
  - `audible(cc, v)` is MSB-only for bend and identity elsewhere;
  - write order within a segment becomes ascending CC, then BEND, then
    Voice;
  - the undo text names the lanes ("Record 12 notes with VOL and PAN").
- **Voice writes** go through `writeLanePoints` with `DOC_CC_VOICE`. Its
  header comment says "Not for DOC_CC_VOICE", so step 5 first audits and
  extends it (J4), and adds a reccheck for the voice-name stamps and the
  tick-0 seed.
- **Ghosts and transient rows** already work for any lane
  (`RecordingPreview::lanes`, `mergeEmptyLanes`); only the bend/mod
  assumptions get removed. The Voice row's ghost draws as voice chips, the
  same way the row draws.

### 3.4 Latch (D3)

- **Mask:** `TimelinePlayer::dispatchDue` takes a per-track latch mask
  alongside `muteMask`. For one track it holds a bend bit, 128 CC bits and
  a program bit. `AudioEngine` owns it, and only the target track can be
  latched.
- **Setting a bit:** a bit is set on the first live move of that lane while
  a capture is armed and the target sounds (J2).
- **Clearing:** the whole mask clears, followed by a `chaseTouched` of the
  cleared lanes, at a Boundary (loop wrap or seek), at Cut, at take end and
  at a target change.
- **Unaffected:** offline render and the non-live paths always pass 0, so
  `poryaaaa_render` parity is untouched.

## 4. Steps

1. **Routes + Settings table + Learn.**
   - Code: `MidiRoutes`, persistence, the Settings → MIDI "Controls" table
     (lane, CC spinner/None, Learn), Learn (J1).
   - Engine wiring: MOD reads its CC from the table; no other behaviour
     changes yet.
   - Harness: `--midicheck` covers route persistence, Learn through the
     injector, reserved-CC refusal, and move-on-conflict.
2. **Live play of routed lanes + program change.** Covers `applyLiveControl`,
   per-lane scaling, program change, the touched
   mask, `chaseTouched`, and generalising CC121 (J5). Harness: an
   engine-state check per lane. **Windows checkpoint:** the
   user's knobs move VOL/PAN/LFOS/BENDR and voice by ear.
3. **Record routed CC lanes.** Covers the `Control` capture kind,
   generalising `RecordingTake`, undo text, ghosts and transient rows for
   VOL/PAN/BENDR/LFOS, restore-after-take, and reccheck unit + e2e
   coverage (mutation-tested). **Windows checkpoint.**
4. **Latch.** Covers the `dispatchDue` latch mask, set/clear rules (J2),
   chase on clear, and BEND/MOD joining in. Harness: during a take, a song
   VOL event after the first knob move doesn't reach the engine, and does
   again after the wrap and after stop.
5. **Record program changes → Voice lane.** Covers the `writeLanePoints`
   voice audit (J4), the `Program` capture kind, the out-of-range ignore
   (J3), voice ghosts, restore-after-take, and latch for the voice.
   **Windows checkpoint.**
6. **Lane-menu Learn + docs.** Adds "Learn MIDI control…" to the lane
   context menu (with the current CC shown, e.g. "MIDI: CC74"), a CHANGELOG
   entry (simple), a TODO outline section in
   `docsrc/manual/midi-keyboard.md` (the user writes the prose) and a
   settings.md stub. Ends with the final Opus branch review.

An Opus code review follows steps 2, 3 and 5, with its fixes committed
before the next step, as on `midi-input`.

## 5. Risks

- **Knob message rate.** Knobs stream faster than a mod wheel. The capture
  ring's capacity and the take's same-tick last-wins should absorb it.
  Step 3 adds a flood check: 3 knobs at 1 kHz for 10 s.
- **Voice writes** are the least-trodden path: no drag editing, a keep-alive
  seed, and name stamps. That is why they are a separate step (5), last
  before docs.
- **Latch touches the shared playback path** (`dispatchDue`). It must stay
  zero-cost with an empty mask and must never be set for offline render.

## Progress

### Step 1 — routes, Settings → MIDI Controls table, Learn (2026-10-12, uncommitted)

**What landed**

- `src/audio/midiroutes.{h,cpp}`: `MidiRoutes`, lane → CC per routable
  lane (`kLanes` = 0x01, 0x07, 0x0A, 0x14, 0x15, the Settings order), with
  `laneFor(cc)` as the reverse lookup. `assign(lane, cc, &movedFrom)` moves a
  CC that another lane holds (that lane goes to none). `assignable(cc)`
  refuses CC0/32, CC6/38, CC64 and CC120–127. Two encodings: `packed()`
  (lane i's CC in byte i, 0xFF none) and `toStrings()`
  (`"MOD=1", …, "BENDR=none"`).
- `MidiInput`: `routes()` / `setRoute(lane, cc)` persisted under
  `midi/routes` as that string list. The key is removed when the routes
  equal the defaults, as `midi/disabledPorts` is. `fromStrings` reads
  defensively: unknown names, unassignable CCs and a repeated CC read as
  none, and a lane the list doesn't name keeps its default where that CC is
  free. Route changes emit `settingsChanged()`, and `MainWindow::syncLiveSettings`
  publishes them with `AudioEngine::setLiveRoutes(packed)`.
- Learn lives on `MidiInput`: `startLearn(lane, timeoutMs = 10 s)`,
  `cancelLearn()`, `learnLane()`, and the signals `learnFinished(lane, cc,
  movedFrom)` and `learnCancelled(lane, timedOut)`. `receive()` checks an
  atomic armed flag **before** the live parse. While armed, an assignable CC
  is CASed into an `std::atomic<int>` slot (first wins, -1 = empty) and
  kept out of the input ring. The script tap still sees it. A 15 ms UI
  timer inside `MidiInput` takes the slot and routes it. Starting a second
  lane cancels the first.
- Engine: the 0xB0 `default:` branch looks the CC up in
  `m_liveRouteOf[128]`. Only the `0x01` (MOD) entry acts so far, through the
  unchanged `applyLiveMod`, so capture follows the route. CC64, 120, 121
  and 123 keep their fixed cases, which run before the lookup. CC121 still
  zeroes bend and MOD whatever MOD is routed to (J5 is step 2).
- Settings → MIDI gets a **Controls** group: one row per lane, a CC
  spinner with the special value "None" (-1), and a checkable **Learn**
  button that reads "Listening…" while armed. A status line below says
  what Learn did ("CC 74 now drives VOL instead of PAN."). Object names
  are `settingsMidiRoute<LANE>`, `settingsMidiLearn<LANE>` and
  `settingsMidiLearnStatus`.
- `--midicheck`:
  - `checkRoutesTable` covers the pure value type.
  - `checkRoutesService` covers persistence, reserved refusal,
    move-on-conflict, and Learn through the injector: reserved CCs pass
    through, the first CC wins and is swallowed, a second knob in the same
    burst isn't learned, cancel works, a second lane cancels the first, and
    a 40 ms timeout fires.
  - `checkControlsPage` covers the table, spinner stepping and validation,
    Learn with move, second-click cancel, Esc cancel and hide cancel.
  - `MainWindow::checkLiveRoutes` covers MOD on CC74 driving the engine at
    the lane's scale while CC1 does nothing, MOD set to None, and a capture
    that records CC74's moves (and none from CC1) as `CaptureEvent::Mod`.
  - Mutation-tested 11 ways: the engine ignoring the table, no swallow,
    CC64 assignable, no move, timeout ignored, no spinner skip, no Esc
    filter, no save, no hide-cancel, no second-click cancel, and validate
    accepting a reserved CC. Each failed the expected checks and was
    restored.
- `docsrc/manual/settings.md`: the MIDI TODO stub mentions the Controls
  table.

**Judgment calls**

- **Packed word instead of a pointer swap.** Since one CC drives one lane
  and there are 5 lanes, lane → CC fits in one `uint64_t`.
  `setLiveRoutes` is a single atomic store of that immutable value. The
  audio thread compares the word each callback and, when it changed,
  unpacks it into its own `m_liveRouteOf[128]` table (an O(1) lookup). No
  heap table means no retire and no lifetime hazard. This meets §3.1's
  intent (immutable, lock-free, nothing freed under the audio thread)
  more simply. The packing has room for 8 lanes; if a step ever needs more
  than 8, switch to a pointer swap.
- **Reserved CCs are refused everywhere**, in Learn, the spinner and
  `setRoute`. The list is J1's: 0/32, 6/38, 64 and 120–127.
  - The spinner's range is -1..119, so 120–127 can't be reached.
  - Its arrows step over 0, 6, 32, 38 and 64.
  - A typed reserved number validates as Intermediate, so it never commits.
- **Move-on-conflict applies to the spinner too**, not just Learn. One rule,
  one CC per lane.
- **What Learn swallows.** While armed, *every* assignable CC is swallowed,
  not only the first one learned. Several knobs in the same instant can't
  leak into live play before the UI poll disarms (≤15 ms). Reserved CCs,
  notes and bend pass through, so the sustain pedal and playing still work
  during Learn. Swallowed messages don't count toward `receivedCount()` or
  `activity()`.
- **Learn's poll and timeout timers live inside `MidiInput`**, so a step-6
  lane menu only calls `startLearn` and listens to the two signals.
- **Settings page cancel rules.** Esc is caught by an app-wide event filter,
  installed only while *this page's* learn runs. It catches Esc on any
  widget in the page's window, claiming the ShortcutOverride and consuming
  the KeyPress, so the dialog stays open. Hiding the page also cancels a
  learn the page started, including switching Settings sections or closing
  the dialog. A learn started elsewhere (the step-6 lane menu) shows on the
  buttons but isn't cancelled by the page.
- **Timeout feedback.** A timed-out learn says "Nothing was moved on the
  keyboard, so nothing changed." on the status line. A plain cancel clears
  the line.

**Gotchas for step 2**

- `AudioEngine::m_liveRouteOf[cc]` holds the lane's document CC (0x01,
  0x07, 0x0A, 0x14, 0x15) or 0xFF. `syncLiveRoutes()` runs at the top of
  the input drain in `applyMidiInput`, so a route change takes effect at the
  next callback, before that callback's messages. Add the other lanes in
  the `switch (m_liveRouteOf[...])` in `handleLiveMessage`'s 0xB0
  `default:`. The fixed cases (64/120/121/123) can't collide, because they
  are never assignable.
- **CC121 still calls `applyLiveMod(0)` unconditionally.** J5 should make it
  reset every routed lane, and decide whether MOD routed to None is still
  reset.
- `setLiveModMax` (`MainWindow::publishLiveTarget`) is unchanged. BENDR's
  0–24 scaling (D2) belongs next to it in the dispatch.
- The engine starts on `MidiRoutes().packed()` from its constructor, so
  harnesses that build an `AudioEngine` without a `MainWindow` get the
  defaults.
- reccheck's e2e mod case still sends CC1 and relies on the default route.
  midicheck's `checkLiveRoutes` leaves the routes at the defaults.
- The live-routes check pumps with the device held, like `checkLivePlay`.
  It arms capture with a private generation (0x7E57) and pops the capture
  ring directly, with no take, so it doesn't touch `m_take`.

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (35/35
each). `--midicheck` passes on build-nomidi, build-off and build-dummy too,
and all five trees build cleanly.

### Step 1 review fixes (2026-10-12, uncommitted)

**What changed**

- **Spinner stepping never moves a route.** `CcSpinBox` takes a
  `heldElsewhere` predicate (the page: "another lane holds this CC"), and
  arrows, PageUp/PageDown and the wheel step over held CCs as they already
  stepped over reserved ones. `stepEnabled()` follows suit, so an arrow
  greys out when nothing is free that way. Only a **typed** CC another lane
  holds moves it, and only when it commits (Enter or focus loss, since
  keyboard tracking is off). `MidiInput::setRoute` gained a `movedFrom`
  out-parameter, and the page reports a move on the status line exactly as
  Learn does ("CC 10 now drives LFOS instead of PAN."). A route change
  that moves nothing clears a stale report, unless a learn is listening.
- **The wheel needs focus.** The boxes are `Qt::StrongFocus`, and an
  unfocused box ignores the wheel, so it passes to the page.
- **Typed, then an arrow.** `stepBy` starts from the box's displayed text,
  as `QAbstractSpinBox::stepBy` does, so "50" then Up gives 51 (it used to
  give the old value + 1). The typed number is **not** committed on its own
  first (no `interpretText()`): with it, typing PAN's CC10 then Up would move
  PAN and then step away, which is the stealing-in-passing this fix removes.
  Qt also emits once, for the stepped value. With nowhere to step, the
  entry stays pending for Enter. An unfinished entry, such as a reserved CC,
  doesn't step, as in Qt. "None" is matched by hand, because Qt's line-edit
  validator checks the special text, not `validate()`.
- **`MidiInput::learnStarted(lane)`**, emitted by `startLearn` once armed
  (after any learn it replaced was cancelled). The page's `syncLearn` follows
  it, so a learn started elsewhere (the step-6 lane menu) shows "Listening…".
  The status line leaves out "Esc cancels" for such a learn, because the
  page's Esc filter only cancels its own.
- **Learn's slot carries a generation.** `m_learnArmed` became
  `m_learnArmedGen` (`uint32_t`, 0 = disarmed). The slot is a
  `std::atomic<uint64_t>` holding `generation << 8 | cc`, with 0 for empty.
  A MIDI thread that read the generation just before a disarm can still CAS
  in after the UI cleared the slot. `pollLearn` drops any value whose
  generation isn't the current learn's, so a straggler can never land on a
  later learn. It costs at most one 15 ms poll's worth of blocked slot. The
  MIDI threads stay lock-free: both atomics are `static_assert`ed
  `is_always_lock_free`.
- **A spontaneous hide (the window minimized) no longer cancels the page's
  learn.** Closing Settings or switching sections still does.
- `static_assert(std::atomic<uint64_t>::is_always_lock_free)` next to
  `AudioEngine::m_liveRoutes`.
- **One packed decode.** `MidiRoutes::fromPacked` is gone. The new
  `MidiRoutes::unpackLaneTable(packed, uint8_t (&laneOf)[128])`
  (allocation-free) is what `AudioEngine::syncLiveRoutes` calls, and the
  harness tests that function.

**Harness** (`--midicheck`)

- `checkRoutesTable`: the packed form unpacks to exactly `laneFor` for every
  CC (defaults and a remap). A malformed word (a reserved CC, a repeated CC,
  CC121, CC0) routes only the first valid lane.
- `checkControlsPage` now hosts the page in a `QDialog`. Keys go in the way
  a real press arrives: a ShortcutOverride, ignored until it is claimed,
  then the KeyPress, sent to the focus widget so that QApplication
  propagates it up to the dialog. It checks:
  - stepping up from None skips CC0 and MOD's CC1, and back down skips them
    again;
  - VOL stepping 5 past PAN's CC10 leaves PAN alone;
  - a typed CC10 doesn't apply before Enter, then moves PAN with the exact
    status text, and the dialog stays open;
  - typed 50 then Up gives 51, and typed 10 then Up gives 11 with PAN
    untouched;
  - the wheel over an unfocused box is ignored and changes nothing, while
    over a focused box it skips PAN's CC10;
  - a learn started on `MidiInput` directly shows "Listening…" on its
    button, without "Esc";
  - Esc on the Learn button and in a CC box cancels, the override is
    claimed, and the dialog is not rejected and still visible;
  - with no learn, Esc closes the dialog;
  - minimizing keeps the learn, and hiding the page cancels it.
- Mutation-tested 10 ways, and each failed the expected checks and was
  restored:
  - stepping doesn't skip held CCs;
  - no move report;
  - the wheel ignores focus;
  - the typed text is ignored when stepping;
  - the typed text is committed before the step (`interpretText()`);
  - no `learnStarted` hookup;
  - the Esc KeyPress isn't consumed;
  - the Esc override isn't claimed;
  - a spontaneous hide cancels;
  - `unpackLaneTable` doesn't validate.
- The generation tag on Learn's slot has no harness check, because the race
  needs a MIDI thread preempted between its load and its CAS. It is
  covered by reasoning (the comment on `m_learnSlot`).

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (35/35
each). `--midicheck` passes on build-nomidi, build-off and build-dummy, and
all five trees build cleanly.
