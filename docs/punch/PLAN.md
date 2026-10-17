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
