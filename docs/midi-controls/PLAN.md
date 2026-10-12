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
| J2 | Latch scope | Latch applies only while a take is armed **and** the control sounds (live play is on). With live play off, nothing of the knob is heard, so the song plays on untouched. The latch clears at a segment boundary (loop wrap or seek), and the lane is chased back to the song: at a wrap, to the song's value carried from the loop end (what plain playback holds there, and what the take's restore point at the loop end commits), the loop start's own events then playing; at a seek, to the song at the new position. On pass 2, before you touch the knob again, you hear the song's original events, not pass 1's take, because the timeline can't be rebuilt mid-take. This is a known mismatch with what gets committed there. |
| J3 | Program numbers | The data byte is used as the voice slot directly (0–127, no bank select). A program change past the voicegroup's last voice is ignored and shown once in the status bar. |
| J4 | Voice writes at tick 0 | A take that changes voice at tick 0 replaces the tick-0 program change; it never deletes it. The track's keep-alive seed (`appendTrackKeepAliveOps`) still holds. Restore-after-take puts the song's voice back at the segment end, just as it does for other lanes. |
| J5 | CC121 (reset all controllers) | It resets every routed lane to the engine's track-init default (the table in `TimelinePlayer::chase`) and leaves the voice alone. In a take, that reset is recorded as moves only on lanes the take already moved (bend and MOD included); untouched lanes keep the song's automation — refined 2026-10-12 after step 3 review. |
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

### Step 2 — live play of routed lanes, program change, touched mask, CC121 (2026-10-12, uncommitted)

**What landed**

- `AudioEngine::applyLiveMod` became `applyLiveControl(lane, value)`, where
  `lane` is the document CC (0x01, 0x07, 0x0A, 0x14, 0x15) and `value` is
  already scaled. The 0xB0 `default:` branch scales per lane (D2): MOD onto
  `0..m_liveModMax` (unchanged), BENDR onto `0..24` as
  `(v·24 + 63) / 127` (round half up), and VOL, PAN and LFOS take the knob's
  value. Bend keeps `applyLiveBend`.
- **Program change.** `MidiInput::parse` now keeps 0xCn (one data byte, `d2`
  = 0; a 2-byte message, as RtMidi delivers it, and the injector's 3-byte
  form both parse), so it reaches the input ring. A new 0xC0 case calls
  `applyLiveProgram(program)`. That function does nothing without a sound
  target. If `program >= m_liveVoiceCount`, it ignores the change and bumps
  `m_liveIgnoredProgram` (`sequence << 8 | program`). Otherwise it calls
  `m4a_engine_program_change` on the target track.
- **Voice count (J3).** `MainWindow::publishLiveTarget` publishes
  `AudioEngine::setLiveVoiceCount`. The count is one past the last slot whose
  `VoicegroupSource::kindAt` isn't `None`, or 128 when the tab has no parsed
  source (the engine's table always has 128 entries, so nothing reads past
  it). `publishLiveTarget` already runs on tab activation, after every
  document change (including a `-G` voicegroup swap) and at take start/end,
  so the count follows the voicegroup.
- **The report.** `MainWindow::pollLiveKeys`, which MIDI activity already
  runs, compares `liveIgnoredProgram()` with what it saw last. When the word
  changed, it shows one status message per poll, however many changes
  arrived since: "Program change 120 ignored: this voicegroup's voices are
  0–9." `m_liveIgnoredReports` counts the reports for the harness.
- **Touched mask.** `bool m_liveBent` became `TouchedLanes m_liveTouched`,
  a plain struct in `core/timelineplayer.h` holding 128 CC bits, a bend bit
  and a program bit. `applyLiveBend`, `applyLiveControl` and
  `applyLiveProgram` set the mask only when the move sounded on a target.
  It clears at a target change, as before, and also in
  `loadSong`/`unloadSong`, where the engine is fresh or songless.
- `TimelinePlayer::chaseBendMod` is **removed**. The new
  `chaseTouched(engine, timeline, pos, track, touched)` does one pass over
  the track's events at or before `pos`:
  - Each touched CC gets its song value, or `ccDefault(cc)` when the song
    has none. `ccDefault` is the `kCcDefaults` table, now file-scope and
    shared with `chase()`.
  - A touched bend gets its song value, or centre.
  - A touched voice gets the program at or before `pos`. When there is none
    there, the scan goes on past `pos` to the track's first later program
    change, which is what `primeVoices` would have applied.
  - A track with no program change anywhere keeps the keyboard's voice. No
    event can stand for "no voice", and an engine track's initial
    zero voice isn't something to restore by hand.
  - An empty mask returns before scanning.
- **CC121 (J5).** `syncLiveRoutes` also computes `m_liveRoutedLanes` (bit i
  = `MidiRoutes::kLanes[i]` has a CC). CC121 lifts the pedal, centres the
  bend, then calls `applyLiveControl(lane, ccDefault(lane))` for each routed
  lane in `kLanes` order. MOD's reset is therefore still captured as a
  `Mod 0` when MOD is routed, exactly as before. **A lane routed to None is
  not reset, MOD included** ("every routed lane"). The voice is never
  touched.
- **Capture is unchanged.** Only MOD pushes a capture event (still
  `CaptureEvent::Mod`). VOL/PAN/BENDR/LFOS moves and program changes are
  heard but not captured. Capturing them is step 3 (and step 5 for the
  voice), so a take recorded in this step commits only notes, bend and MOD,
  as before.

**Stuck-value audit (matches bend/mod today):**

- Seek, `updateTimeline` (any document edit) and `updateVoicegroup` run a
  full `chase()`. All five lanes are in `kCcDefaults`, and the program is
  chased or primed, so live VOL/PAN/BENDR/LFOS and the live voice are
  replaced by the song's, as bend and MOD are.
  - Exception: a track with no program change anywhere keeps a live voice
    through a chase too, because `chase()` doesn't touch its voice either.
- A song switch reinitialises the engine.
- Stop, pause, panic (the command and CC120/123), a port closing and a
  tab's release don't chase anything. So a parked knob's VOL, like a
  parked wheel's bend, stays on the track until the next seek, edit or
  target change. That was already true for bend and mod. The touched mask
  survives a chase, so a later target change re-chases lanes the song
  already holds, which is harmless (same values).

**Known issues**

- Auditions (clicking roll notes) on the target track play with the
  keyboard's live voice, BENDR and LFOS until a handback, seek or edit,
  as they already did with a live bend or MOD. They sound on the target
  track's engine state, which the keyboard has moved.

**Harness**

- `--midicheck` `MainWindow::checkLiveControls`, after `checkLiveRoutes`,
  with the device held. It seeks to 0, so both tracks hold their chased
  song state, then routes BENDR to CC20 and LFOS to CC21. It checks:
  - VOL (`rawVolume`, and `volume` through the master volume), PAN
    (byte − 64) and LFOS at the knob's value;
  - BENDR at 0→0, 3→1, 127→24, 64→12;
  - an unrouted CC22 moves nothing;
  - track b's seven lanes (VOL, PAN, BENDR, LFOS, MOD, bend, program) are
    untouched by any of it;
  - the published voice count equals the source's;
  - a 2-byte program change switches the target's `currentProgram` (and
    voice type) and leaves b alone;
  - with the count shortened to 10, programs 10 and 120 in one burst are
    ignored and give exactly one status message (naming 120 and "0–9"); a
    second poll doesn't re-report, and program 9 still plays;
  - republishing restores the real count;
  - CC121 with PAN and MOD routed to None resets VOL 127, BENDR 2, LFOS 22
    and bend 0, keeps PAN, MOD and the voice, and leaves b alone;
  - selecting b, after every lane has been moved off its song value
    (asserted), hands all seven of a's lanes back to exactly the song's
    values, and b is undisturbed;
  - with no target, a program change is neither applied nor counted, and a
    knob does nothing.
  Routes go back to the defaults.
- The parse table now says that a program change parses (2 bytes, a masked
  data byte, a third byte ignored), and that `{0xC0}` and `{0xB0, 7}` are
  rejected as truncated.
- `--primecheck` gains a `chaseTouched` unit block on its synthetic song.
  Every lane of tracks 0–2 is poked first. Then:
  - VOL, BENDR, bend and voice on track 0 come back as song VOL, default
    BENDR 2, centred bend and the tick-0 voice, while PAN, LFOS and MOD
    stay as poked;
  - PAN, LFOS and MOD chased late give the late PAN, LFOS 22 and MOD 0,
    and VOL isn't re-chased;
  - track 1's voice, with nothing before `pos`, comes from its first later
    program change;
  - track 2 has no voice event anywhere, so it keeps the keyboard's voice;
  - an empty mask moves nothing;
  - `ccDefault` matches the engine.
  This is where "untouched lanes stay untouched" is proven: end to end, a
  knob can't move a lane without touching it.
- `--scriptcheck` changes:
  - `receivedCount` now grows by 6 for the tap batch, because the program
    change reaches live play's ring.
  - The tap-overflow burst uses channel pressure (0xD0) instead of program
    changes, so the burst can't change the live target's voice 1100 times.
- Mutation-tested 15 ways. Each failed the expected checks and was
  restored byte for byte (a script asserted the restore):
  - BENDR unscaled;
  - the CC applied to the wrong track;
  - no CC touch bit;
  - no program touch bit;
  - CC121 resetting all lanes;
  - CC121 resetting MOD only;
  - no voice-count limit;
  - a program counted with no target;
  - re-reporting every poll;
  - no voice-count publish;
  - parse dropping 0xC0;
  - `chaseTouched` ignoring the program;
  - no later-program fallback;
  - the mask ignored (all five lanes chased);
  - bend always chased.

**Notes for step 3**

- `applyLiveControl` is where VOL/PAN/BENDR/LFOS capture goes. It currently
  pushes only for `lane == 0x01`, as `CaptureEvent::Mod`. `pushCapture`'s
  `lateSamples` list names `Mod`, so add `Control` there too.
- CC121 already calls `applyLiveControl` for every routed lane, so once
  capture is generalised its VOL 127 / PAN 64 / BENDR 2 / LFOS 22 resets
  will record as moves (J5) with no further work. Check that reccheck's
  CC121 case expects them.
- The capture value is the scaled, as-heard value (BENDR in semitones, MOD
  in the lane's range), the same as MOD today.
- Program changes are deliberately not captured (step 5). `applyLiveProgram`
  returns early without a target, and step 5 must revisit that, because
  notes are captured even when they don't sound.

**Windows checkpoint (manual test script)**

1. Open a song with a few tracks, select a track with a sustained voice,
   and press Play (or just hold notes while stopped).
2. Settings → MIDI → Controls: click **Learn** on VOL and turn a knob; then
   do the same for PAN, BENDR and LFOS with three other knobs. MOD stays on
   the mod wheel (CC1) unless you change it.
3. While holding a note:
   - **VOL**: turning down makes the track quieter; full up is loud.
   - **PAN**: sweeps the track from left to right; the middle is centre.
   - **LFOS**: with the mod wheel up (vibrato on), changes the vibrato
     speed. Down to 0 stops the vibrato.
   - **BENDR**: with the pitch wheel pushed up, sets how far it bends, up
     to two octaves at full turn. At the knob's bottom the pitch wheel does
     nothing.
4. Press your keyboard's program-change buttons (or use its program
   up/down). The selected track's voice changes; the notes you play use the
   new voice. Going past the voicegroup's last voice changes nothing and
   shows "Program change N ignored: …" in the status bar.
5. Move every knob away from where it was, change the voice, then click
   another track in the song. Play the song: the track you left sounds
   like the song again (its volume, pan, bend range, vibrato speed and
   voice); the new track is unaffected until you touch something.
6. If your keyboard can send Reset All Controllers (CC121; often a
   "reset" or "panic" function, so check its manual): with knobs moved, send
   it. VOL jumps to full, PAN to centre, BENDR to 2, LFOS to the default
   speed, the bend centres and MOD goes to 0, but the voice stays. Set PAN
   to None in Settings and repeat: PAN is no longer reset.
7. Known, by design: stopping playback or a panic doesn't undo a knob (the
   same as the pitch wheel today). A seek, an edit or a track change does.
   Knob moves of VOL/PAN/BENDR/LFOS and program changes aren't recorded
   yet (steps 3 and 5).

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (35/35
each). `--midicheck` and `--primecheck` pass on build-nomidi, build-off and
build-dummy, and all five trees build cleanly.

### Step 2 review fixes (2026-10-12, uncommitted)

**Engine (poryaaaa).** A local branch `lfos-phase` in the submodule holds
commit d440c05, which isn't pushed. Porydaw's submodule pointer is bumped
but not committed.

- `m4a_engine_cc` case 0x15 (LFOS) zeroed `lfoSpeedC` and `modM` on every
  message. The GBA's `ply_lfos` only stores `lfoSpeed`, and calls
  `clear_modM` when the value is 0. So a streaming LFOS knob, or a song
  that repeats the command, kept restarting the LFO's phase, and the
  vibrato stalled. A nonzero speed now only changes the rate.
- poryaaaa's `test_engine.c` gains `test_lfos_keeps_phase`. With the old
  code it fails 5 of its 9 asserts. `poryaaaa_unit_tests` passes 716/716,
  and `poryaaaa_render` and `poryaaaa_test` still build. They were built by
  hand with gcc, because the submodule's own CMake needs the clap-wrapper
  checkout.
- Checked against `m4a_1.s`:
  - `ply_mod`, `ply_vol`, `ply_pan`, `ply_bendr` and `ply_bend` match for
    streaming. The engine's handlers push the change to channels at once,
    while the GBA flags it for the end of the tick, which a knob can't
    tell apart.
  - Not fixed: `ply_modt`, `ply_tune` and `ply_lfodl` are still TODO
    no-ops. Because LFODL is ignored, `lfoDelayC` is never set, so the
    note-on reload of `lfoDelayC` (with its `clear_modM`) never happens.

**Porydaw**

- `VoicegroupSource::voiceCount()` (one past the last slot with a voice
  line) replaces `publishLiveTarget`'s loop.
- `m_liveIgnoredProgram` is now `seq << 16 | count << 8 | program`. The
  count is the one the change was ignored against, so a tab switch before
  the poll still names the right range. The sequence is 16 bits, there is
  still a single writer, and 0 still means "none yet".
- The tap comment in `midiinput.cpp` no longer lists program change among
  what live play drops.

**Harness (`--midicheck` `checkLiveControls`)**

- The voice count is checked against the voicegroup file itself: voice
  macro lines after `voice_group`, which is 95 for `abandoned_ship` (not
  96).
- The ignored-range report is checked with the real count republished
  between the ignore and the poll. The check matches "voices are 0–9.",
  so "0–94" can't pass it.
- "No target: not counted" sends program 10 against a count of 10, which
  would definitely be counted with a target.
- New LFOS block:
  - with MOD up and LFOS 1, the phase is run to 40..120;
  - LFOS 1, 1, 2 and 3, each applied in a one-frame pump, keep the phase
    and `modM`, allowing for one LFO step if a tick falls in the frame;
  - a 30 ms pump then advances the phase by 1..30 steps, with no reset;
  - LFOS 0 clears the phase and `modM`.
- Mutation-tested 7 ways, and each failed the expected checks and was
  restored:
  - the old LFOS reset;
  - only `modM` zeroed;
  - no `clear_modM` at 0;
  - the report using the poll-time count, which first passed because
    "0–94" contains "0–9" (the check was tightened);
  - `voiceCount` off by one;
  - a program counted with no target;
  - plus the poryaaaa unit test against the old code.

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (35/35
each), and no render or roundtrip harness changed with the LFOS fix.
`--midicheck` and `--primecheck` pass on build-nomidi, build-off and
build-dummy, and all five trees build cleanly.

### Step 3 — record routed CC lanes (2026-10-12, uncommitted)

**What landed**

- **Capture.** `CaptureEvent::Mod` became `CaptureEvent::Control`: `key`
  holds the lane's document CC (0x01, 0x07, 0x0A, 0x14, 0x15) and `value`
  the value it set, as heard (MOD on the lane's range, BENDR in semitones,
  the rest the knob's 0..127). `AudioEngine::applyLiveControl` pushes one
  for every lane, before the sound target is consulted, so a move is
  captured whether or not it sounds, exactly like MOD and notes before.
  `pushCapture` stamps `lateSamples` on it like a bend. Program changes are
  still not captured (step 5).
- **Lane values as stored.** Nothing converts between capture and the
  document. The lanes store the CC byte that the engine's `m4a_engine_cc`
  takes, live and in playback alike: PAN 0..127 (the engine subtracts 64
  itself), VOL raw (the engine applies `songMasterVolume` on both paths),
  BENDR in semitones. reccheck proves this end to end. It records VOL 37,
  PAN 113, BENDR 20 (knob 106) and LFOS 77, then seeks into the committed
  take, and the engine track's `rawVolume`, `pan`, `bendRange` and
  `lfoSpeed` equal what they were live.
- **`RecordingTake`.**
  - `Control.cc` is the event's key. `audible()` is unchanged: bend MSB,
    and identity for every CC lane, so a repeated value is dropped but a
    one-step change is kept.
  - `toLaneWrites` already keyed its map by (segment, cc). Within a
    segment the writes are therefore in ascending CC with BEND (0xFF) last,
    which the comments now document. The writes touch different lanes, so
    the order only makes the result deterministic.
  - Same-tick last-wins and inaudible-change dropping apply per lane,
    unchanged.
- **Restore-after-take, per lane.** The new `RecordingTake::restValue(cc)`
  returns `TimelinePlayer::ccDefault(cc)` (VOL 127, PAN 64, BENDR 2, LFOS
  22, MOD 0), centre for bend, and 0 for a CC without a default.
  `restorePoint` starts from it instead of 0. A take's last value on a lane
  with no song point before the write's end is therefore compared with what
  the engine actually plays there, the track-init value. A restore point
  is written exactly when the two sound different. MOD and bend behave as
  before, since their rest values were already 0.
- **Undo and status text.** The new `takeLanesText` names the lanes in
  lower case, as their rows are titled: pitch bend first, then the CC
  lanes in ascending CC ("mod", "volume", "pan", "bend range", "LFO
  speed"), joined "a, b and c". The existing strings come out unchanged
  ("Record 1 note(s) with pitch bend and mod", "Record mod"). New ones
  look like "Record 1 note(s) with volume, pan, bend range and LFO speed",
  "Record volume and pan", "Recorded pan.".
- **Ghosts and transient rows.**
  - `takeLanes` (MainWindow) builds a preview lane for every cc the writes
    touch, instead of the fixed {MOD, BEND}.
  - The `TakeLaneBefore` cache became a per-cc map, filled lazily and
    cleared on a revision or track change.
  - `RecordingPreview::lanes`, `mergeEmptyLanes`, read-only ghost rows and
    `paintRecordingPreview` were already lane-generic. Only comments
    changed there.
- **Axis.** `AutomationArea::laneDisplayMax` now counts the take's ghost
  points as data, just as committed points already grow a zoomed axis
  back. So turning a knob past a zoomed VOL/BENDR/LFOS range grows the axis
  while recording, instead of clipping the ghost at the row's top.
  `recordingLanesChanged` repaints the whole row (gutter range included)
  when a zoomable row's ghost peak changes, because a rescale moves every
  point. PAN and bend aren't zoomable, and PAN draws on its plain 0..127
  axis, as its rows always have.
- **Capture ring priority (§5 flood risk).** Bend and lane moves can't take
  the ring's last `kCaptureReserve` = 1024 slots (a quarter of 4096). Only
  notes, cuts, boundaries and the Begin can use them. When the UI can't
  drain in time, a knob flood therefore loses moves before it loses a note.
  The loss counts in `captureDropped()` as before, so the commit's status
  says "Some MIDI input was lost while recording.". The ring size stays
  4096 (see the flood result below).
- `docsrc/manual/midi-keyboard.md`: the recording outline gains one line
  saying that knobs routed to VOL/PAN/BENDR/LFOS record onto those lanes.

**Judgment calls**

- **Undo text uses the rows' names in lower case** ("volume", "pan",
  "bend range", "LFO speed"), not the plan's example "VOL and PAN". This
  keeps the existing "pitch bend and mod" wording and its style instead of
  mixing lane codes with words. Pitch bend is listed first so the existing
  strings don't change.
- **Moves always captured**, with or without a sound target, as MOD and
  notes already were (`applyLiveControl` pushes before the target check).
- **The reserve rather than a bigger ring.** Drained at the playhead
  timer's 17 ms, a 3 kHz flood is about 51 events a frame against 3072
  slots for moves, so it takes a UI stall of about 1 s to drop anything. A
  bigger ring would only move that edge. Keeping notes safe when it happens
  is the guarantee that matters. One counter covers both kinds of loss: the
  user-facing message is the same.
- **CC121 records VOL 127 / PAN 64 (and BENDR 2 / LFOS 22 when routed)**
  as moves, per J5, even on lanes the user never touched in the take.
  Their restores put the song back at the stop, as for bend and MOD.
  (Superseded by the step 3 review fixes: only lanes the take moved.)

**Flood check (`checkRecordingFlood`)**

The check runs three knobs (VOL CC7, PAN CC10, LFOS CC21) at 1 kHz each
for 10 s (30 000 moves), with 100 notes played through them, in 1 ms pumps,
drained every 17 ms the way the playhead timer drains.

- Result: no capture drops, all 30 000 moves in the take, one undo step
  with all 100 notes, and each lane holding at most one point a tick over
  the take's ~400 ticks (same-tick last-wins).
- `toLaneWrites` over 30 000 moves takes about 0.25 ms. The slowest
  drain-and-preview frame was about 0.3 ms (normal build, WSL).
- Then the UI stalls for 2 s (no drain) under the same flood. The moves
  overflow (drops counted, the status bar reports the loss), and every one
  of the 20 notes played meanwhile lands.
- With the reserve mutated out, that check fails: notes are lost.

**Harness (`--reccheck`)**

- Unit (`checkTakeRules`):
  - a multi-lane take keeps each Control's lane and value;
  - the writes come out in ascending CC with BEND last, segment by segment;
  - VOL drops a repeat and keeps a one-step change, PAN's same-tick last
    wins, BENDR and LFOS convert, and a second segment's writes are right;
  - `restValue` matches the engine;
  - per lane (VOL, PAN, BENDR, LFOS): a value left away from rest restores
    the rest with no song point, a value left at rest restores nothing, and
    a song point wins over the rest;
  - `applyLaneWrites` keeps to its lane, and PAN restores centre.
- End to end (`checkRecordingTakes`):
  - CC121 now expects "Record pitch bend, mod, volume and pan", with VOL
    127 and PAN 64 recorded and nothing on BENDR (unrouted).
  - A take with all four routed knobs gives one undo step that names them,
    the status text, each lane as the move as heard over the song's other
    points with the song's value back at the stop, playback that matches
    live, the song's values past the stop, and undo.
  - A looped take: a VOL move before the wrap is restored at the loop end,
    and a PAN move after it is its own write to the stop ("Record volume
    and pan").
  - A lane move's callback wait moves it back, like a note's.
- `checkRecordingCcLaneDisplay`:
  - VOL, BENDR and LFOS ghosts, sorted by cc;
  - BENDR and LFOS on transient rows, each curve back at the clock to its
    track-init value (2, 22);
  - LFOS zoomed to 32 grows past it, and the stretch drawn before the
    growth is repainted on the new axis;
  - ghost alpha on both rows, a solid curve after Stop, the undo text, and
    undo removing the take's rows.
- `--midicheck` `checkLiveRoutes` reads `Control` with key 0x01.
- Mutation-tested 13 ways. Each failed the expected checks and was restored
  byte for byte (a script asserted the restore):
  - capture only for MOD;
  - the Control's cc forced to MOD;
  - the restore from 0;
  - `restValue` 0;
  - BEND sorted first;
  - CC `audible` folding;
  - `takeLanes` fixed to MOD/BEND;
  - "volume" missing from the text;
  - no capture reserve;
  - `laneDisplayMax` ignoring the ghost;
  - no peak repaint;
  - the before-cache reading the wrong lane;
  - no `lateSamples` on Control.

**Notes for step 4 (latch)**

- The take records what the keyboard sent, but until the latch lands the
  song's own VOL/PAN/BENDR/LFOS events on the target track still play over
  a knob mid-take, so what you hear can differ from what gets committed.
  Step 4's harness can reuse `checkRecordingTakes`' `heard()` engine-state
  reads.
- `applyLiveControl` and `applyLiveBend` are where the latch bit is set:
  capture armed (`capturing()`), target sounding (`m_liveTrack >= 0`) and
  live play on. CC121's resets go through `applyLiveControl` too, so they
  will latch the routed lanes. Decide whether that is wanted (probably
  yes: what you hear is what is committed).
- The `checkRecordingTakes` routed-lanes block deliberately reads the live
  value 1 ms after each knob, before a song event could move it. With the
  latch that guard becomes unnecessary, and a check that a song VOL event
  after the knob no longer reaches the engine fits there.

**Windows checkpoint (manual test script)**

1. Settings → MIDI → Controls: route VOL, PAN, BENDR and LFOS to four
   knobs (Learn). Select a track that holds notes (a sustained voice is
   easiest to hear).
2. Put the edit cursor a bar before a phrase and press Record. While it
   plays, play a few notes and turn each of the four knobs.
   - While recording, the Volume, Pan, Bend range and LFO speed rows show
     your moves as a faded curve in the track's colour. A row the track
     didn't have (often Bend range and LFO speed) appears for the take.
   - Zoom the LFO speed row's value range small first (row menu) and turn
     the knob high: the row's scale grows to fit while recording.
3. Press Stop.
   - The curves turn solid. The undo menu and the status bar say e.g.
     "Record 3 notes with volume, pan, bend range and LFO speed".
   - Right after where you stopped, each lane steps back to the value the
     song had there (for a lane the song never set: volume full, pan
     centre, bend range 2, LFO speed 22).
4. Play the recorded stretch back: it sounds like what you played (until
   step 4, a song's own volume or pan changes during the take may have
   overridden a knob while you recorded; the recording itself has your
   values).
5. Ctrl+Z: notes and all four lanes go back as they were, and rows that
   appeared only for the take disappear.
6. Loop: turn on Loop with a short loop region and record across the loop
   end. Turn VOL before the wrap and PAN after it. After Stop, VOL steps
   back to the song's value at the loop end, and PAN's move starts on the
   second pass and is restored where you stopped.
7. Turn only one knob in a take (no notes): the undo step says e.g.
   "Record pan".
8. If your keyboard sends Reset All Controllers (CC121) mid-take, the take
   records the reset only on the lanes you already moved in that take
   (volume full, pan centre, bend centred, mod 0, ...), restored to the
   song's values at the stop. A lane you didn't touch keeps the song's
   automation. Live, every routed lane and the bend still reset.
9. Pitch bend and the mod wheel still record exactly as before.

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (35/35
each). `--midicheck` and `--reccheck` pass on build-nomidi, build-off and
build-dummy, and all five trees build cleanly.

### Step 3 review fixes (2026-10-12, uncommitted)

**What changed**

- **CC121 records only on lanes the take moved (J5 refined).** Live
  behaviour is unchanged: CC121 still lifts the pedal, centres the bend and
  resets every routed lane audibly. But each of those resets is now
  captured with the new `CaptureEvent::Reset` flag (a `flags` byte on the
  event; `applyLiveBend`/`applyLiveControl` take a `reset` argument that
  `pushCapture` stamps). `RecordingTake::push` keeps a per-take
  `m_moved[256]` (by cc, BEND included), set by every unflagged move and
  cleared by `clear()`/`begin()`, and drops a flagged event on a lane not
  yet moved. The filter lives in the take, not on the audio thread: no
  per-take state there, and the arm/generation filter that already guards
  `push` scopes it to the take. `m_moved` spans segments, so a lane moved
  in pass 1 and reset in pass 2 records the reset. This also changes bend
  and MOD: their CC121 resets used to be recorded always, and are now
  recorded only after a move of that lane in the take. Before this, a
  CC121 wrote a point on every routed lane from the reset to the segment
  end, wiping the song's VOL/PAN/... automation there and leaving
  redundant BENDR 2 / LFOS 22 points on lanes that had none.
- **Full-row ghost repaint only on a rescale.** `recordingLanesChanged`
  now compares the row's display max with the old and new ghost
  (`laneDisplayMaxWith`, which `laneDisplayMax` now wraps) instead of the
  ghosts' peaks. A VOL ghost rising under its fixed 127 axis repaints only
  from the changed tick, as any other change does. The automation area
  counts full-row rescale repaints in a `rescaleRepaints` property for the
  harness.
- `MainWindow::takeLanes`: the dead `lane.ranges.empty()` skip is gone
  (each cc comes from the writes, so it always has a range).
- **`takeLanesText` is a `MainWindow` static** (so reccheck can call it).
  It now names `DOC_CC_VOICE` "voice", ordered last (after LFO speed), and
  a cc it has no name for reads "CC n" in CC order instead of vanishing, so
  the text can't come out "Record ". The joining is translatable piece by
  piece: `tr("%1 and %2")` for the last pair, then `tr("%1, %2")` for each
  earlier name, both with disambiguation comments.
- **Flood checks.** `notes == 100` / `notes == 20` counted only what the
  harness sent. They are now `m_take.notes().size()` read after the last
  drain, next to the existing committed-document count.

**Judgment calls**

- **Not `QLocale::createSeparatedList`.** On Qt 6.2.4 its output is
  "a, b, c" in the C locale (no "and"), "a, b, and c" in en_US (Oxford
  comma), and "a, b and c" in en_GB. It also follows the user's locale
  rather than the UI's language. So the existing English strings ("Record
  pitch bend, mod, volume and pan") would change by locale. Instead each
  separator is its own `tr()` string, which keeps the wording stable and
  still lets a translation choose both separators.
- **The unknown-cc fallback is "CC n"** (e.g. "Record volume, CC 24 and
  voice"), sorted among the CC lanes by number.
- **`m_moved` is per take, all segments**, as the review asked. A move
  dropped by a full ring doesn't mark its lane, so a later reset there is
  dropped too. That's consistent: neither is in the take.

**Harness (`--reccheck`)**

- Unit (`checkTakeRules`): a VOL moved in pass 1 keeps its CC121 resets in
  pass 1 and pass 2; bend's reset in pass 1, before any bend move, is
  dropped, and its pass-2 reset after a bend move is kept; MOD and PAN,
  never moved, record nothing. A fresh arm whose only input is a CC121
  records no controls and no writes.
- Unit (`runRecordWindowCheck`): `takeLanesText` for no lanes, one, two,
  four (out of order, duplicated), the voice last, voice alone, and an
  unknown CC 24 ("CC 24", "volume, CC 24 and voice").
- End to end (`checkRecordingTakes`): with a song PAN point at the cursor,
  a take moves the bend and VOL, then sends CC121. It commits "Record pitch
  bend and volume": the bend and VOL lanes hold the moves and their resets
  (centre, 127) with the song back at the stop, while the MOD, PAN and
  BENDR lanes are exactly as before.
- `checkRecordingCcLaneDisplay`: VOL is set to a flat 40 before the take,
  so a rising VOL knob (60..120) raises its ghost's peak. That causes no
  full-row repaint (`rescaleRepaints` unchanged), while LFOS 100 past its
  zoomed 32 does. A grab after the VOL frames flushes their repaints, so
  the existing "stretch before the growth repainted on the new axis" pixel
  check keeps its teeth. (Without that grab the VOL frames' pending
  repaints healed the stale LFOS stretch, and the pixel check passed with
  the rescale repaint mutated out.)
- Mutation-tested 13 ways, each failing the expected checks and restored
  byte for byte (a script `cmp`ed the restore):
  - no reset filter;
  - every reset dropped;
  - `m_moved` cleared per segment;
  - `m_moved` not cleared on clear/begin;
  - the engine not stamping the flag;
  - the old peak comparison for the full-row repaint;
  - no rescale repaint;
  - no generic "CC n" name;
  - no "voice" name;
  - voice sorted first;
  - no capture reserve (flood stall check);
  - flood stall check on the take's notes alone, off by one;
  - (and, while checking the pixel check, the rescale repaint mutated out
    with the VOL loop disabled).

**Note for step 4 (latch)**

- A CC121 reset on a lane the take hasn't moved is now not recorded, so
  it shouldn't latch that lane either: the song's events there are what
  gets committed. Latch a lane on a reset only when the take already moved
  it (the audio thread would need its own moved mask per arm for that, or
  the latch could skip resets entirely and rely on the earlier move having
  latched the lane already, which is simpler).

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (35/35
each). `--midicheck` and `--reccheck` pass on build-nomidi, build-off and
build-dummy, and all five trees build cleanly.

### Step 3 Windows checkpoint follow-up (2026-10-12)

User's Windows pass of step 3: everything works, except the BENDR row drew the
full 0–127 axis while the knob only reaches 0–24 (D2). Fixed, user-chosen
option: the BENDR row's default value axis is **0–24** (`laneRangeDefault`,
`kBendRangeAxis` in `songview.cpp`), and its Value range menu gains a 0–24
entry (BENDR only). A song's points above 24 still grow the axis, as on any
zoomed row. `--midicheck` asserts the default axis (mutation-tested); the
reccheck BENDR ghost probe now samples below the line (24 tops the row); and
rollcheck's data-free-lane probes, which pick BENDR first and mirror a
0–127 axis, pin that row to 0–127 with `setLaneDisplayRange`. Sweeps normal +
ASAN PASS.

### Step 4 — latch (2026-10-12, uncommitted)

**What landed**

- **`LaneLatch`** (`core/timelineplayer.h`): a track plus a `TouchedLanes`
  (128 CC bits, bend, program), with `holds(ev)`: a CC event whose number is
  latched, a bend if the bend is, a program change if the voice is, on the
  latch's track only. Notes and every other event never match.
- **`TimelinePlayer::render` / `settleWrap` take a `LaneLatch *`**, default
  null, passed on to `dispatchDue`. A null or empty latch costs one pointer
  test per event: `dispatchDue` nulls an empty one on entry. A held event is
  skipped after the loop-end note-on rule and before the keyed-on
  bookkeeping (it is never a note).
- **The wrap ends the latch inside the player.** Once the loop end's
  events have dispatched, and before the position moves to the loop start,
  `dispatchDue` calls `chaseTouched` for the latched lanes at the loop end
  (events at or before it, the loop end's own included). That is the
  song's carried loop-end value, what an unlatched pass takes across the
  GOTO, and what the take's restore point at the loop end commits. It then
  clears the latch in place, so the loop start's own events all play on top,
  and pass 2 plays the song's own events (J2). (As first landed it chased at
  the loop start; corrected in the step 4 review fixes.)
- **Every other path passes no latch**: `wavexport.cpp`, the render CLI
  (`tools/porydaw_render_cli.cpp`), loopcheck, polycheck, xcmdcheck. Only
  `AudioEngine::process` (render) and `settleLoopWrap` (settleWrap) pass
  `&m_liveLatch`. Audited by grepping every `render(` / `settleWrap(`
  caller.
- **`AudioEngine`**:
  - `m_liveLatch`, `m_captureMoved` (the lanes this arm's take moved: an
    unflagged move while `capturing()`, sounding or not, cleared at the
    Begin; the audio-thread twin of `RecordingTake::m_moved`) and the last
    live value per lane (`m_liveCcValue[128]`, `m_liveBendValue`,
    `m_liveProgramValue`).
  - **Set:** `applyLiveBend` and `applyLiveControl` latch a lane when
    `capturing()` and the move sounds (`m_liveTrack >= 0`, which already
    means live play is on and a song is loaded), and the lane is in
    `m_captureMoved`. An unflagged move has just been put there. A CC121
    reset is flagged, so it latches only a lane the take already moved,
    exactly where the take records it (step 3 review fixes). A lane moved
    on pass 1 and reset on pass 2 is latched again by the reset, and the
    take records that reset.
  - **Release** (`releaseLiveLatch(pos)`: `chaseTouched` of the latched
    lanes at pos, then clear):
    - in `applyTransportTransition` for Stop and Pause, at the stop
      position, before Stop rewinds;
    - in `syncCapture` when the arm changes (disarm, punch-out, new arm);
    - in `applyMidiInput` on every callback that isn't capturing (the catch-
      all for a disarm or a stopped transport);
    - at the app's panic and at a tab's release (`liveAllNotesOff`,
      `liveReleaseNotes`).
  - **Drop** (clear, no chase, because a full chase or a fresh engine
    already covers the lanes): at a seek (`applyPendingSeek`, and a seek that
    races into `updateTimeline`); at a target change, where the touched-lanes
    chase covers the latch (latched ⊆ touched); and in `loadSong` /
    `unloadSong`.
  - **Reapply after a full chase** (`reapplyLiveLatch`): `updateTimeline`
    (a document edit mid-take) and `updateVoicegroup` re-send the knob's
    last values for the latched lanes after their `chase()`. What you hear
    stays what gets committed. When the arm is already gone (the commit's
    own edit lands in `finishRecording` after `armCapture(0)`, before the
    audio thread has seen the disarm), it drops the latch instead, so the
    chase stands.
  - Program change: `applyLiveProgram` stores `m_liveProgramValue`, but
    the program bit is never set (step 5).
- Not recording: no latch, last-wins as before. `capturing()` gates the
  set, and the catch-all releases anything left.

**Judgment calls**

- **The wrap clears inside the player**, through the latch pointer, rather
  than AudioEngine noticing `wrapCount()` after render. One render call can
  cross the wrap and go on playing pass 2, so a clear after render would
  skip the loop start's events and the rest of the buffer.
- **CC121 latching** uses an audio-thread `m_captureMoved` rather than
  "a reset never latches". With the simpler rule, a lane moved on pass 1
  (the latch then ended at the wrap) and reset on pass 2 would be recorded
  but not latched. Now latched equals recorded and sounding in every case
  except a move lost to a full capture ring (the take doesn't mark it
  moved, while the engine does).
- **Live play off → recorded, not latched** (J2). A lane moved while not
  sounding (live play off, or no target) is in `m_captureMoved` but not
  latched, so the song's events play over what the take records there.
  Turning live play on and moving the lane again latches it. A CC121 that
  sounds latches it too, because the take moved it.
- **The keyboard's CC120/123 (all notes off) don't end the latch.** They
  concern notes; the app's panic does end it. *(Changed in the step 4
  review fixes: mid-take, the panic and a port closing don't either.)*
- **A document edit keeps the latch** unless it carries a raced seek. A
  track move or delete mid-take: `updateTimeline` reapplies on the old
  index, and the next callback's target change (`publishLiveTarget` follows
  the take to the new index) hands that index back to the song by
  `chaseTouched`. At most one callback of the knob's value on whichever
  track now has the old index. Not worth more machinery. *(Fixed in the
  step 4 review fixes: the take's track lost its latch there.)*
- **Stop's own release chases at the stop position**, before the rewind.
  In the app a take's end is usually chased twice over anyway: the commit's
  edit runs a full chase (its reapply drops the latch, since the arm is
  gone), and Stop then seeks to the edit cursor. The audio-side releases
  are what matter when nothing is committed or the commit is deferred (an
  open edit group), and for an auto-stop.

**Harness**

- `--primecheck` gains a latch block on a new looped synthetic song
  (`buildLatchSong`, loop ticks 5..30, set on the timeline):
  - a null latch and an empty latch play every event;
  - VOL, PAN, bend and voice latched on track 0 after the keyboard set them
    hold exactly those events (VOL 90/80, PAN 100, bend +1024, voice 9),
    while MOD, track 1's VOL and track 0's note play;
  - the loop end's VOL 11 is held too;
  - one tick past the wrap the latch is gone and its lanes equal an
    unlatched run's at the same point: the loop end's state carried (PAN
    100, bend +1024, voice 9), then the loop start's VOL 90; never the
    song's at the loop start (PAN 20, bend centred, voice 5) (review fixes);
  - pass 2 plays the song;
  - a latch on track 1 leaves track 0 alone.
- `--reccheck` `checkRecordingLatch(a, b)`, after the routed-lanes block, on
  setup points (a: VOL 99 / PAN 30 / bend 2048 at t1, VOL 88 / PAN 70 at
  t2; b: VOL 55 at t1). The expected song values come from the document
  (`songAt`), so the song's own automation can't break it. It checks:
  - **In a take**, VOL and the bend moved before t1 hold past t1, while
    PAN (unmoved) and b's VOL play t1's events.
  - **A document edit mid-take** keeps the held lanes at the knob's values.
  - **CC121** resets VOL, PAN and the bend live. VOL and the bend stay held
    past t2; PAN, never moved, plays t2's event.
  - **With no take**, the knob lasts until the song's next event, and the
    event wins.
  - **With a private arm** (no take, so no commit edit chases on its own):
    a disarm, Pause, Stop, a seek and live play going off each end the
    latch; the app's panic and a port closing don't (review fixes). The lanes are chased to the song at the playhead
    (immediately, not at the next event), and later song events play.
  - **Live play off in a take**: the knob isn't heard, t1's VOL plays, and
    the move is recorded.
  - **Loop**: a VOL point before the loop start, one early in the loop and
    a PAN point on the loop start. VOL and PAN are moved just before the
    loop end. Right after the wrap they equal a plain play of the same
    stretch (the loop end's VOL, the loop start's PAN; review fixes); the
    unheld bend carries across as ever; pass 2 plays the in-loop VOL point.
- The routed-lanes block's 1 ms workaround is gone: each knob's live value
  is read 40 ms after the move, held by the latch.
- Mutation-tested 14 ways. Each failed the expected checks and was restored
  byte for byte (a script `cmp`ed every file, plus a final md5 check):
  - no skip;
  - `holds` ignoring the track;
  - the wrap keeping the latch;
  - the wrap chasing at the loop end;
  - the wrap not chasing;
  - latching with no take (caught only after the "knob lasts until the next
    event" check was added: the catch-all release had healed it a callback
    later);
  - a reset always latching;
  - `m_captureMoved` not cleared at the Begin (PAN, moved in earlier takes,
    latched by CC121);
  - no reapply;
  - no disarm release (both the `syncCapture` one and the catch-all);
  - no Pause release (the transition's and the catch-all);
  - the seek keeping the latch;
  - the panic keeping it;
  - the target change keeping it.
- Render, roundtrip and golden harnesses are unchanged (the sweep covers
  them). No call site but `AudioEngine`'s passes a latch.

**Notes for step 5 (program latch)**

- Set `m_liveLatch.lanes.program` (and `m_captureMoved.program` for an
  unflagged, captured change) in `applyLiveProgram` under the same rule as
  the other lanes. `holds()` already matches 0xC on the latched track.
  `chaseTouched` already restores the voice (with the later-program
  fallback), and `reapplyLiveLatch` already re-sends `m_liveProgramValue`
  before the CCs.
- `applyLiveProgram` returns early without a target, before any capture.
  Step 5 must capture first, as the other lanes do, so that "moved while
  not sounding" works for the voice too.
- A latched voice's `chaseTouched` at a wrap uses the song's program at or
  before the loop start. A track whose only program change is after the
  loop start gets that later one (the `primeVoices` rule), not the voice
  it had on pass 1; check that's what step 5 wants.

**Windows checkpoint (manual test script)**

1. Pick a track with VOL or PAN automation in the song (or add a few
   Volume points a bar or two after the edit cursor with the pencil, e.g. a
   dip to 30). Route a knob to VOL (Settings → MIDI → Controls) and select
   that track.
2. Press Record a bar before the automation. Turn the VOL knob before the
   automated stretch, then leave it. Through the stretch, the track stays
   where the knob put it: the song's own volume points don't pull it away.
   Other tracks' automation still plays, and so does this track's PAN
   automation if you don't touch PAN.
3. Stop. Play the stretch back: it sounds like the take (your knob, not the
   old points, which the take replaced).
4. Undo. Play without recording, turning the knob before the stretch: as
   before step 4, the song's next volume point takes over from the knob.
5. Loop: set a short loop over the automated stretch and record. Turn the
   knob on pass 1. At the wrap the track's volume goes back to the song's:
   the value it has at the end of the loop carries into pass 2, just as in
   plain looped playback (then the loop start's own points, if any, play).
   Pass 2 plays the song's points again until you touch the knob again
   (then it holds again). By design: pass 2 doesn't play pass 1's take back
   (J2). After Stop, the take's stretch ends at the loop end on that same
   value.
6. Pause mid-take: the take commits, and the track's volume is the song's
   (now including your take) at the pause point. Same with toggling Record
   off while playing (punch-out): playback goes on with the song's own
   automation again.
7. Live play off (Settings → MIDI): record turning the knob. You don't hear
   it, the song's volume points play as usual, and the knob's moves are
   still recorded.
8. Pitch bend and the mod wheel hold in the same way: during a take, a bend
   moved before a song bend point keeps the wheel's value through it.
9. If your keyboard sends CC121 mid-take, the lanes you already moved stay
   held at their reset values; a lane you never touched (say PAN) resets
   live but then plays the song's next PAN point.
10. Panic and unplug mid-take: turn the VOL knob before the automated
    stretch, then use Tools → MIDI: All Notes Off (or unplug the keyboard)
    before the stretch. The hold stays: the song's volume points still
    don't pull the track away, and after Stop the stretch plays back as you
    heard it.
11. Track insert mid-take: while recording with the knob holding, add a
    track and drag it above the recording track. The take follows its
    track, the hold stays on it through its volume points, and the track
    that now sits in the old row plays its own volume.

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (all
harnesses). `--reccheck`, `--midicheck` and `--primecheck` pass on build-nomidi,
build-off and build-dummy, and all five trees build cleanly.

### Step 4 review fixes (2026-10-12, uncommitted)

**What changed**

1. **The wrap chases to the loop end** (medium). `dispatchDue`'s wrap
   branch called `chaseTouched` after moving to the loop start, so a
   released lane took the song's value there. Plain playback carries each
   controller's loop-end state into pass 2 (the GOTO touches none), and the
   take's restore point at the loop end (`LaneWrite::restorePoint`, `<=
   tickEnd`) commits that same value. The chase now runs after the loop
   end's events have dispatched and before the position moves (at `m_pos`,
   the loop end, its own events included). The latch then clears, and the
   loop start's events play on top. After the wrap, a released lane is
   where an unlatched pass is, which is also the committed restore value.
   J2's wording now says so.
2. **A panic or a port closing mid-take keeps the latch** (medium). The
   take goes on recording the knobs to its end, so releasing the latch
   there made what you hear differ from what gets committed. The panic
   branch in `applyMidiInput` no longer calls `releaseLiveLatch`. Outside
   a take there is no latch: the catch-all (`!capturing()`) releases
   anything left, so panic behaves as before. Notes and the pedal are
   handled as before. A tab's release (`liveReleaseNotes`) still releases,
   which changes nothing: a tab switch ends the take first.
   - **Live play off mid-take** (target → −1) still releases the latch, as
     J2 sanctions: the knob no longer sounds. It is a known mismatch for
     the rest of the pass. From there to the segment end you hear the
     song's events, while the commit writes the knob's last value.
3. **A track move keeps the take's latch** (low).
   `MainWindow::followTakeTrack` now publishes the move
   (`AudioEngine::followLiveTrack(generation, old, now)`) before the
   rebuilt timeline goes in. `updateTimeline` (a cold op, device stopped)
   consumes it with the timeline swap. When the take `generation` has begun
   on the audio side and `m_liveTrack` is `old`, it moves `m_liveTrack`,
   `m_liveTarget` and the latch's track to `now`, keeping the bits and
   the live values. Then, as for any edit, the full chase gives the old
   index to whichever track has it now, and `reapplyLiveLatch` re-sends
   the knobs on the new index. There is no stray callback: the timeline
   and the index change together. The reapply never writes onto the old
   index, because the follow is consumed before it.
   - **Why an explicit publish.** During capture the target is locked to
     `m_take.track()` (`publishLiveTarget`), so in the app a target change
     mid-take is either the take's own remap or live play going off. But a
     harness's private arm, or a stale state, can't be told apart on the
     audio thread by "capturing" alone. The published follow is keyed to
     the take's generation and to `from == m_liveTrack`, and is cleared at
     every `updateTimeline`, `loadSong` and `unloadSong`.
   - **The ordering guard.** Until the follow is consumed,
     `setLiveTarget(now)` publishes `old`. So a `publishLiveTarget` that
     ran between `tracksRemapped` and the rebuild (a selection signal, for
     instance) can't move the target while the old timeline still has the
     track at `old`. Two moves before one rebuild compose (`from` stays
     the first move's origin).
   - Held keyboard notes are no longer released by a target change at a
     track move. As for any edit, the rebuild's all-notes-off releases
     their sound, and the key-ups land on the track's new index.
4. **Note: the shifted-start window.** A write's `tickBegin` is the first
   move shifted back by the latency offset and the arrival wait. Song events
   between that tick and the moment the latch takes hold (tens of ms) are
   heard live but replaced by the commit. This is inherent to offset
   stamping, and left as is.
5. **A dropped move doesn't latch** (low). `pushCapture` returns whether
   the event went into the ring. `applyLiveBend` and `applyLiveControl` set
   `m_captureMoved` and the latch bit only when it did. A move the full ring
   drops is heard and gone, like a move outside a take, so "latched =
   recorded" holds there too. (A lane already latched by an earlier
   recorded move stays latched.)
6. **The transition's latch release stays, with a reason.** For Pause it
   duplicates the catch-all. For Stop it doesn't: Stop rewinds the player
   to 0 in the same transition, so the catch-all would chase the latched
   lanes at tick 0, while every other lane stays at the stop position (the
   state live play hears while stopped, when nothing seeks). A new check
   shows it: with the transition release removed, the Stop check fails
   (VOL 127 and bend 0 from tick 0, instead of 99 and 16).
7. `reapplyLiveLatch`'s `capturing()` guard is unchanged.

**Harness**

- `--primecheck` latch block: after the wrap, the latched run's lanes equal
  an unlatched run's (same song, same pokes, same 31 ticks). That run is
  asserted to hold the loop end's carried state: VOL 90 from the loop start,
  PAN 100, bend +1024 and voice 9 from before the loop end.
- `--reccheck` `checkRecordingLatch`:
  - **Loop:** the take's state right after the wrap is compared with a plain
    play (no take, no knob) of the same stretch through the same wrap, read
    at the same point. The plain VOL is asserted to be the loop end's and
    not the loop start's.
  - **Track insert mid-take:** a track is added and moved into the take's
    slot after VOL and the bend latched. The take and the target follow to
    `a + 1`. That track keeps the knob values through t1 and t2 (t2's VOL
    88 moved with it). The new track at `a` plays its own song values.
  - **Private arm, a follow published before its timeline:**
    `setLiveTarget(a + 1)` keeps `a`, a callback keeps the latch, and
    `updateTimeline` moves the target and the latch (bits kept, values
    re-sent) to `a + 1`.
  - **Private arm, the app's panic (`m_midiPanicAction`) and a port
    closing (`portsClosed`):** both keep VOL and the bend held through t2.
    The disarm after them hands the lanes back.
  - **Private arm, Stop:** the held lanes are chased to the song where the
    player stopped, not at tick 0 (the fixture is asserted to tell them
    apart).
- `--reccheck` flood, stalled UI: a bend sent into the full ring is dropped
  and doesn't latch, while the flooded VOL (captured before the ring
  filled) does (`AudioEngine::liveLatchForTest`).
- Mutation-tested 7 ways. Each failed the expected checks, and each file
  was restored byte for byte (`filecmp`, plus md5 before and after):
  - the wrap chasing at the loop start (the old rule): primecheck and
    reccheck loop;
  - the panic releasing again: reccheck panic and port close;
  - `updateTimeline` not consuming the follow: reccheck insert and the
    direct follow check;
  - `followTakeTrack` not publishing: reccheck insert;
  - `setLiveTarget` not holding the old index: the direct follow check
    (the app's own flow publishes the target only after the rebuild, so
    only the direct check sees it);
  - latching a dropped move (gate on `capturing()` again, bend): flood;
  - no Stop release in the transition: reccheck Stop.

**Sweeps:** `tools/run_checks.sh` PASS on build and build-asan (all
harnesses). `--reccheck`, `--midicheck` and `--primecheck` pass on
build-nomidi, build-off and build-dummy. All five trees build cleanly.
