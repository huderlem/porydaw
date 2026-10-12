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

(none yet)
