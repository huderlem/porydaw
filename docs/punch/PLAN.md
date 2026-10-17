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
| P4 | Pre-roll | Record while stopped starts playback **pre-roll** bars before punch-in with the song audible. New setting in Settings → Audio beside Count-in: Punch pre-roll Off / 1 bar / 2 bars, default 1 bar, clamped at tick 0, bars measured with the meter in force at punch-in. The count-in is NOT used in punch mode. Metronome clicks if it's on. |
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

## 5. Step 1 — as built (2026-10-17, uncommitted)

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
