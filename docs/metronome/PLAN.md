# Metronome and count-in

## 0. Problem and goal

MIDI keyboard recording landed (docs/midi-input/PLAN.md), but D10 there left
out a metronome and count-in: you start a take from the edit cursor with
nothing to keep time against. This adds a click that follows the song's tempo
map and meter, and a count-in before Record.

## 1. Decisions (confirmed by the user 2026-10-15)

| # | Question | Decision |
|---|---|---|
| M1 | Sound source | ✅ **Synthesized click mixed after the GBA engine**: a short sine blip with a fast exponential decay, higher pitch on the downbeat. No GBA channels, no song reverb, no sample assets. |
| M2 | What it clicks | ✅ **Every beat of the meter in effect**, on the grid `MidiTimeline::timeSigSegments()` describes (the ruler's and beat companion's): 6/8 clicks eighths, 4/4 quarters. The first beat of every bar is accented. Tempo and meter changes are followed. |
| M3 | When it sounds | ✅ **Whenever playback runs and the metronome is on**, recording or not. No "only while recording" mode. |
| M4 | Toggle | ✅ **Metronome** button on the transport toolbar, keymap command `transport.metronome` (default **`C`**). No menu entry: porydaw has no Transport menu, and Loop and Follow Playhead live on the toolbar only (checked 2026-10-15). On/off persists app-wide in QSettings, like Follow Playhead. |
| M5 | Count-in | ✅ **Record from stopped only** (not Play, not punch-in while playing). Setting *Count-in: Off / 1 bar / 2 bars*, default 1 bar. Clicks only, at the tempo and meter at the edit cursor; then the song starts at the cursor, recording. It sounds even with the metronome off. |
| M6 | Notes played during count-in | ✅ Heard (live play). Recorded only if **still held when the song starts**, and then starting **at the cursor**. Notes released before the start are dropped. |
| M7 | Volume | ✅ **Settings → Audio: Metronome volume** 0–100%, independent of Output level but scaled by it. |
| M8 | Exclusions | ✅ Not in **WAV export** (offline path, automatic), not in the **audio tap** (meters/plugins hear only the song), not in **polyphony stats**. |
| M9 | Edge cases | ✅ Playback starting exactly on a beat clicks that beat. Stop cuts a ringing click. A loop wrap clicks only the grid beats inside the loop: no click at the loop start unless it is a beat. |
| M10 | Control surface | ✅ MCU **Click** button (note `0x59`) toggles the metronome. **LED feedback deferred**: porydaw has no MIDI output at all (no `RtMidiOut` anywhere), so lighting it is a separate feature. |
| M11 | Scripting | ✅ **Deferred.** No `porydaw.transport.metronome` in v1. |
| M12 | Pre-roll (play the song from N bars early, punch in at the cursor) | ✅ **Deferred.** |

To change a decision later, update this table and every step it affects
**before** starting the next step.

## 2. Design stances

1. **The click grid is precomputed on the UI thread.** `timeSigSegments()`
   returns a fresh `std::vector`, so the audio thread can't call it. When a
   timeline is built, it also builds an immutable `clickGrid`: one entry per
   piece of constant tempo *and* meter (a split at every tempo point and
   every meter change), each holding its start tick, start sample, samples
   per tick, beat ticks, beats per bar and the beat index (within the bar)
   of its first beat line. The audio thread reads it like `events`, lock-free
   because the timeline is immutable once published (`updateTimeline` swaps
   it with the device stopped). The last piece runs forever, so the click
   continues past the song end (the 3 s tail, a take past the last note).
2. **A pure scheduler.** `ClickGrid::clicksIn(fromSample, toSample,
   out[], cap)` lists the beats in `[from, to)` as (sample, accent). It's pure
   so `--metrocheck` can compare it with the `samplePos` of notes placed on
   every `stepGridLines(…, bars=false)` line, across tempo changes, meter
   changes and odd tick rates. Beat samples are computed bit-identically to
   `build()`'s file-local `tickToSample` (which positions events), so a click
   and a note on the same tick land on the same sample.
3. **Loop wraps from what the player reports.** `process()` notes
   `m_player.position()` and `wrapCount()` around each `render()` chunk. No
   wrap: the chunk covered `[p0, p1)`. With wraps: `[p0, loopEnd)`, then
   whole loops `[loopStart, loopEnd)` for any extra wraps, then
   `[loopStart, p1)`, with frame offsets accumulating across them. The
   player itself doesn't change.
4. **Mixed after the tap.** Clicks found during the chunk loop go into a
   small fixed array of (frame offset in the callback, accent), at most 64
   per callback; overflow is dropped. After `m_tap.write(...)` a
   `ClickVoice` (allocation-free, at most two ringing clicks) renders them
   into `interleavedOut`, scaled by output gain × metronome volume. The tap,
   the polyphony counts and WAV export never see them (M8).
5. **Engine state is hot atomics.** `setMetronomeEnabled(bool)` and
   `setMetronomeVolume(float)`, the same pattern as `m_loopEnabled` and
   `m_outputGain`. Stopping (or a seek) cuts the ringing click (M9). Toggling
   mid-play takes effect from the next callback; an already ringing click
   finishes.
6. **Count-in is a new engine phase, not a UI timer** (step 2). A UI timer
   can't start the song sample-exactly on the beat. The engine gets
   `countIn(cursorSample, lengthSamples, grid)`: the transport stays out of
   Playing (the song doesn't render, the playhead stays on the cursor), and
   a count-in clock runs from `cursor − length` to the cursor, clicking the
   cursor's meter grid **extended backward** from the cursor's piece. So the
   beat carries straight into the song even when the cursor isn't on a bar
   line, and a cursor near tick 0 still gets full bars. At the end the engine
   switches itself to Playing **at that sample within the callback** and
   renders the rest of the callback from the cursor. Stop, Space or Record
   during a count-in cancels it and the take (an empty take commits nothing).
   With the loop on and the cursor at or past the loop end, the song really
   starts at the loop start (the player wraps there at once), so the
   count-in is computed for, and holds at, the loop start instead. A seek
   during the count-in moves where the song will start; the count-in runs
   its length regardless.
7. **M6 lives where held keys live.** The engine already knows the live held
   keys (`liveHeldKeys`). During a count-in, note captures are suppressed.
   At the switch to Playing, a note-on is pushed at the cursor's sample for
   every key still held, with its original velocity, flagged as carried
   over. Releases after that capture normally, except that a carried key
   whose release, moved back by the recording offset, lands at or before the
   start was heard released during the count-in and is dropped too.

## 3. Facts the steps rely on

- `TimelinePlayer` dispatches events by `samplePos` computed by `build()`'s
  file-local `tickToSample` (`miditimeline.cpp:52`). `sampleForTick`
  (`miditimeline.cpp:427`) rounds from the tempo map's rounded points
  instead and can be 1 sample off, so the click grid copies `tickToSample`'s arithmetic, not
  `sampleForTick`'s.
- `MidiTimeline::timeSigSegments()` (`miditimeline.h:129`) always has the
  4/4 segment at tick 0; `beatTicks = tpb*4 >> denomPow2`. `stepGridLines`
  restarts the grid at every meter change.
- `AudioEngine::process` (`audioengine.cpp:1267`) renders in chunks of at
  most `m_bufCapacity`. It sums the main and preview engines into
  `interleavedOut` × gain, then calls `m_tap.write` (`:1333`), then handles
  capture boundaries per wrap (`:1336`). WAV export (`wavexport.cpp`) uses
  its own `TimelinePlayer` and never calls `process`.
- `TimelinePlayer::position()` / `wrapCount()` (`timelineplayer.h:93-96`).
  A render can wrap more than once only when the loop is shorter than the
  chunk.
- Record from stopped: `MainWindow::beginRecording` (`mainwindow.cpp:5868`)
  arms capture, then calls `startPlayback()` from the edit cursor.
  `Transport` is `Stopped / Paused / Playing` (`audioengine.h:44`).
- Control surface: `mcu::decode` (`mcu.h`) and
  `MainWindow`'s `SurfaceEvent` switch (`mainwindow.cpp:~5700`). The MCU
  Click button is note `0x59`.
- Bare `C` is unbound in `keymap.cpp`'s command table.
- Settings → Audio: `AudioSettingsPage` (`audiosettingspage.cpp`), groups
  "Output" and "GBA engine".

## 4. Steps

For every step: implement → Opus code review (subagent) → Opus fixes the
worthwhile findings (subagent) → build all configs + sweep (normal + ASAN
`tools/run_checks.sh`) → commit (no Claude attribution) → **check in with
the user for a Windows test** before the next step.

### Step 1: Metronome click (engine, toggle, volume)

- `MidiTimeline`: `ClickPiece` + `clickGrid` built in `build()` (stance 1).
- `src/audio/metronome.{h,cpp}`: `clicksIn` (stance 2), `ClickVoice`
  (sine blip: ~1.5 kHz accent / ~1 kHz beat, ~1 ms attack, ~30 ms decay,
  sample-rate aware, a fixed peak level so 100% sits about as loud as a
  full-volume song note).
- `AudioEngine`: atomics, wrap-aware scheduling in the chunk loop
  (stance 3), mixed after the tap (stance 4), cut on stop/seek.
- UI: Metronome toggle on the transport toolbar (next to Loop; a painted
  icon to match the toolbar's style), keymap command
  `transport.metronome` = `C`, QSettings `metronome` (default off).
  Settings → Audio: new "Metronome" group with *Volume* (0–100%, default
  70%, QSettings `metronomeVolume`).
- `--metrocheck` harness, added to `run_checks.sh`: the scheduler vs the
  `samplePos` of notes on the `stepGridLines` beats, on synthetic timelines
  (tempo change mid-bar, 4/4→6/8→3/4, tpb 24/48/96, a loop that isn't
  beat-aligned, a loop shorter than a chunk); the engine through `holdDeviceForTest` /
  `pumpForTest`: clicks present in the output at the expected frames and
  **absent from the tap**, accent on bar starts, silence when off, a cut on
  stop. A negative test for each assertion.

### Step 2: Count-in for Record

- Settings → Audio "Metronome" group: *Count-in when recording: Off / 1 bar
  / 2 bars* (QSettings `countInBars`, default 1).
- `AudioEngine` count-in phase (stance 6) and the held-note carry-over
  (stance 7). The capture clock, playhead and transport telemetry report
  the cursor during the count-in.
- `beginRecording`: from stopped with count-in on, starts the count-in
  instead of `startPlayback()`. Status bar "Counting in…" until the song
  starts, then "Recording into …". Stop/Space/Record cancel (stance 6). The
  Record button shows recording from the press. Punch-in while playing is
  unchanged.
- `--reccheck` / `--metrocheck` additions: count-in length and click
  positions for a bar-aligned cursor, a mid-bar cursor and a cursor near
  tick 0; the song's first sample lands exactly on the cursor; a key held
  through the start records at the cursor tick; a key released during the
  count-in records nothing; cancel commits nothing.

### Step 3: MCU Click, docs, CHANGELOG, final review

- `mcu::kClick = 0x59` → `SurfaceEvent::Click` → toggles the metronome.
  `--midicheck`/surface decode test.
- CHANGELOG entry (simple). `docsrc/manual/`: a "Metronome and count-in"
  section as a TODO outline for the user's prose, in the MIDI keyboard
  page next to Recording; the Transport buttons section lists Click.
  `mkdocs build --strict`.
- Final Opus review of the whole branch, fixes, all builds + sweeps.

## 5. Progress

### Step 1 — done 2026-10-15

- `ClickGrid` (src/core/clickgrid.{h,cpp}) built in `MidiTimeline::build`;
  `ClickVoice` (src/audio/metronome.{h,cpp}); engine scheduling across
  wraps, mixed after the tap; toolbar toggle (painted icon), bare `C`,
  QSettings `metronome` / `metronomeVolume`, Settings → Audio "Metronome"
  volume (double-click resets to 70%).
- `--metrocheck` in `run_checks.sh`, every assertion negative-tested.
- Opus review: no high/medium. Fixes applied: rate-robust short-loop case
  (onset positions SKIP above ~65 kHz, count still asserted), song switch
  cuts the click, 5 ms closing fade, auto-stop keeps the callback's earlier
  clicks, more harness cases (seek cut, off mid-click, same-tick tempos,
  cap truncation, far into the last meter, song switch, auto-stop), plan
  text fixes, style nits. Not taken: computing event samples through the
  grid (structural bit-exactness), caching per-loop clicks.
- All 5 builds; normal + ASAN sweeps PASS.

### Step 2 — done 2026-10-15

- `ClickGrid::countIn` / `countInClicksIn`; `AudioEngine::playWithCountIn`
  with a seqlock handoff (state = generation + phase), count-in phase in
  `process()`, sample-exact song start mid-callback, `finishCountIn` begins
  the take and carries sounding keys (`CaptureEvent::Carried`; dropped if
  released inside the latency window). `MidiTimeline::sampleForTick` now
  uses the click grid (it could be 1 sample off build()'s event positions).
- MainWindow: Record from Stopped counts in (loop-end cursor starts from
  the loop start), "Counting in…" status swapped once the song starts (only
  if still showing), Record/Stop during the count-in cancel. Settings →
  Audio: Count-in Off / 1 / 2 bars (default 1).
- Harnesses: metrocheck count-in grid + engine cases; reccheck
  checkRecordingCountIn; midicheck and reccheck takes run with count-in
  off; `--selftest` uses default metronome settings. All negative-tested.
- Opus review: 2 medium (midicheck broken by the default count-in; loop-end
  cursor droned loop-start notes and used the wrong tempo), lows (audio
  thread could clobber a fresh request; torn request copy; latency-window
  blip; status swap over Learn prompts) — all fixed. Left for the user:
  knob/bend/voice moves during the count-in are heard but not recorded;
  pedal-held keys count as held (M6).
- All 5 builds; normal + ASAN sweeps PASS.
