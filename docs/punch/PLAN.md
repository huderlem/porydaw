# Punch recording — plan

Time-selection punch-in recording, toggled from the transport bar, a keymap
command, or the KeyLab's Punch button. Behavior agreed with the user
2026-10-17 (P1–P11, all as recommended; P11 added by the user).

## 1. Background

Today's recording (docs/midi-input/PLAN.md D5–D8): Record while stopped
plays from the edit cursor (after the count-in) and records; while playing
it punches in at once; Record again punches out, commits and keeps playing;
Stop/Space/Pause commit. The take is **overdub** (`SongDocument::addNotes`,
new note wins overlaps), written once at punch-out/stop as one undo step
(`MainWindow::finishRecording` → `commitTake`). Lane writes (BEND/MOD/VOL/
PAN/Voice) already replace only the span where the control moved. Loop
passes merge, later wins.

Punch recording here = a second record mode where the window is the time
selection and the take **replaces** the target track's notes there.

## 2. Decisions

| # | Topic | Decision |
|---|---|---|
| P1 | Toggle | Checkable **Punch** action on the transport toolbar beside Record, keymap command `transport.punch` (no default key), the KeyLab Punch button (§3) toggles it. Persisted app-wide in QSettings like the metronome Click toggle. Regular Record (Punch off) is unchanged: overdub from the edit cursor. |
| P2 | Range | The punch range is the active **time selection's tick span** [startTick, endTick), latched when Record is pressed (changing the selection mid-take moves nothing). Scope (tracks/lanes) is ignored; the take still targets the selected track. While Punch is on, the ruler tints the selection band in the record color so it reads as the punch range. |
| P3 | No selection | Punch on + no time selection: Record disabled, `recordUnavailableReason()` gives "Punch recording needs a time selection." No silent fallback to overdub. |
| P4 | Pre-roll | Record while stopped starts playback **pre-roll** bars before punch-in with the song audible. The pre-roll is the existing Count-in setting (Settings → Audio → Count-in when recording: Off / 1 bar / 2 bars, default 1 bar; no separate setting — revised 2026-10-17, §5.4), clamped at tick 0, bars measured with the meter in force at punch-in; Off starts at punch-in. No count-in clicks in punch mode. Metronome clicks if it's on. |
| P5 | Window | Only input inside [in, out) is kept. A key held at punch-in starts at punch-in (as count-in-held keys do today); a note held at punch-out ends at punch-out. Same window for BEND/MOD/VOL/PAN/Voice lane writes (a control's value at punch-in is not written unless moved inside). |
| P6 | After punch-out | Playback keeps going until Stop (no auto-stop). The take is **committed at Stop**, not at punch-out (a document edit restarts audio with all-notes-off — an audible chop right where the user is listening). Record pressed again during a punch take ends it and commits, as today. |
| P7 | Replace | At commit the take **replaces the target track's notes** in the replaced span: notes starting inside are removed; notes starting before punch-in and sounding into it are shortened to end at punch-in. This happens even for an empty take (blanking bars is a feature). Replaced span = [in, out), or [in, stop) if stopped inside, or nothing if playback never reached punch-in. With loop passes, the replaced span is the union of what any pass covered. One undo step ("Punch record %n note(s)"; empty: "Punch record (cleared)" or similar). |
| P8 | Automation | Lanes as today: a lane is replaced only where its control moved during the take (within the window). Untouched automation stays. |
| P9 | Old part silenced | During a punch take the target track's notes inside the punch range are **silent** (a playback-only mask; the document is not edited until commit). The roll still shows them, dimmed, until the commit replaces them. |
| P10 | Edge cases | Record while already playing with Punch on: arms; recording only happens in the window (if the playhead is past it, only a later loop pass reaches it). Loop passes through the window each record and merge, later wins (D8). |
| P11 | Time selection survives track switches | A plain track switch (clicking another track header / changing the selected track) **no longer clears** the time selection. Its scope already resolves live from the header selection, so the band simply re-scopes. (Song swaps still clear it.) Needed so "sweep the ruler, then pick the target track" works. |

## 3. KeyLab Punch button (MCU)

User capture 2026-10-17: one Punch press sends **two** Note Ons, D#6 and E6
at velocity 127 = notes **87 (0x57, MCU "Drop")** and **88 (0x58, MCU
"Replace")** (C4 = 60 naming, as the midi-monitor plugin prints), presumably
with Note Offs on release like every other KeyLab button. Both decode to one
new `SurfaceEvent::Punch`; the dispatch must toggle **once per physical
press**: toggle when the count of held punch notes goes 0 → 1, ignore the
second note's press while the first is held, and track releases. (A device
sending only one of the two must still work. Devices without releases are
unsupported — user decision from the control-surface work.) A press whose
partner note never comes is just a normal press. The midicheck decoder table
moves 0x57/0x58 from "unmapped" to Punch.

## 4. Steps

Per-step workflow (user): Opus 5.5 subagent implements → Opus 5.5 subagent
reviews → Opus 5.5 subagent fixes worthwhile findings → commit (no Claude
attribution) → user Windows check before the next step. Each step: all
builds clean, clang-format, harness checks added with mutation testing,
normal + ASAN sweeps.

1. **P11 + Punch mode core.** Time selection survives track switches.
   Punch action/command/QSetting, P3 gating, range latching, window
   filtering in the take (notes + lanes, P5), replace-on-commit (P7) incl.
   loop-pass union, commit at Stop (P6), Record-while-playing arming (P10),
   ruler punch tint (P2). reccheck coverage: window clipping, held-across-in
   and -out, replace + truncate, empty-take clear, stop-inside, never
   reached, two loop passes, regular Record unchanged.
2. **Pre-roll + silencing.** P4 setting + start position, count-in skipped
   in punch mode; P9 playback mask for the target track's notes in range
   (applied at take start, removed at commit/abort without leaving the
   timeline masked) + dimmed drawing. Checks: start tick for 0/1/2 bars incl.
   clamp and a meter change; masked notes produce no events; mask cleared on
   every take-ending path (stop, tab switch/close, project switch, track
   deleted).
3. **KeyLab Punch + docs.** §3 decoder + once-per-press dispatch +
   midicheck; Settings → MIDI supported-buttons list; CHANGELOG; manual
   TODO-outline section (user writes the prose); final Opus review of the
   branch.

## 5. As built

### 5.1 Step 1 (2026-10-17, committed 1176547)

Where it lives: `RecordingTake::setPunchWindow` / `punchedSpans` and the
window rules in `toNotes` / `toLaneWrites` (core/recordingtake.*); the Punch
action, P3 gating, latching, preview and the replace in `commitTake` /
`replacePunchedNotes` (mainwindow.cpp); P11, `timeSelectionChanged`,
`setPunchMode` / `setPunchLatch` / `punchRange` and the ruler tint
(ui/songview.*); `transport.punch` (ui/keymap.cpp, Global, no default,
attached to the action).

Judgment calls (things the plan left open):

- **P11 scope.** A *track*-scoped selection survives a plain track switch
  and re-scopes; a *lane*-scoped one naming a track's rows is still cleared
  (its rows are the old track's and leave the lanes area); one on the tempo
  row alone (track -1) survives. `selectTrack` now repaints the timeline
  views itself: it used to get that from `clearTimeSelection` (velcheck
  caught the missing repaint).
- **Window in ticks, after the shifts.** P5 is applied to each note's ticks
  after the latency/wait shift and rounding, before the later-wins pass. A
  lane move counts by its shifted tick, except that one the shift carried
  across punch-in (it reached the take at or after in: its unshifted tick
  is >= in) lands on in, as a note sounding across in starts there; one
  that reached the take before in is pre-window input and goes; a CC121 reset counts only on a lane
  moved *inside* the window; a lane write ends at min(segment end, out),
  where the song's value is restored as at any write end.
- **Replaced span starts where recording started.** `punchedSpans` =
  the take's recorded spans (Begin / loop start / seek landing to the
  segment's end or stop) in ticks ∩ [in, out), merged. So a take armed or
  started *inside* the window (Record while playing there, or the edit
  cursor inside it) replaces from where it started, not from punch-in.
- **Truncation.** A note starting before a replaced span and sounding into
  it ends at the earliest span start it reaches (its tail past the span is
  lost, per P7). An unterminated note-on is left alone. Done with
  `deleteNotes` (from the first read) then `resizeNotes` (one per distinct
  duration change, re-read by NoteId) inside the commit's edit group, then
  the take's `addNotes` and lane writes: one undo step. The lane writes'
  restore points and the voice lead are read *before* the replace: its
  deletes can seed a keep-alive tick-0 program change
  (`appendTrackKeepAliveOps`), which isn't the song before the take.
- **Texts.** Undo: "Punch record %n note(s)", "… with %1", "Punch record
  %1" (lanes only), "Punch record (cleared)". Status: "Punch recorded %n
  note(s).", "Punch recording cleared the range.", "Nothing was
  recorded." when nothing changed (never reached, or an empty range), or
  "The recording could not be added to the song." for an empty take with a
  replaced span that couldn't be written (locked document, track gone).
  During the take: "Punch recording into N: Name…" (count-in: "Counting in…
  then punch recording into …"). Record's tooltip names punch mode.
- **REC tint of a punch take** shows the replaced span (clipped to the
  window), not everything the playhead passed.
- **Ruler punch tint**: the record red at alpha 32 over the selection band
  plus red edge lines, ruler only. While a punch take records it shows the
  latched range (`setPunchLatch`, cleared at commit, on a dropped take and on
  a song swap) whatever the Punch toggle says; otherwise the live selection,
  only with Punch on.
- **Mode latched at Record**: toggling Punch mid-take doesn't change the
  running take (only the button/tint).
- Punch glyph: painted brackets around a dot, tinted like the transport.
- P6 needed no new code: nothing ends a take at punch-out; Stop/Space/Pause/
  Record commit as before.

Checks (reccheck): `checkPunchTakeRules` (pure take: window clipping, held
across in and out, held at a stop inside, stop inside → [in, stop), never
reached, armed past out, two loop passes → union and merge, lanes windowed +
reset rule + end at out/stop, the same events without a window unchanged) and
`MainWindow::checkRecordingPunch` (toolbar placement beside Record, command
without default key, QSettings persistence, P11 both scopes, P3 disable +
tooltip + status, ruler tint on/off, latch mid-take and released after,
held-across-in/out + after-out dropped + ghosts in-window only + REC tint =
window, no commit at punch-out, replace/truncate/keep as one step + undo,
empty take clears, stop inside, never reached, Record-while-playing arming
before the window and armed past it, two loop passes on a [8, 16)-beat loop
replacing the union and keeping a note between, Punch off = overdub with a
selection present).

Mutation tests (each applied by hand, rebuilt, reccheck run, restored): all
20 caught — note clip removed (12 fails), truncation skipped (5), span hull
instead of union (2), empty take not written (5), track switch clears again
(1), P3 gate removed (17), ruler latch not set (1), lane moves outside kept
(2), lane end not clipped (1), REC tint unclipped (1), reset always counts
(2), window not latched at Record (15), removal skipped (5), tint ignores
punch mode (1), latch ignored (1), toggle not persisted (2), `punching()`
true for an empty window (127, regular Record), commit at punch-out (5),
Punch off still punching with a selection (2), latch kept after the take (1).

Sweeps: `tools/run_checks.sh` normal and ASAN, ~/pokeemerald +
~/teamaqua-pokeemerald: PASS (36/36 each). build, build-asan, build-dummy,
build-nomidi, build-off: clean. clang-format 22 on touched files.

**Review fixes (2026-10-17, uncommitted).** An Opus review found no
high/medium bugs; applied: (1) `selectTrack` keeps a tempo-row-only lane
selection (P11), stale `trackHeaderClicked` comment fixed; (2) `commitTake`
reads lane restore points and the voice lead before `replacePunchedNotes`
(keep-alive seed); (3) a lane move shifted across punch-in is clamped to it,
not dropped; (4) an unwritable empty punch take with spans says it could not
be added; (5) the ruler tint follows the take's latch regardless of the
toggle, the live selection only with Punch on; (6) simplifications:
`replacePunchedNotes` deletes first and resizes by id per distinct change,
`RecordingTake::recordedTickSpans` (clipped only when punching) feeds both
`punchedSpans` and the preview's REC tint, a file-static `tickAt` replaces
the duplicated lambdas. New checks: `checkPunchTakeRules` offset case (note,
MOD and program change shifted across in, pre-window move and note
dropped); `checkRecordingPunch`: tempo-row selection survives a switch, a
lanes-only punch take through `commitTake` (bend moved only before the
window and its CC121 reset not written, MOD + reset inside written with the
song's value restored at out, VOL/PAN untouched, notes replaced, one step,
undo), a 100 ms offset end to end (key and MOD at in), Record again and
Pause ending a punch take, track deleted mid-take clears the latch, track
moved mid-take replaces on the moved track alone, locked document (notes,
and an empty take), Punch toggled mid-take both ways, the keep-alive case.
Mutation tests (by hand, restored from backup files): all caught —
lane-scope clear incl. tempo row (1), restore read after replace (1), lane
clamp removed (3), old status text (1), tint gated on the toggle (1),
resize skipped (2), tick spans unclipped (9), latch kept on track delete
(1), reset rule dropped (5), lane end unclipped (4), take not following a
track move (5). Not mutation-targeted: Pause/Record-again (shared
`finishRecording` path, no punch-specific code to break). Sweeps:
`tools/run_checks.sh` normal and ASAN PASS; build, build-asan, build-dummy,
build-nomidi, build-off clean; clang-format 22.

**Note for step 2 (P9 mask).** The mask must silence the same spans the
commit replaces (`punchedSpans`, growing as the take records), not the whole
latched [in, out): the replaced span starts where recording started, so a
take armed mid-window (Record while playing inside it, or the edit cursor
inside it) keeps [in, p) — masking all of [in, out) would wrongly silence
[in, p) on later loop passes. On a loop pass that will record [in, p) too,
the mask may cover it only once that pass's span covers it.

### 5.2 Step 2 (2026-10-17, uncommitted)

Where it lives: `MidiTimeline::preRollStartTick` (core/miditimeline.*); the
pre-roll start in `MainWindow::beginRecording` / `startPlayback(…, startTick)`
and the setting (`punchPreRollBars`, `AudioSettingsPage::kPunchPreRollBars*`,
`punchPreRollBarsChanged`, combo `settingsPunchPreRoll` under Count-in); the
mask as `NoteMask` in `TimelinePlayer::render` / `dispatchDue` (core/
timelineplayer.*) driven by `AudioEngine::setPunchMask` / `clearPunchMask`
(audio/audioengine.*); the dimming as `RecordingPreview::silenced`
(ui/songview.*), filled by `syncRecordingPreview`.

Judgment calls:

- **Pre-roll from Stopped only.** Record while Paused in punch mode resumes
  where it paused (no pre-roll, no count-in), like any take from a pause; the
  take arms as P10 says (records only in the window). Record while playing is
  unchanged. The count-in is never used in punch mode (any start), whatever
  its setting; the metronome clicks during the pre-roll if it is on.
- **Bars** = `bars × beatsPerBar × beatTicks` of the meter segment in force
  at punch-in (a meter change exactly at punch-in counts), subtracted in
  ticks, clamped at 0; meters between are ignored, as P4 says. A pre-roll
  start at/after the loop end wraps like any start.
- **Edit cursor untouched.** The pre-roll start is a one-off seek;
  `startPlayback` takes a start tick that overrides the cursor only from
  Stopped. Stop returns to the edit cursor as always (not the pre-roll start).
- **The mask is player-side, not a rebuilt timeline.** A masked timeline
  would need a rebuild (`updateTimeline` = all-notes-off) at take start and
  at the end; instead `TimelinePlayer` skips the target track's note-ons with
  tick in [in, out) while a `NoteMask` is passed (the live render only;
  offline renders pass none, one pointer test per event), so nothing restarts.
  The UI publishes {generation, track, in, out} before `armCapture`; the
  audio thread loads it **at the take's Begin** (syncCapture), so it only
  ever applies from where recording started.
- **Equality with the commit's replaced spans.** `punchedSpans` = the take's
  recorded segments ∩ [in, out). The player only moves forward within a
  segment, so masking note-ons in [in, out) from the segment's start on
  silences exactly [max(in, segment start), out) of each pass — the spans
  the commit replaces (a later pass starting at the loop start ≤ in covers
  [in, …) as it plays; a stop inside ends both at the stop). Deviation: the
  commit rounds span edges to ticks (`tickAt`), the mask works on the
  playback position, so a take armed inside the window can differ by ≤ 1
  tick at its Begin (a note on that tick may play and then be deleted, or
  vice versa).
- **Cut.** Song notes the target track still sounds when the pass reaches
  max(in, Begin) are released there (once per pass; re-armed when the
  player is before in again — a wrap or a seek), with their gate-carried
  releases dropped: the commit truncates them there. Only notes keyed on
  before out (a TIE or gate-carried note from after out, carried across a
  loop wrap, isn't the commit's and keeps sounding), and never a key the
  keyboard holds on the track (review fixes M1/M2 below). A render stops at
  in's sample so the cut is sample-exact. A song note-off on the masked
  track for a key the player hasn't keyed on is dropped too, so a silenced
  (or cut) note's note-off never kills the keyboard's own note on that key.
- **Lifetime.** Kept after the disarm until the take's commit lands (as
  revised by review fix L2 below): narrowed at the take's end to what the
  commit replaces, kept through other rebuilds while the commit waits,
  dropped by the commit's own rebuild, or at once when the commit writes
  nothing; also dropped by a Stop/Pause transition, a seek while not
  capturing, another arm, followTakeTrack's drop, `loadSong`/`unloadSong`.
- **Mid-take edit**: `updateTimeline` while capturing keeps the mask and
  recomputes its samples from its ticks (a tempo edit moves them).
- **Track move**: `followLiveTrack` moves the mask's track with the live
  target and the latch in `updateTimeline`; `followTakeTrack` also updates
  the published track for a Begin that hasn't happened yet.
- **Dimming** (as revised by review fix L1): `RecordingPreview::silenced`
  = `RecordingTake::silencedTickSpans`, what the engine is silencing; the
  roll paints the take track's notes starting there at
  fill alpha 70 (`kPunchSilencedAlpha`), or half the ghost alpha when the
  track isn't selected. Notes sounding *into* the range (cut, truncated at
  commit) aren't dimmed. After the take ends, the frozen ghosts carry the
  replaced span while the commit waits (L2); cleared when it lands and
  when the take's track is deleted.
- Settings: the combo sits under "Count-in when recording:" as "Punch
  pre-roll:", Restore Defaults resets it to 1 bar; the count-in tooltip now
  says it isn't used for a punch recording.
- Step-1 checks: `checkRecordingPunch` pins the pre-roll to 1 bar and its
  two-loop-pass case now arms while playing (from Stopped it would pre-roll
  before punch-in instead of starting at beat 12).

Checks: metrocheck `checkPreRoll` (Off, 1 and 2 bars in 4/4, 6/8 and on/
around the 3/4 change of the grid song, an exact-bar 0 and the clamp at 0).
reccheck `MainWindow::checkRecordingPunchMask` (device held, two
DirectSound tracks soloed, count-in set to 2 bars throughout): the Settings
combo applies and persists; Record from Stopped starts (seek target and the
take's Begin) at in / in − 1 bar / in − 2 bars with no count-in, clamps at
0 (punch-in at bar 1.5), measures a 3/4 set at punch-in as 3 beats, never
moves the edit cursor and Stop goes back to it; from a pause it resumes in
place. Through a take: the track's note before punch-in sounds and the
mask is set; inside the range the target track sounds nothing (a note
starting inside silent, one sounding into it cut) while the other track's
note plays; `silenced` == [in, out); the roll's pixel at the silenced note
changes with the dimming; the keyboard's key outlives a silenced note's
note-off; a mid-take edit keeps the mask (a later note in range still
silent); the note at punch-out plays; Stop clears mask and dimming and the
range plays again; Record again and Pause inside the range each clear the
mask and the rest of the range plays (at once / on resume); a tab switch
mid-take clears it and the range plays after coming back; deleting the
track clears it; a track move moves it (the moved track silent in range,
soloed alone); a take armed inside the range on a [8, 16)-beat loop: the
note before the Begin plays, the Begin cuts the track, `silenced` ==
[begin, out), the rest of the pass silent, after the wrap the note before
punch-in plays and from punch-in the track is silent again, `silenced` ==
[in, out).

Mutation tests (by hand via a scripted apply, rebuilt, metrocheck +
reccheck run, restored from backup files): caught — note-on mask off
(6 reccheck fails), cut at range start removed (3), masked note-off skip
removed (1), cut not re-armed on a new pass (1), mask not loaded at Begin
(11), rebuild always drops it (4), mask not following a track move (2), UI
never sets it (11), every take-end clear removed (7; with the
loadSong/unloadSong clears too, 9), the rebuild drop and UI clears removed
together (3), pre-roll ignoring bars (metrocheck 10, reccheck 15), meter at
tick 0 instead of punch-in (6, 2), no clamp (2, 1), count-in used in punch
mode (7), start at the edit cursor (7), pre-roll setting not persisted (1),
combo not emitting (2), dimming not painted (1), `silenced` ignoring the
Begin (1), `silenced` never set (4). Not caught, by design (redundant
nets): the audio-side drops alone (the UI clear remains), the UI clears
alone or with the Stop/Pause transition drops (the commit's rebuild drops
it), the loadSong clear alone (the UI clear). The render stopping at in's
sample is now checked and mutation-tested (review checks below). Not tested directly: project switch, quit and song-end auto-stop
(they end the take through `finishRecording` / the Stopped transition /
`unloadSong`, all covered above).

Sweeps: `tools/run_checks.sh` normal and ASAN, ~/pokeemerald +
~/teamaqua-pokeemerald: PASS (36/36 each). build, build-asan, build-dummy,
build-nomidi, build-off: clean. clang-format 22 on touched files.

#### Step 2 review fixes (2026-10-17, uncommitted)

- **M1 — held key across punch-in.** The engine stops channels by
  track+key, so the cut's `m4a_engine_note_off` also silenced the
  keyboard's own note on that key (held across in, or held at a Begin
  inside the window). `NoteMask::held` (128 bits) is filled before every
  render/settleWrap from the held entries of `m_timedActive` on the mask's
  track (`AudioEngine::fillPunchHeld`); the cut skips the note-off for
  those keys and just forgets the song's note (no longer keyed on, its
  gate-carried release dropped). Chosen over re-striking, which would
  re-attack at in a note the player didn't play. The song's note on that
  key sounds on under the held one until the keyboard's release
  (`releaseActive` stops both); its own note-off is dropped as before
  (not keyed on).
- **M2 — only what the commit truncates.** The cut releases only keys with
  `m_keyedOnTick < outTick`, and drops only those keys' gate-carried
  releases: a TIE or gate-carried note starting after out, carried into
  the next pass by a loop wrap, keeps sounding (the commit doesn't touch
  it), and its release still fires.
- **L1 — dimming = what is silenced and replaced.**
  `RecordingTake::silencedTickSpans` = `punchedSpans` (what the commit
  replaces so far) ∪ the current pass's [max(in, segment start), out);
  `RecordingPreview::silenced` is now a sorted span list. The segment-0
  special case is gone: a loop start inside the window dims from the loop
  start on later passes, and a seek forward inside the window leaves the
  skipped stretch undimmed.
- **L2 — deferred commit.** `clearPunchMask(generation)` is
  generation-guarded (compare-exchange gen → 0 inside the seqlock write):
  a late clear never wipes a newer take's request. Beyond the review's
  suggestion: keeping the whole [in, out) mask through a deferred commit
  would silence the rest of the window after a take ended inside it (the
  commit doesn't replace that), so at the take's end (`finishRecording`)
  the mask is narrowed (`narrowPunchMask`) to the replaced span and the
  frozen ghosts carry it as `silenced`. Several replaced spans (rare:
  armed inside and wrapped, or a seek inside) clear it at once instead
  (the old part may sound until the commit lands, as before); no spans
  clear it at once. `commitTake` leaves the mask alone while it defers;
  landing, it calls `endPunchMaskAtRebuild(take)` before its edits, so
  the edit's own rebuild drops the mask together with the old notes (no
  moment of old part, and no moment of the new part masked), and
  `clearPunchMask(take)` after (which drops it at once when nothing was
  written: a locked document, a vanished track, a no-op replace).
  `updateTimeline` while not capturing keeps a mask whose generation is
  still published and not ending (the open group's own edits rebuild
  first). Left as is: a Stop/Pause, or a seek, while the commit waits
  still drops the mask (ordinary playback from there).
- **L3 — torn reads.** The request is a seqlock: `m_punchSeq` (odd while
  the UI writes), the fields atomics (generation, track, in, out,
  ending), written only on the UI thread (`writePunchRequest`); the audio
  thread reads a snapshot and re-checks the sequence (`readPunchRequest`,
  16 tries). A Begin whose read keeps tearing loads at the next callback
  (`m_punchLoadPending`). `applyPunchMask` re-reads only when the
  sequence moved: a cleared or other generation drops the mask, the same
  one applies a narrowed range (the track follows the take via
  `followLiveTrack`, so the request's track is read only at a Begin).
- **Simplification.** The eight reset pairs are `dropPunchMask()`.

Checks added (reccheck `checkRecordingPunchMask`, after the loop case):
a key held across punch-in on the key of the old part's note sounding
into the range (41) keeps sounding past in and past the song note's
note-off, and stops with the release — from Stopped and armed inside the
range (M1); on an [8, 16)-beat loop a TIE (50) and a gate-carried note
(52) from after punch-out survive punch-in on the next pass, and 52's
carried release still fires (M2); a take armed inside the range on an
[11, 20)-beat loop dims [begin, out), then [11, out) after the wrap; a
seek forward inside the range dims [in, left) and [landing, out) (L1);
with an edit group open (and an edit in it, a rebuild), a take ended past
punch-out on a loop keeps the next pass's range silent and dimmed until
the commit lands, then the mask is gone; a take ended inside the range
keeps a narrowed mask and the rest of the range plays; a locked
document's failed commit drops the mask at once (the next pass plays the
old part); a newer take armed while the older commit waits keeps its mask
when that commit lands (L2); Record from a pause inside the range masks
and dims from the Begin and replaces from it; the release at punch-in is
sample-exact (punch-in a few ticks off any event, the player pumped to
three frames before inSample with 41 sounding, then one 4-frame period
releases it). metrocheck `checkPreRoll`: mid-bar punch-ins in 4/4, in 6/8
two bars back into 4/4, and in 3/4 two bars back across 6/8 into 4/4.

Mutation tests (scripted apply, rebuilt, reccheck and/or metrocheck run,
restored from backup files): caught — held keys not skipped (4 fails),
held mask never filled (4), no keyed-on-tick test (2), all of the track's
pending offs dropped (1), dimming from in (5), dimming without the
replaced spans (1), no narrowing (2), narrowing skipped with the mask kept
(1), rebuild always drops (1), no `endPunchMaskAtRebuild` (2), no landing
clear (1, the locked case), unguarded clear (1), render not stopping at
inSample (1), pre-roll meter from tick 0 (metrocheck 8 incl. both new
cross-meter cases, reccheck 2). Not mutation-tested: the seqlock retry
and the pending load (a torn read needs a UI write racing the audio
read; not reproducible deterministically in the harness); Record from a
pause has no pause-specific code (its check fails under the dimming
mutation).

Sweeps: `tools/run_checks.sh` normal and ASAN, ~/pokeemerald +
~/teamaqua-pokeemerald: PASS (36/36 each). build, build-asan,
build-dummy, build-nomidi, build-off: clean. clang-format 22 on touched
files.

### 5.3 Step 3 (2026-10-17, uncommitted)

Where it lives: `SurfaceEvent::Punch` and `SurfaceEvent::note` (the
button's note number, set for every button), `mcu::kPunchDrop` (0x57) /
`kPunchReplace` (0x58) (audio/mcu.*); the once-per-press dispatch in
`MainWindow::onSurfaceEvent` with `m_surfacePunchHeld` (bit per note,
`surfacePunchBit`), reset in the `portsClosed` handler (mainwindow.*); the
Settings → MIDI buttons line (ui/midisettingspage.cpp); CHANGELOG; manual
outlines in docsrc/manual (midi-keyboard.md "Punch recording" section +
Transport buttons, main-window.md transport bar, settings.md Audio).

Judgment calls:

- **Held set, not a count.** The dispatch keeps one bit per Punch note:
  a press toggles only when no bit was set, a release clears its own bit.
  A repeated press of an already-held note (a lost release) does nothing
  until the bits clear — devices without releases are unsupported (§3),
  and the reset below keeps it from lasting past a reconnect.
- **Counted under a modal dialog.** A Punch press under a modal dialog
  doesn't toggle (J5, as every button) but is still marked held, so its
  partner arriving after the dialog closes doesn't toggle either.
  Releases always clear, as Rewind/FF's do.
- **Reset where Rewind/FF's hold is**: the `portsClosed` handler (a port
  unplugged/disabled/reopened, or an open port's role change), *after*
  draining what the port queued (a queued press still toggles, then
  nothing is held). Not on a song switch: Punch is app-wide, unlike a
  Rewind/FF hold.
- **Mid-take**: the surface triggers the toolbar action, so a take keeps
  the mode it latched at Record (no new code).
- **Scripting docs**: API.md / gen_scripting_docs.py don't list surface
  button kinds (only port roles), so nothing to change or regenerate.
- **CHANGELOG**: one "Added punch recording…" entry plus "Punch" in the
  existing transport-buttons list. Manual: TODO outlines only, no new page
  (mkdocs nav unchanged); `mkdocs build --strict` PASS.

Checks (midicheck): `checkMcuDecoder` maps 0x57/0x58 to Punch (removed
from the unmapped list) and checks every mapped button's event carries its
note number. `checkSurfaceTransport`: both notes in one press toggle once
(and the view's punch mode); releases don't toggle; a second press (partner
first, released in the other order) toggles back; a partner re-press while
the other is held is ignored; single-note devices (only 0x57, only 0x58)
toggle per press, a velocity-0 note on releases; a port closing mid-press
lets the next press toggle; a press queued before the port closes is
handled (toggles) and leaves nothing held; Punch under a modal dialog does
nothing; Punch mid-take toggles both ways with the take running and its
mode unchanged. Settings page buttons line text.

Mutation tests (scripted apply, rebuilt with exit-code check, midicheck
run, restored from backup files): all 9 caught — 0x58 not decoded (8
fails), note not set (22), no held test (13), releases not tracked (9),
no reset on portsClosed (2), reset before the drain (1), one bit for both
notes (11), Punch not dispatched (7), Settings line unchanged (1). Not
mutation-tested: the held-under-modal marking (a partner arriving after a
dialog closes mid-press isn't drivable without timing a real press).

Sweeps: `tools/run_checks.sh` normal and ASAN, ~/pokeemerald +
~/teamaqua-pokeemerald: PASS (all harnesses; 36/36 ok on ASAN). build, build-asan, build-dummy,
build-nomidi, build-off: clean. clang-format 22 on touched files.

**Final review fixes (2026-10-17, uncommitted).** A final Opus review of
the branch found no high/medium bugs; applied:

- **L1 — say why.** A Record press from the control surface, or the roll's
  R key, while Record is unavailable now shows `recordUnavailableReason()`
  in the status bar for 6 s, as `beginRecording` does
  (`MainWindow::explainRecordUnavailable`); both used to do nothing. The R
  key still does nothing at all when Record is hidden (no keyboard port:
  no button, no key, as before); the surface's Record says "Connect a MIDI
  keyboard to record." then (its button has no tooltip).
- **L2 — no tint for an overdub.** `SongView::setOverdubLatch` (set by
  `beginRecording` for a non-punch take; `clearPunchLatch` ends either
  latch, on every take-ending path and a song swap): while an overdub
  records into the view, `punchRange()` is empty even if Punch is turned on
  mid-take; a punch take keeps showing its latch.
- **L3 — Punch hidden with Record.** `syncRecordAction` shows Punch exactly
  when Record is shown: a keyboard port open, or a take recording (one
  whose keyboard went away keeps both). A hidden QAction is disabled (Qt),
  so its `transport.punch` shortcut is off and — mirroring Record, whose
  surface press with no keyboard does nothing — the surface's Punch button
  doesn't toggle it then, and says why (the same "Connect a MIDI keyboard
  to record."). An explicit `setEnabled` was tried and dropped: a mutation
  removing it went uncaught because hiding already disables.
- **L4** — CHANGELOG "Changed": switching tracks no longer clears the time
  selection (a selection of a track's automation lanes still is).
- **L5** — midi-keyboard.md outline bullets (TODO only): Record while
  playing inside the range replaces from where recording started; Record
  from Pause resumes in place with no pre-roll; a selection on a track's
  automation lanes is still cleared by a track switch (the outline bullet
  that said switching keeps the selection is corrected); keys held across
  punch-in keep sounding, only the old part's note is cut; with Loop on, a
  pre-roll start or range past the loop end is never reached ("Nothing was
  recorded."); Punch shows only with a keyboard. `mkdocs build --strict`
  PASS.
- **L6 — no code change; verified and pinned.** A note's and a lane
  move's latency-shifted sample is already clamped to its segment's start
  (`floorSample`, = the take's Begin for a take armed or resumed inside the
  window, a loop start or seek landing for later segments) *before* the tick
  conversion, and `punchedSpans` starts each span at `tickAt` of that same
  sample; `tickAt` is monotonic, so a note or move can never land in
  [in, p) for a take that began at p: it is clamped to p (the "clamp to
  span start" rule), on the same footing as lane moves. A second clamp in
  tick space would be dead code (nothing could make it fire, so nothing
  could test it). Pinned instead by a new `checkPunchTakeRules` case.

Checks added: midicheck `checkSurfaceTransport` — surface Record with no
keyboard port and with Punch on and no selection each start nothing and
show the reason; surface Punch with no keyboard port doesn't toggle and
says why (the Punch block now opens a keyboard port, as does the modal
case, which also checks Punch is enabled there so only the dialog keeps it
out). reccheck — Punch hidden (and disabled) with no keyboard, shown with
one, kept mid-take when the keyboard goes away, hidden after (L3); R with
Record shown but unavailable says why, R with no keyboard stays quiet
(L1); an overdub with Punch turned on mid-take has no punch range or ruler
rect, and after it the selection tints again (L2); `checkPunchTakeRules`:
a take armed inside the window at 55 with a 5-tick offset replaces
[55, 80), and a note and a MOD move shifted to 52/51 land at 55 (L6).

Mutation tests (scripted apply, rebuilt with exit-code check, harness run,
restored from backup files): caught — surface Record silent when
unavailable (midicheck 2 fails), surface Punch silent when disabled (1),
R key silent (reccheck 1), `punchRange` ignoring the overdub latch (1),
overdub latch never set (1), `clearPunchLatch` keeping the overdub latch
(4), Punch visibility not synced (reccheck 2, midicheck 1), the note floor
skipped for punch takes (1), the lane floor skipped for punch takes (1).
Not caught, and removed: an explicit `setEnabled` on Punch (redundant with
hiding).

Sweeps: `tools/run_checks.sh` normal and ASAN, ~/pokeemerald +
~/teamaqua-pokeemerald: PASS (36/36 each). build, build-asan, build-dummy,
build-nomidi, build-off: clean. clang-format 22 on touched files.

### 5.4 Settings merged (2026-10-17, uncommitted)

User decision 2026-10-17: one setting instead of two. The separate "Punch
pre-roll" setting is gone (combo `settingsPunchPreRoll`,
`AudioSettingsPage::kPunchPreRollBars*`, `punchPreRollBars()` /
`setPunchPreRollBars` / `punchPreRollBarsChanged`, QSettings
`punchPreRollBars`, `MainWindow::m_punchPreRollBars`); the old stored key is
no longer read and not migrated. The Count-in setting (`countInBars`) does
both: with Punch off it is Record's count-in as before; in punch mode its
bars are the pre-roll (`preRollStartTick(in, m_countInBars)`, same
computation), with no clicks. The combo keeps its short label "Count-in when
recording:"; its tooltip says it is the pre-roll for a punch recording.
§5.2's notes on the setting (combo, Restore Defaults, "count-in never used
in punch mode whatever its setting") are superseded by this.

Checks: `checkRecordingPunchMask` no longer pins a count-in of 2 alongside a
separate pre-roll: Settings has no `settingsPunchPreRoll` combo, the
Count-in combo sets the bars and persists `countInBars`, and Off / 1 / 2
bars of count-in start the punch take at in / in − 1 bar / in − 2 bars with
no count-in clicks. `checkRecordingPunch` sets a 1-bar count-in for its
pre-rolled takes (its two overdubs from Stopped now count in a bar first and
still pass as written).

Mutation tests (restored from backup files): caught — pre-roll fixed at 1
bar instead of the count-in (reccheck 4 fails), count-in clicks in punch
mode (7), the Count-in combo not emitting (3).
