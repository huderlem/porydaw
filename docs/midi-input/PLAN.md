# MIDI Input — play and record from a MIDI keyboard

Status: **decisions confirmed 2026-10-11; step 1 done (28aaf34); step 2 done (ef8a087); step 3 done 2026-10-11 (uncommitted)** (written 2026-10-11).
File/line references verified against `midi-input` at `6b2e78c`. Work the
steps **in order, one agent per step**; each step must leave the branch
green (`tools/run_checks.sh`, normal + ASAN) and ends with a progress entry
at the bottom of this file.

## 0. Problem and goal

Porydaw has no MIDI input at all: no library, no device code, nothing in
the build (`grep -ri rtmidi src CMakeLists.txt` is empty). Notes can only
be auditioned by clicking the roll's keyboard column, one at a time, and
entered with the mouse.

**Goal:**

1. **Live play.** Notes from any connected MIDI keyboard sound
   immediately, polyphonically, with the **selected track's** voice, VOL
   and PAN, whether the song is stopped or playing.
2. **Record.** A Record button captures what you play into the selected
   track while the song plays. The take becomes **one undo step**.

Out of scope for v1 (see §1 for what's deferred and why): MIDI output and
thru, recording pitch bend, mod wheel or other CCs, a metronome or
count-in, step input, and MIDI learn.

## 1. Decisions (all confirmed by the user 2026-10-11)

| # | Question | Proposed default |
|---|---|---|
| D1 | MIDI library | ✅ **Vendor RtMidi 6.0.0** (two files plus a MIT-style license) into `external/rtmidi/`. It covers WinMM, CoreMIDI and ALSA, and poryaaaa's standalone app already depends on it upstream through clap-wrapper. Qt and miniaudio have no MIDI support. |
| D2 | Which input reaches which track | ✅ **All enabled ports, all channels (omni), go to the selected track of the active tab.** Settings → MIDI lists the ports with an on/off toggle each; new ports are on by default. The MIDI channel is ignored. |
| D3 | Where live notes sound | ✅ **On the main engine, on the selected track itself**, through the existing polyphonic preview machinery: the timed-preview path the rubber-band sweep already uses, extended with "held until released" entries. Notes get the track's real voice, VOL and PAN at the playhead and share the GBA channel pool with the song. See §2 stance 2 for the edge cases and the fallback. |
| D4 | What a take records | ✅ **Notes and velocity only.** The sustain pedal is honoured both live and in the take: it extends the recorded note lengths, because mid2agb has no sustain. Pitch bend and the mod wheel are **heard but not recorded**. They act on the whole target track, song notes included, which is the desired behaviour (user, 2026-10-11). Recording them as lane automation is deferred. |
| D5 | Record transport model | ✅ **Record** toggle on the transport toolbar plus a keymap command `transport.record`. Its default key is picked in step 3 from what's free; the proposal is `R`. While stopped, Record starts playback from the edit cursor and records. While playing, Record punches in at once. Record again punches out, commits and keeps playing. Stop or Space commits. |
| D6 | When notes enter the document | ✅ **Once, at punch-out or stop.** Every document edit rebuilds the timeline and restarts the audio device with all-notes-off (`AudioEngine::updateTimeline`, `audioengine.cpp:208-238`), so writing notes during the take would chop the song and your held notes. While recording, the roll shows the take as ghost notes (step 4). |
| D7 | Overlap with existing notes | ✅ **Overdub.** Take notes are merged in with `SongDocument::addNotes`, so `resolveNoteOverlaps` (`songdocument.cpp:1009`) lets the new note win. "Replace" mode, which clears the punched range first, is deferred. |
| D8 | Loop | ✅ **Keep recording across loop passes and merge them all.** Later passes win overlaps through D7. A note held across the wrap ends at the loop end. |
| D9 | Quantize | ✅ **None at input.** Times are rounded to the nearest tick and nothing else. Step 5 adds an optional "Quantize note starts to grid" toggle, off by default, that uses the roll's `snapTick`. |
| D10 | Metronome or count-in | ✅ **Not in v1.** Porydaw has neither, and it's a separate feature. You start recording from the edit cursor; leave yourself a bar. |
| D11 | Input while Porydaw isn't the focused window | ✅ **Keep sounding and recording**, as DAWs do. |
| D12 | Velocity | ✅ **Pass the raw value through** to the engine and the note. Settings → MIDI gets an optional *Fixed velocity* (off, or 1-127). |
| D13 | Scripting `porydaw.midi` | ✅ **Yes, as step 6**, after the core works. The namespace stays reserved (`docs/scripting/PLAN.md:529`, `:544-546`). |

To change a decision later, update this table and every step it affects
**before** starting the next step.

## 2. Design stances

1. **Three threads, two lock-free rings, no UI hop on the sound path.**
   - **MIDI threads** are RtMidi's callbacks, one per open port. They parse
     messages and push `{status, d1, d2}` into the **input ring**, which is
     multi-producer through a short producer-side mutex.
   - The **audio thread** drains the input ring at the top of
     `AudioEngine::process` (`audioengine.cpp:681`), next to the other
     `apply*` calls.
     - It plays each message through the same active-note table as the
       timed previews (§2 stance 2). The input ring is separate from the
       timed ring, so the timed ring keeps its single UI producer.
     - If a take is armed, it stamps the message with the player's sample
       position and pushes it into the **capture ring**. The capture ring is
       SPSC from the audio thread to the UI.
   - The **UI thread** drains the capture ring on the existing 17 ms
     playhead timer (`mainwindow.cpp:486-495`). Latency is therefore the
     audio period, not the period plus a UI tick, and the timestamps match
     what the player was rendering when the note sounded.
2. **Live notes reuse the band-sweep's polyphonic path (D3).**
   - The rubber-band sweep already auditions chords: `previewNoteTimed`
     (`audioengine.h:128`, `applyTimedPreviews` at `audioengine.cpp:569-631`)
     stacks up to 24 notes on the main engine. It handles retriggers and
     steals the oldest note, and a velocity-0 entry releases a key early.
     A keyboard is the same thing without a known length.
   - So the timed ring gains a **held** flag: no countdown, and the note
     lasts until its velocity-0 release. The single-note `previewNote`
     (`:532-557`) is not used.
   - The voice, VOL and PAN come from the main engine's track state,
     already chased to the playhead (`chase`/`primeVoices`). Live notes
     therefore follow automation during playback and share the GBA's
     15 PCM + 4 CGB channels with the song, as they would on hardware.
   - **Edge cases**, all of which only matter while the song plays:
     - Every transport transition (`cutAllSound`, `:483-491`) and every
       document edit (`updateTimeline`, `:208-238`) clears the timed
       previews.
       - Keys held when you press Play or Record go silent; the next
         press sounds normally.
       - With D6 (commit at stop), no edits happen mid-take.
       - This is accepted; the fix, if wanted, is to re-sound the held set
         after the cut.
     - The engine releases notes by track and key (`m4a_engine_note_off`).
       - A song note on the same track and key releases your held key at
         its note-off.
       - Releasing your key also releases the song's note.
       - This only happens when overdubbing a pitch the track already plays
         at that moment.
   - **Fallback, if those edge cases prove annoying in the manual pass:**
     move live notes to a mirrored track on the preview engine
     (`m_previewEngine`, which transport changes don't touch). That needs a
     poryaaaa helper to copy track state across engines. The cost is that
     live notes would no longer share the channel pool with the song.
3. **The take is UI-side data until commit.** A `RecordingTake` holds the
   target track, which is **locked at punch-in**, plus finished and open
   notes in sample time.
   - Commit converts to ticks with `MidiTimeline::tickForSample`
     (`miditimeline.h:122-123`), which handles tempo changes.
   - It applies the loop rule: a note-off stamped before its note-on means
     the playhead wrapped, so the note ends at `loopEndTick`.
   - Each note lasts at least 1 tick.
   - All notes are written in one `beginEditGroup`/`addNotes`/`endEditGroup`,
     with undo text "Record %n note(s)". An empty take pushes nothing.
4. **Latency compensation is one number.** The capture stamp is the player
   position at the start of the callback. You *heard* the song about one
   output latency later (period × periods, `audioengine.h:82-83`), so you
   play late by that much.
   - Commit subtracts an offset. The default is the device's reported
     latency; Settings → MIDI can override it in ms.
   - Jitter is at most one period. That is about 10 ms with the default
     period, or 30 ms under WSL; one tick at 24 tpb and 120 bpm is
     about 21 ms.
5. **Recording respects the same edit gate as the roll.** Read-only or
   bundle sessions (`porydaw.song.readOnly`) audition but refuse Record:
   the button is disabled and the status bar says why. A draft tab records
   normally.
6. **Panic is cheap and automatic.** All live notes are released when:
   - the target track changes,
   - the tab or song switches,
   - the voicegroup is swapped (`updateVoicegroup`),
   - a device closes,
   - the app quits.

   A "MIDI: All Notes Off" command is also in the keymap.
7. **Builds without MIDI still work.** CMake option `PORYDAW_MIDI` (default
   ON) uses RtMidi's dummy API when the platform backend is missing, for
   example Linux without `libasound2-dev`. The Settings page then says "No
   MIDI support in this build". Every harness check that needs a device
   uses the test injector (step 1), never real hardware.

## 3. Facts the steps rely on

- **Selected track:**
  - Stored in `SongView::m_selectedTrack` (`songview.h:906`), read with
    `selectedTrack()` (`:261`).
  - Changes emit `selectedTrackChanged(int)` (`:817`).
  - `updateSong` resets it **without** the signal when the track becomes
    unused (`songview.cpp:8688-8698`, `:8647`). Whatever publishes the live
    target must also re-read it after `updateSong`.
- **Audition VOL and PAN at a tick:**
  - `SongView::auditionVolume`/`auditionPan` (`songview.h:366-379`).
  - With the D3 design the mirror copies the main engine's track state
    directly, which is already chased to the playhead
    (`TimelinePlayer::chase`/`primeVoices`, `timelineplayer.cpp:120-148`).
  - `primeVoices` borrows the first later program for tracks with none
    yet, so an empty-start track still has a voice.
- **Engine API:** `m4a_engine_note_on/note_off/program_change/cc/pitch_bend/all_notes_off`
  (`external/poryaaaa/plugin/m4a_engine.h:517-531`).
  - Pitch bend takes signed `bend14 >> 7`.
  - CC1 is mod depth; CC7 and CC10 are VOL and PAN for the whole track.
  - Keysplit, rhythm and PSG voices all resolve inside `m4a_engine_note_on`.
  - The engine is single-writer: the audio thread owns both engines.
- **Adding notes:** `SongDocument::addNotes(engineTrack, vector<NewNote>)`
  (`songdocument.cpp:1105`; `NewNote{tick,key,duration,velocity}` at
  `songdocument.h:182-187`).
  - It pushes one `SongEditCommand`, and resolves overlaps first.
  - It does **no** same-tick dedup within a batch, so the take must collapse
    a repeated (tick, key) itself, last one wins, as `EditApi::addNotes`
    does (`scriptapi.cpp:2687-2696`).
  - Edit groups: `beginEditGroup`/`endEditGroup` (`songdocument.h:468-469`).
- **Transport:**
  - The toolbar is built at `mainwindow.cpp:649-728`; a Record action goes
    beside Stop and Loop (`~:693-700`).
  - `startPlayback` (`:5291`) seeks to `editCursorTick`.
  - `stopPlayback` (`:5326`) seeks back to it.
  - `updateTransportActions` (`:5344`) gates the enable state.
  - Commands live in the keymap table at `keymap.cpp:90-103`, bound with
    `keys.attach(id, action)`.
- **Playhead to tick:** `SongView::setPlayheadSample` (`songview.cpp:10279`)
  calls `timeline->tickForSample`. The loop range is
  `MidiTimeline::loopStartTick/loopEndTick/hasLoop()`
  (`miditimeline.h:101-118`).
- **Settings:** `SettingsDialog::addPage` (`settingsdialog.cpp:61-73`,
  `enum Page`, ids at `:28-36`). Pages apply changes live through signals
  that are wired in `MainWindow::buildSettingsDialog`
  (`mainwindow.cpp:2992-3045`). QSettings keys are slash-grouped, as in
  `engine/maxPcmChannels`.
- **Environment:**
  - WSL2 has **no `/dev/snd`**, so no ALSA sequencer and no USB MIDI.
    Linux-side verification is harness-only, through the injector.
    Real-device checks are a **user manual pass on Windows** (WinMM).
  - `libasound2-dev` is installed locally.
  - CI (`.github/workflows/build.yml:73-74`) installs only `pulseaudio` and
    needs `libasound2-dev` added, in `release.yml` too.
  - Windows links `winmm`; macOS links `CoreMIDI`, `CoreAudio` and
    `CoreFoundation`.

## 4. Steps

### Step 1 — RtMidi vendored, `MidiInput` service, Settings → MIDI page

- **Vendor RtMidi** (D1):
  - `external/rtmidi/{RtMidi.h,RtMidi.cpp,LICENSE}`, version 6.0.0, with
    the version recorded in a README line.
  - CMake option `PORYDAW_MIDI`, per-platform defines
    (`__WINDOWS_MM__`/`__MACOSX_CORE__`/`__LINUX_ALSA__`) and the link
    libraries above.
  - Fall back to `__RTMIDI_DUMMY__` with a `message(WARNING)` when ALSA dev
    is missing.
  - Add `libasound2-dev` to both workflows.
  - Third-party notice in the about box or docs, wherever miniaudio's is.
- **`src/audio/midiinput.{h,cpp}`**, a QObject owned by `MainWindow`:
  - Port enumeration (`ports()` returns names) and open/close by name.
  - Hot-plug polling: a 2 s QTimer compares `getPortCount`/names and opens
    newly appeared enabled ports.
  - The RtMidi callback parses note on, note off (including note-on with
    velocity 0), CC, pitch bend and channel aftertouch (ignored), drops
    sysex, clock and active sensing, and pushes `MidiMsg{status,d1,d2}`
    into the input ring.
  - The ring is 256 entries, power-of-two, has a producer mutex, and keeps
    a drop counter.
  - An `activity()` signal is throttled on the UI side.
  - `injectForTest(MidiMsg)` pushes through the same producer path.
- **Settings → MIDI page** (`src/ui/midisettingspage.{h,cpp}`, new
  `SettingsDialog::Midi`):
  - A port list with a checkbox per port; the setting is
    `midi/disabledPorts`, stored by name so new ports default to on.
  - A Refresh button.
  - "Play incoming notes" (`midi/livePlay`, default true).
  - *Fixed velocity* (`midi/fixedVelocity`, 0 = off).
  - *Recording offset* (`midi/latencyMs`, default "Auto").
  - With no backend: a single explanatory label.
- **A MIDI activity LED** on the transport toolbar: a painted dot that
  flashes on input. Its tooltip names the open ports.
- **Harness `--midicheck <root> <label>`** (`src/midicheck.cpp`, added to
  `tools/run_checks.sh`):
  - Port-list plumbing with the dummy or real backend, without asserting
    that any devices exist.
  - The injector reaches the ring.
  - Parse table: note-on velocity 0 counts as note-off, and running status
    is handled by RtMidi.
  - Ring overflow increments the drop counter.
  - The settings page persists toggles. Use
    `QStandardPaths::setTestModeEnabled` as settingscheck does.

### Step 2 — Live play on the selected track

- **Audio-thread state.**
  - `AudioEngine` gets `setLiveTarget(track)`, an atomic where -1 means
    none.
  - It also gets the input-ring consumer, which runs in `process()` right
    after `applyTimedPreviews`.
- **Held entries in the timed-preview table.**
  - `TimedPreview`/`m_timedActive` gain a `held` flag. Held entries skip
    the countdown in `applyTimedPreviews` and are released only by a note-off.
  - Steal order prefers non-held entries.
  - Live notes go through the same `previewNoteOn` (`audioengine.cpp:561-567`)
    on the target track. They pass VOL and PAN as "none", so the track's
    chased state applies.
  - Messages are handled as follows:
    - note on/off follow the retrigger rule;
    - *Fixed velocity* overrides the velocity when set;
    - CC64 is sustain: note-offs are deferred while the pedal is down and
      released on pedal-up;
    - CC120/123 are panic.
  - **Pitch bend and mod act on the whole track, as intended (D4).**
    - Pitch bend goes to `m4a_engine_pitch_bend` on the target track, as
      `bend14 >> 7`; the track's own BENDR sets the range. It re-pitches
      sounding song notes too.
    - CC1 goes to `m4a_engine_cc` as mod depth.
    - While playing, the song's own bend and mod events overwrite these
      values when they occur.
    - Restoring the track's state:
      - The keyboard's values are not undone on release; a centred wheel
        sends 0 anyway.
      - A target change re-chases the old track's bend and mod at the
        playhead (`TimelinePlayer::chase`), so a parked mod wheel doesn't
        stick to a track you've left.
      - Transport and seek re-chase as they already do.
- **Target change.** When the target track changes, release the held and
  sustained sets on the old track.
- **UI wiring.**
  - `MainWindow` publishes the target on `selectedTrackChanged`, on tab
    activation, after `updateSong`, and as -1 when there is no song or
    `!m_audioOk`.
  - "Play incoming notes" off means target -1 for sound; capture still
    works.
  - A `midi.panic` keymap command in the "Transport" category.
- **Roll feedback (small).**
  - Keys held from MIDI light up on the roll's keyboard column, with the
    same highlight as a mouse-held key.
  - The audio thread publishes an atomic 128-bit held mask for this.
- **midicheck additions**, all through the injector, with audio up:
  - Three held keys give three active channels on the target track.
  - They stay sounding past the band-sweep's 10 s cap: held entries never
    count down. Negative-test this by dropping the flag.
  - Note-off releases them.
  - Changing the selection releases the old notes and plays the new track's
    voice.
  - Sustain holds and releases.
  - The fixed-velocity setting is honoured.
  - A bend message changes the target track's bend (song notes included).
    A target change restores the old track's chased bend and mod.
  - Held entries survive steal pressure from 24+ band-sweep previews ahead
    of non-held ones.
- Ends with a Windows manual checkpoint (user): latency feel, chords, sustain, bend/mod, track following, hot-plug.

### Step 3 — Recording: capture, Record action, commit

- **Record action.** `transport.record` is a checkable QAction on the
  toolbar with a painted red-dot icon (matching the painted-icon rule).
  Behaviour follows D5. It is disabled for read-only sessions or when no
  track is selected. Pick a free default key (verify against the keymap
  table and the roll's single-letter bindings A/V/B/M/S); fall back to no
  default if there's a conflict.
- **Capture.** An atomic `m_captureArmed` on `AudioEngine`. While it is
  armed and the transport is Playing, every applied note on/off (after
  sustain processing, so pedal-held notes end on pedal-up) is pushed to the
  capture ring as `{key, vel, on, samplePos}`.
  - `samplePos` is `m_player.position()` at the callback start.
  - Capture is armed on the UI side **before** `play()` on a fresh start.
- **`RecordingTake`** (`src/core/recordingtake.{h,cpp}`) is pure logic with
  no Qt widgets, so it is harness-testable without audio.
  - `begin(track, punchInSample)`, `push(event)`, and
    `finish(timeline, stopSample, latencySamples, loop)` returning
    `vector<NewNote>`.
  - Rules:
    - Notes still open at finish end at the stop position.
    - The wrap rule from §2.3.
    - Notes with negative ticks after the latency shift are clamped to 0,
      or to punch-in for a mid-song punch.
    - Minimum 1-tick duration.
    - Same-(tick,key) collapse, last one wins.
- **Commit.** At punch-out or stop, `MainWindow` drains the remaining
  capture events, then `doc.addNotes` inside an edit group "Record %n
  note(s)". The commit happens **before** `stopPlayback` seeks the cursor,
  so `tickForSample` uses the take's own timeline. If the document changed
  mid-take (a user edit), the take still commits against the current
  timeline; mention this in the notes for the user's manual pass.
- **Session switching.** Switching tab or song mid-take commits the take to
  its own session first; the take holds a `QPointer`/session id.
- **midicheck additions:**
  - Pure `RecordingTake` cases with fabricated sample stamps: tempo change
    mid-take, loop wrap, held-at-stop, latency clamp, dedup.
  - End to end: arm, play, inject notes with pumped events, stop. Then
    check: the notes landed on the target track within ±1 period of the
    expected tick, there is exactly one undo entry, and undo removes them
    all.
  - Record is disabled on a read-only bundle tab.
  - An empty take pushes no undo entry.
  - Target lock: change the selected track mid-take and the notes still
    land on the original track.
  - Negative-test each rule.
- Ends with a Windows manual checkpoint (user): recorded timing vs. what was
  played, Auto recording offset, 2-pass loop overdub, punch in/out.

### Step 4 — Live take display

- While recording, the roll paints the take's notes, finished and growing,
  in the target track's colour at reduced alpha, with a "REC" tint on the
  ruler's punched span.
- This is a roll-internal overlay fed by a `SongView::setRecordingPreview(
  vector<PreviewNote>)` call on each playhead tick. It does **not** use
  document edits.
- Held notes grow to the current playhead tick.
- With Follow Playhead off, the view does not scroll on its own.
- rollcheck- or midicheck-style pixel probe: a growing note is visible
  during the take and gone (replaced by a real note) after commit.

### Step 5 — Recording options

- **Input quantize** (D9): a toggle in the Record button's drop-down menu
  and on Settings → MIDI (`midi/quantizeStarts`). At commit it snaps
  starts with the roll's `snapTick` at the time of commit, and keeps
  durations.
- **Recording offset auto-detection** shown on the Settings page as "Auto
  (≈ N ms)".
- Revisit D7 (Replace mode) and D10 (count-in) **only if the user asks**.

### Step 6 — Scripting `porydaw.midi` (D13)

- `porydaw.midi.on('message', fn)` delivers `{status, data1, data2,
  port}` on the UI thread from a third ring, or as a fan-out of the
  capture drain.
- `midi.ports()` lists the ports.
- `midi.send` is not provided (no output).
- Also: API.md, d.ts, regenerated `docsrc/reference/scripting.md`, and a
  scriptcheck case using the injector.

### Step 7 — Docs, CHANGELOG, final review

- `docsrc/manual/` page "Playing and recording with a MIDI keyboard". It
  covers device setup and WSL/Linux notes (needs the ALSA sequencer;
  WSL has none). It also covers the §2.2 edge cases (keys held across Play
  go silent; overdubbing a pitch the track is playing).
- A CHANGELOG bullet under Unreleased.
- `/code-review` the branch, then full normal + ASAN sweeps.
- **User manual pass on Windows with a real keyboard:**
  - latency feel;
  - hot-plug;
  - the recording offset;
  - a 2-pass loop overdub.

## 5. Progress

### Step 1 — done 2026-10-11 (uncommitted on `midi-input`)

**Landed**

- `external/rtmidi/` holds the unmodified RtMidi 6.0.0 `RtMidi.h`, `RtMidi.cpp`
  and `LICENSE`, fetched from the `6.0.0` tag.
  - Version, URL and sha256 are in `external/rtmidi/LICENSES.md`, following
    the miniz/dr_libs convention rather than a README.
  - CMake builds it as the static library `rtmidi` when `PORYDAW_MIDI` is ON
    (the default). It defines `PORYDAW_MIDI` (PUBLIC) on `porydaw_app`.
  - Per-platform defines and link libraries: `__WINDOWS_MM__` + `winmm`;
    `__MACOSX_CORE__` + CoreMIDI/CoreAudio/CoreFoundation; `__LINUX_ALSA__`
    + `ALSA::ALSA` + Threads.
  - Without ALSA it falls back to `__RTMIDI_DUMMY__` and prints a
    `message(WARNING)`.
  - `libasound2-dev` was added to every Linux job that compiles: the
    `build` matrix (as an `if: runner.os == 'Linux'` step), `asan-checks`,
    and release.yml's Linux AppImage job.
  - The README's License section credits RtMidi. Nothing credits miniaudio
    anywhere (no About-box or docs notice exists), so the README is the
    only place a notice could go.
- `src/audio/midiinput.{h,cpp}`:
  - `MidiMsg{status,d1,d2}`.
  - `MidiInputRing`: 256 entries, multi-producer through a mutex,
    lock-free single consumer, with a drop counter.
  - `MidiInput` (QObject, owned by MainWindow through
    `std::unique_ptr`, accessor `midiInput()`):
    - one enumerating `RtMidiIn` plus one `RtMidiIn` per open port;
    - a 2 s hot-plug poll;
    - `midi/disabledPorts`, `midi/livePlay`, `midi/fixedVelocity` and
      `midi/latencyMs` (`kLatencyAuto` = -1);
    - a static `parse()`, `injectForTest()`, and a throttled `activity()`
      signal (at most one queued hop in flight, at least 40 ms apart).
- `src/ui/midisettingspage.{h,cpp}` adds Settings → MIDI as the second
  section (`SettingsDialog::Page::Midi`, id `"midi"`).
  - It holds the port list (checkable items), a Refresh button, "Play
    incoming notes", Fixed velocity (0 shows "Off") and Recording offset
    (-1 shows "Auto").
  - objectNames: `settingsMidiPorts`, `settingsMidiRefresh`,
    `settingsMidiPortsStatus`, `settingsMidiLivePlay`,
    `settingsMidiFixedVelocity`, `settingsMidiLatency`, and
    `settingsMidiUnavailable` for the no-backend label.
  - The page talks to `MidiInput` directly, as the Plugins page talks to
    `ScriptHost`; `MidiInput` persists.
- `src/ui/midiactivityled.{h,cpp}` is the painted dot on the transport
  toolbar after Follow Playhead, objectName `transportMidiLed`.
  - It is dim when no port is open, a ring while listening, and filled
    with `polyphony_cell_active_background` for 120 ms on activity.
  - `MainWindow::updateMidiLed` sets the tooltip from the open ports, or
    "No MIDI input" plus the reason.
- `--midicheck <root> <label> [shot.png]` lives in `src/midicheck.cpp` and
  `MainWindow::runMidiWindowCheck`, and is wired into `run_checks.sh` after
  velcheck. It checks:
  - the parse table;
  - ring FIFO, overflow and drop count;
  - 4 producer threads against a concurrent consumer: nothing lost or
    duplicated, per-producer order kept;
  - port plumbing on whatever backend exists;
  - the injector reaching the ring;
  - activity coalescing and throttling;
  - page persistence and relaunch reload, and port toggles through
    `setPortListForTest`;
  - the LED and the Settings section with mus_abandoned_ship open.
- settingscheck now expects 5 sections (4 without scripting), with MIDI at
  row 1.

**Deviations**

- The page's controls exist only when a backend is up; with none it is the
  single label, as planned.
  - The harness drives the controls through
    `MidiSettingsPage(midi, parent, /*showWithoutBackend=*/true)` and
    `MidiInput::setPortListForTest`, because WSL and CI have no backend.
- Program change (0xCn) is dropped by `parse()` along with aftertouch.
  - D2/D3 play the selected track's voice, so a keyboard's program change
    has nothing to drive.
  - Step 6's scripting fan-out would need parse() widened if plugins want
    it.
- A port that fails to open (for example held by another program) is not
  retried by the 2 s poll. Refresh, toggling its checkbox, or the port
  reappearing retries it. This avoids a log line every 2 s; the page marks
  such a port "(unavailable)".
- Two ports with an identical name count as one identity, and only the
  first opens. Names are as RtMidi reports them: WinMM appends the device
  index and ALSA the `client:port` pair, so in practice names are unique.

**Gotchas for step 2**

- **Consumer API:** call `MidiInputRing::pop(MidiMsg *out)` in a loop at
  the top of `AudioEngine::process`; it returns false when empty and never
  locks.
  - Hand the engine a `MidiInputRing *` (for example
    `setMidiInputRing(&m_midiInput->ring())`).
  - `MainWindow::~MainWindow` already calls `m_midiInput->closeAll()`
    before `m_audio.shutdown()`. The ring lives inside `MidiInput`, so
    keep `m_audio.shutdown()` before `m_midiInput` is destroyed. Member
    order makes `m_midiInput` die before `m_audio`, which is safe only
    because shutdown runs first in the destructor body.
- **Nothing drains the ring in step 1.** In the app it fills after 256
  messages and every later message is dropped and counted. Step 2's
  consumer fixes that.
  - Harness checks that inject must `pop` or drain the ring first, or
    they hit the full ring.
- Messages in the ring are already normalised:
  - a velocity-0 note on arrives as 0x8n with d2 = 0;
  - the channel stays in the low nibble (omni: mask it off);
  - pitch bend is d1 = LSB, d2 = MSB, so `bend14 = d1 | d2 << 7`, centre
    8192.
- `livePlay()` and `fixedVelocity()` live on `MidiInput`, and
  `settingsChanged()` fires on each change. Mirror them into atomics on
  `AudioEngine` from MainWindow.
- RtMidi throws only when no error callback is installed. Every client
  porydaw makes installs `onRtMidiError` (which only logs) right after
  construction, so later calls report through `isPortOpen()` and empty
  names instead of exceptions.
  - Construction itself still throws; it is caught.
  - With only the dummy API compiled, `RtMidiIn(UNSPECIFIED)` throws
    "no compiled API support found". `startBackend` checks
    `RtMidi::getCompiledApi` first for that case.
- On WSL every `MidiInput` construction prints ALSA's
  `snd_seq_hw_open ... /dev/snd/seq failed` line plus RtMidi's own stderr
  line. This happens in every harness that builds a MainWindow; it is
  harmless noise. The Refresh button retries the backend, but the poll
  does not.

**Verification**

- Builds: normal `build`, `build-asan`, `build-nomidi`
  (`-DPORYDAW_MIDI=OFF`) and `build-dummy`
  (`-DCMAKE_DISABLE_FIND_PACKAGE_ALSA=ON`, which shows the CMake warning)
  all build.
  - `--midicheck` passes on all four. The dummy and OFF builds report "This
    build of porydaw has no MIDI support…".
- `tools/format.sh --check` is clean, and the new files pass
  `clang-format --dry-run`.
- Negative tests: each assertion failed with the code broken and passed
  once it was restored.
  - Velocity-0 conversion removed.
  - Drop counter removed.
  - `setLivePlay` persistence removed.
  - Activity hop never posted.
  - Activity interval check removed.
  - LED `activity` connect removed.
  - Port-toggle `setPortEnabled` call removed.
- Sweep `tools/run_checks.sh <bin> ~/pokeemerald ~/teamaqua-pokeemerald`:
  every harness passes on both the normal and ASAN builds except
  `editcheck`, including midicheck and settingscheck.
  - `editcheck` fails on `mus_bw_surf: a '['-named track lost its name in
    the move`.
  - That failure is **pre-existing and unrelated**. HEAD `b385746`, built
    in a scratch worktree, fails identically. The song was added to the
    local `~/pokeemerald` on 2026-10-04, and editcheck touches no file this
    step changed.
- Not verified: real devices. WSL has no `/dev/snd`, so no port has ever
  opened. Open/close, hot-plug, the WinMM/CoreMIDI backends and the
  callback thread delivering into the ring all wait for the user's manual
  pass on Windows.

### Step 2 — done 2026-10-11 (uncommitted on `midi-input`)

**Landed**

- `AudioEngine` (`src/audio/audioengine.{h,cpp}`):
  - `setMidiInputRing` (MainWindow hands it `&m_midiInput->ring()` before
    `init`), `setLiveTarget`/`liveTarget`, `setLivePlay`,
    `setLiveFixedVelocity`, `liveAllNotesOff` (a counter applied at the next
    callback), and `liveHeldKeys(lo, hi)` (two relaxed `uint64_t` atomics).
  - One active-note table for timed previews and live notes. `ActiveTimed`
    gained `held` and `order`; `startActive` (retrigger in place, else a
    free slot, else steal: the non-held entry closest to its note-off, the
    oldest held entry only when every slot is held) and `releaseActive`
    (note-off **only** when the table still has that track+key) are shared
    by `applyTimedPreviews` and the live path. Held entries skip the
    countdown.
  - `applyMidiInput()` runs in `process()` right after
    `applyTimedPreviews`: effective target (-1 when live play is off, no
    song is loaded, or the target is out of range), panic counter, target
    change (release on the old track, then `TimelinePlayer::chaseBendMod`
    on it at `m_player.position()` if the keyboard's bend/mod touched it),
    then drains the ring through `handleLiveMessage`, then
    `publishLiveHeld()` (mask from the table's held entries on the current
    target).
  - `handleLiveMessage`: omni; note on (fixed velocity override; a key
    already on retriggers), note off (deferred while CC64 ≥ 64), CC64
    pedal-up releases keys on but not physically down, CC120/123 release
    everything, CC1 → `m4a_engine_cc(track, 1, v)`, pitch bend →
    `m4a_engine_pitch_bend(track, int16_t(bend14 - 8192))`. All other CCs
    are ignored (CC7/CC10 are not forwarded).
  - `cutAllSound` and `clearTimedPreviews` (so loadSong, unloadSong,
    updateTimeline, updateVoicegroup and seeks) also call
    `forgetLiveNotes()`: the keyboard's on/down sets and the published mask
    are cleared without note-offs.
  - Harness hooks: `holdDeviceForTest(bool)` (stops the device and keeps
    cold ops from restarting it), `pumpForTest(frames)` (the real
    `process()` into scratch, 512-frame chunks), `engineForTest()`.
- `TimelinePlayer::chaseBendMod(engine, timeline, pos, track)`: the latest
  bend and CC1 on that one track at or before `pos`, else the engine
  defaults (bend 0, mod 0). Nothing else is touched.
- `MainWindow`: `publishLiveTarget()` (selected track of the active tab, -1
  without a loaded song or `!m_audioOk`) on `selectedTrackChanged`, at the
  end of both `activateSession` branches, and after `updateSong` in
  `onDocumentChanged`. `syncLiveSettings()` on `MidiInput::settingsChanged`.
  `activateSession` also calls `liveAllNotesOff()` and clears the previous
  view's lit keys. A port vanishing from `openPorts()` (on `portsChanged`)
  calls `liveAllNotesOff()`. Tools → "MIDI: All Notes Off", keymap
  `midi.panic` (category Transport, no default key).
- Roll feedback: `SongView::setLiveHeldKeys(lo, hi)` (+ getters) forwards
  to `PianoRoll::setLiveKeys`, which repaints only the keyboard column on a
  change. `keyLit(key)` (mouse-held key **or** live mask) drives both
  `drawKeyboard` and the hover geometry, so a live key gets the same
  highlight as a mouse-held one.
- `--midicheck` live-play block (`MainWindow::checkLivePlay` in
  `src/midicheck.cpp`): picks two used tracks with a plain DirectSound voice
  at the playhead (the first with a looping sample if there is one: on
  mus_abandoned_ship tracks 0 (looping) and 1), parks the device and pumps
  simulated time. It checks every item of the Step 2 list, plus: the ring
  is drained by the callback; the activity-driven poll lights the roll;
  a pixel probe of the keyboard column (all keys lit vs none); fixed
  velocity off passes the keyboard's through; the new target's bend/mod are
  untouched by the old one's re-chase; panic command and CC123; live play
  off and target -1 drain silently; the target republishes.

**Deviations**

- Pitch bend: `m4a_engine_pitch_bend` takes the signed 14-bit value
  (-8192..8191) and does the `>> 7` itself (`m4a_engine.c:1192-1204`), as
  `TimelinePlayer::dispatchEvent` already calls it, so live play passes
  `int16_t(bend14 - 8192)` unshifted. Shifting first would make it 128×
  too small.
- Held-key polling uses its own 17 ms `m_liveKeysTimer`, started by
  `MidiInput::activity` and stopped after ~0.5 s with nothing held. The
  playhead timer only runs while playing, so it can't light keys at rest.
- The held mask shows what the table **sounds** on the target (pedal-held
  keys stay lit until pedal-up; a key cut by a transport change goes dark
  although it's still physically down).
- Two guards stop a stray note-off after a cut: `forgetLiveNotes` clears the
  keyboard's on-set, and `releaseActive` sends no note-off for a key the
  table no longer has. The negative test had to break both.
- A timed preview on the same track+key as a held live note retriggers it
  as a timed entry (one entry per track+key, as before); the user's
  note-off still releases it. This is rare, so it was left alone.
- App quit needs no extra code: `~MainWindow` closes the ports, then
  `m_audio.shutdown()` destroys the engines.

**Gotchas for step 3**

- **Capture hook:** `AudioEngine::applyLiveNote(key, velocity)` is the
  single place an applied note on (velocity > 0) or off (0) lands, after
  sustain processing and the fixed-velocity override. Today it returns early
  when `m_liveTrack < 0`. Capture must sit **above** that early return, so
  that live play off still records.
  - But live play off currently also makes the effective target -1, and a
    target change runs `releaseLiveNotes()`. For capture with live play
    off, split the "sound target" (-1 when live play is off) from the
    "capture target" (the selected track, locked at punch-in).
- **Timestamps:** `m_player.position()` read at the top of `process()`,
  before `m_player.render`, is the "player position at the start of the
  callback" from §2.4. `applyMidiInput` already runs before render, so that
  value is current there. Within one callback every message gets the same
  stamp, which is the planned one-period jitter.
- `forgetLiveNotes()` runs on every cut, including the Stopped→Playing
  transition that starts a take. Keys held across Record therefore produce
  no capture event at all, neither on nor off. `RecordingTake` must still
  tolerate a note-off without a note-on.
- Test driving: `holdDeviceForTest(true)` plus `pumpForTest` gives
  deterministic sample time. `m_audio.play()` takes effect at the next pump.
  `engineForTest()->pcmChannels[i]` has `trackIndex`, `midiKey`, `velocity`
  and `status`; "keyed" means `CHN_ON` without `CHN_STOP|CHN_IEC`.
  `QImage::operator==` reported two pixel-identical grabs as different, so
  compare pixels.

**Verification**

- Builds: `build`, `build-asan` and `build-nomidi` all build.
  `tools/format.sh --check` is clean.
- `--midicheck` passes on all three. On build-nomidi the injector still
  feeds the ring, so live play works there too.
- Negative tests: each was broken, confirmed FAIL, and restored by a
  targeted reverse edit; the `git diff` hash was checked afterwards. They
  were:
  - held flag dropped;
  - steal ignores held;
  - note-off ignored;
  - no release on target change;
  - selection not published;
  - sustain ignored;
  - fixed velocity not mirrored;
  - bend not sent;
  - mod not sent;
  - no re-chase;
  - mask not cleared;
  - stray note-off (both guards);
  - panic action inert;
  - CC123 ignored;
  - live play ignored;
  - view mask not stored;
  - activity doesn't start the poll;
  - roll ignores the live mask (pixel probe);
  - ring not drained.
- Sweep `tools/run_checks.sh <bin> ~/pokeemerald ~/teamaqua-pokeemerald`:
  **PASS (all harnesses)** on both the normal and ASAN builds. That
  includes `editcheck`: the `mus_bw_surf` failure noted in step 1 did not
  reproduce this time, and this step does not touch editcheck's code path.
- Not verified:
  - real devices and latency feel (WSL has no `/dev/snd`);
  - the port-close panic (needs a real port to vanish);
  - live notes during playback by ear, including the §2.2 overdub edge
    cases;
  - the visual highlight on a real display.

  These are for the Windows manual checkpoint.


### Step 2 follow-up — mod wheel scaled to the MOD lane's range (2026-10-11)

- Windows checkpoint (user, 2026-10-11): live play works. The one request
  was that the mod wheel should scale to the MOD lane's **visible** range
  (MOD auto-fits from 0..16 by default), not send its raw 0..127.
- Wheel 0..127 now maps onto 0..`SongView::laneDisplayMax(track, 1)`,
  rounded. That is the axis max as drawn: the chosen "Value range", or the
  auto-fit rung, grown to hold the data. The lane-axis logic moved out of
  `AutomationArea::rowRange` into a public `laneDisplayMax`, so it works
  for a hidden row too.
- `MainWindow::publishLiveTarget` publishes it with
  `AudioEngine::setLiveModMax`, an atomic. It republishes on the new
  `SongView::laneRangesChanged` signal (menu or sidecar restore) and after
  every document change, which covers auto-fit growth.
- midicheck asserts:
  - the wheel lands on the scaled value;
  - full travel equals the displayed max;
  - a range change rescales the wheel.
- Negatives:
  - with the engine scaling removed, all three fail;
  - with the range-changed republish removed, the rescale check fails.
- Normal and ASAN sweeps both PASS.

### Review fixes (2026-10-11, uncommitted on `midi-input`)

A code review of 28aaf34, ef8a087 and 096c667 was verified finding by finding.
Every finding checked out, and all were fixed.

- **A, the midicheck early exits.** When the project failed to open or the
  song didn't load, `runMidiWindowCheck` returned 1, but the caller ignored
  it and the run still printed "OK". Both exits now count a failure.
  Running with a bad label or a bad root now FAILs with exit 1.
- **B, stable port identity.**
  - The pure helpers are `MidiInput::portBaseName`, `portIdentities` and
    `identityStable`, with `PortNaming::{Plain, Alsa}`.
  - **Identity** is the base name plus the port's count among same-named
    ports: "Name", "Name (2)", and so on. A device whose real name is
    "X (2)" keeps it, and the counted port moves on to the next number.
  - **WinMM**: CMake defines `RTMIDI_DO_NOT_ENSURE_UNIQUE_PORTNAMES` PUBLIC
    on `rtmidi`, and `midiinput.cpp` `#error`s on Windows without it.
  - **ALSA**: the trailing ` client:port` is stripped.
  - **CoreMIDI**: names are used as reported.
  - `midi/disabledPorts` stores identities. Old suffixed entries just go
    stale.
  - Each open port records the raw list it opened from. The poll keeps a
    port only while `identityStable` holds: the identity is still listed
    and the ports sharing its base name are unchanged. So unplugging
    either of two identical keyboards closes both, and the remaining one
    reopens as "Name".
  - A failed identity that is no longer stable is forgotten, so it is
    retried.
  - **Refresh** now closes and reopens every open port, which recovers a
    handle left dead by a replug at the same place within one poll.
  - The new `portsClosed()` signal replaces MainWindow's open-list diff, so
    a reopen also panics: `liveAllNotesOff`.
- **C, opening at a stale index.** `openQuietly` looks the index up from a
  fresh scan. After `openPort` it checks that the index still has the
  expected raw name. If not, it closes the port without marking it failed,
  so the next poll retries.
- **D, the stuck activity timer.** `deliverActivity` reads `elapsed()`
  once and starts the timer with `max(1, interval - since)`.
- **E, Settings → MIDI with no backend.**
  - The page now always builds its controls, plus a no-backend panel: the
    `settingsMidiUnavailable` label and a **Try Again** button,
    `settingsMidiRetry`.
  - `syncBackend()` toggles between them on `portsChanged` (queued).
  - `refresh()` also emits `portsChanged` when starting the backend fails
    again.
  - Harness hook: `MidiInput::setBackendForTest(name)`.
- **F, the harness's settings redirect.** midicheck now calls
  `QSettings::setDefaultFormat(IniFormat)`, as rollcheck and velcheck do.
- **G, what closing guarantees.** The comments now say that only ALSA joins
  its callback thread on close; WinMM and CoreMIDI don't wait for a running
  callback. `OpenPort::closing`, an atomic, is set in `shutPort` before
  `closePort`, and `onMessage` returns early when it is set. This narrows
  the window but does not close it.
- **H, three small fixes.**
  - The disabled list is cached in `m_disabled`.
  - `~MainWindow` calls `closeAll(/*quietly=*/true)`, which emits nothing.
  - When ports are listed but none is open, the LED tooltip says "Every MIDI
    port is turned off or could not be opened; check Settings → MIDI."
- **I, sustain never reset.**
  - The panic counter (the `midi.panic` command and a port closing), CC120
    and CC123 now clear `m_liveSustain`.
  - CC121 lifts the pedal: it releases the keys held only by the pedal.
  - Tab switching now uses a new `liveReleaseNotes()`, which releases the
    notes but keeps the pedal state. A plain target change keeps it too.
  - The harness no longer sends its own pedal-up after the panic tests.
  - New checks: after a panic, after CC123 and after CC121, a key's note-off
    releases at once; CC121 releases the keys the pedal was holding.
- **J, two consumers on the ring.** The window check now holds the device
  around its LED-test drain.
- **K, bend and mod values that could match the song's.** The bend
  (±32) and the wheel (100 or 50) are now picked to differ from the track's
  chased values.

**Negatives**

Each was broken, confirmed FAIL, and restored by a targeted reverse edit;
the `git diff` hash was identical afterwards.

- B: no occurrence numbering; no ALSA strip; `identityStable` ignores the
  group; the literal-name collision loop removed.
- E: `syncBackend` not driven by `portsChanged`; the no-backend panel never
  shown.
- H: the "turned off" tooltip branch skipped; `saveDisabledPorts` not
  called.
- I: panic keeps the pedal; CC123 keeps the pedal; CC121 ignored.
- K: the bend forced to match the song's, which trips the guard.

Not negative-testable here (no backend on WSL):

- **C**: the stale-index check after open.
- **D**: needs a scheduler race.
- **F**: Linux has no registry or plists.
- **G**: the closing flag.
- **J**: a race.

**Builds and sweeps**

- `build`, `build-asan`, `build-nomidi` and `build-dummy` all build.
  midicheck reports OK on all four.
- `tools/format.sh --check` is clean.
- `tools/run_checks.sh` passes every harness on both the normal and ASAN
  builds.

**Report-only, not fixed** (pre-existing or consistent with the design)

- The bend/mod re-chase ignores the loop.
- Lane-range keys go stale after a track is deleted or moved.
- The two halves of the held mask can tear.
- CoreMIDI run-loop reentrancy.
- Unfixable by design: with identical WinMM or CoreMIDI devices, which one
  is "Name (2)" depends on position, so after an unplug a disabled
  "Name (2)" can turn into the enabled "Name".

Real-device behaviour (WinMM naming, hot-plug, Refresh recovery) is left
for the user's Windows pass.

### Fix — crash on unplug (Windows, 2026-10-11)

- User report: Porydaw crashed on Windows every time the keyboard was
  unplugged.
- **Cause, found by reading the code** (it can't be run here): RtMidi 6.0.0's
  `MidiInWinMM::closePort`. On an unplugged device,
  `midiInUnprepareHeader` fails. The loop had already freed sysex buffer 0
  and then returned early, leaving `connected_` true and the critical
  section held. `~MidiInWinMM` then ran `closePort()` again and
  double-freed the buffer. The hot-plug poll closes a vanished port, so
  every unplug reached this.
- **Fix:**
  - `external/rtmidi/RtMidi.cpp` is patched (marked `porydaw patch`, noted
    in `LICENSES.md`). It frees every buffer, clears the vector, finishes
    the close, and only then reports the error.
  - `MidiInput::openQuietly` also calls `setBufferSize(1024, 0)`, so WinMM
    prepares no sysex buffers at all. Sysex is ignored anyway. This also
    avoids the upstream window where a late `MIM_LONGDATA` callback can
    read a freed buffer during close.
- **Verified:** all four builds pass; midicheck passes under ASAN; and the
  patched `RtMidi.cpp` cross-compiles with MinGW (`__WINDOWS_MM__`). The
  unplug itself needs the user's Windows pass.

### Step 3 — done 2026-10-11 (uncommitted on `midi-input`)

**Landed**

- `src/core/captureevent.h`: `CaptureEvent{samplePos, resumePos,
  generation, segment, kind, key, velocity}`. The kinds are `Begin`,
  `NoteOn`, `NoteOff`, `Cut` and `Boundary`.
- `src/core/recordingtake.{h,cpp}`: `RecordingTake`, pure logic.
  - `begin(track, generation)`, `push(ev)`, `clear()`.
  - `toNotes(timeline, endSample, endSegment, latencySamples)` returns
    `vector<SongDocument::NewNote>` sorted by tick, then key.
  - Accessors: `notes()` (raw sample-time notes), `punchInSample()`,
    `segment()`, `unmatchedOffs()`.
  - The rules are in the class comment. In short: ticks are nearest-tick
    through `tickForSample`, with a 1-tick minimum. The offset moves played
    times earlier but never before the note's segment start. Ends the take
    imposed (wrap, seek, cut, stop) are not shifted. A note held across one
    or more wraps ends at the loop end. Unmatched offs are ignored. Later
    notes win.
- `TimelinePlayer::wrapCount()`: a never-reset counter bumped at each loop
  wrap.
- `AudioEngine` capture:
  - `armCapture(generation)` (0 disarms), `popCapture`, `captureDropped`,
    `captureClock(&pos, &segment)` and `outputLatencySamples()`.
  - The ring is SPSC, 4096 entries, with a drop counter.
  - `syncCapture()` runs at the top of `applyMidiInput`, after the
    transition and the seek. It pushes `Begin` at the first Playing callback
    of a new generation.
  - `applyLiveNote` pushes NoteOn/NoteOff **above** its early return.
  - `forgetLiveNotes` (every cut) pushes `Cut`.
  - `applyPendingSeek` and `updateTimeline` (when the position moves) push a
    `Boundary`; so does every wrap `process()` rendered.
  - The clock (segment << 44 | position) is published at the end of each
    capturing callback.
- `MainWindow`:
  - `transport.record`: a checkable toolbar action between Stop and Loop,
    with a painted dot in `song_view_playhead` red (`recordIcon`). The
    keymap default is **R**, which is free in every context.
  - `toggleRecord`, `beginRecording`, `finishRecording`, `drainCapture`,
    `recordUnavailableReason`, `syncRecordAction` (called from
    `updateTransportActions`), and `recordingLatencySamples()`.
  - Commit points: `stopPlayback`, `pausePlayback`, Record again,
    `activateSession`, `destroySession`, `maybeSaveSession` (every close,
    replace, project switch and quit prompt), `populateSession` (in-place
    replace), and `synchronizePlayhead` when the transport is no longer
    Playing (the song ending by itself).
  - The status bar says "Recording into 3: Name…", then "Recorded N
    note(s)." or "Nothing was recorded.".
- `--reccheck <root> <label>` (`src/reccheck.cpp`), wired into
  `run_checks.sh` after midicheck.
  - Pure take cases: basics and generation filtering, rounding and the
    minimum duration, a tempo change, held-at-stop (and a stop read in
    another segment), single and double wraps, the offset clamped at
    punch-in and loop start with forced ends unshifted, later-pass
    dedup and trims, cut, re-press, a lost Boundary, never-begun.
  - End to end with the device held: a fresh start (Begin at the cursor,
    ±1 period, one undo step "Record 2 note(s)", undo restores); an empty
    take; punch in and out while playing (a key held at punch-in is ignored,
    the offset is clamped at punch-in); the target lock; sustain; fixed
    velocity; live play off; Auto and 100 ms offsets; a wrap plus a seek
    plus a 2-pass overdub in one take; a document edit mid-take; capture
    ring overflow; the song ending by itself; a tab switch committing to its
    own tab; Record disabled on a read-only bundle tab.

**Deviations, and why**

- **The capture event carries structure, not only notes.** A wrap counter
  alone can't place the end of a note held across a seek, or across a wrap
  whose position is unknown. So the audio thread sends explicit `Boundary`
  events (end, resume) and stamps every event with a segment index.
  - A dropped Boundary is still detected from the segment jump.
  - A generation stamp keeps a late event from an earlier take (a callback
    in flight at punch-out) out of the next one.
  - The punch-in is the audio thread's `Begin` position, not a UI guess.
  - So `finish(timeline, stopSample, latency, loop)` became
    `toNotes(timeline, endSample, endSegment, latency)`: the loop comes in
    through the Boundaries.
- **The lock also covers the sound.** While recording,
  `publishLiveTarget` publishes the take's track. A selection change
  mid-take therefore neither moves the sound nor releases the held notes,
  which would otherwise end them in the take. The target is published
  again at commit.
- **Pause punches out and commits**, like Stop: the audio thread captures
  only while Playing. Record while paused resumes from the pause position;
  only Stopped starts from the edit cursor.
- **A seek mid-take** (moving the edit cursor while playing, Go to Start)
  starts a new segment. Held notes end at the seek point, and recording
  carries on at the landing.
- **A document edit mid-take** (an undo, a mouse edit) is allowed. The
  engine cuts the live notes, so the take ends its held notes there too
  (`Cut`). The take commits at punch-out against the timeline as it is
  then, as a separate undo step after the edit's. Positions are in sample
  time, so a tempo edit mid-take moves where the take lands. **Flag for the
  manual pass.**
- **Within-batch overlaps are resolved, not only same-(tick, key).**
  Different passes can overlap on a key at different ticks, and `addNotes`
  only resolves against the document. The take applies
  `resolveNoteOverlaps`' head, tail and cover rules among its own notes,
  with later notes winning.
- **Ends the take imposed are not shifted by the offset** (wrap, seek, cut
  and stop); only played note-offs (including pedal-up) are. So a wrapped
  note ends exactly at `loopEndTick`.
- **The status-bar reason** for a disabled Record is the action's status
  tip and tooltip ("Record (unavailable: …)"). A disabled action's R can't
  fire, so pressing R on a bundle tab does nothing visible.
  `beginRecording` itself puts the reason in the status bar when it is
  reached. **Judgment call for the user.**
- **The checks are a separate `--reccheck`**: midicheck was already 936
  lines.

**Gotchas for step 4 (ghost notes)**

- **Reading the take:** `MainWindow::recordingTake()` is non-null while
  recording. `m_audio.captureClock(&pos, &seg)` then
  `take->toNotes(*m_active->timeline, pos, seg, recordingLatencySamples())`
  gives exactly what a commit at that moment would write, held notes grown
  to the clock.
  - `toNotes` is per-key and cheap, but it allocates; call it once per
    playhead tick.
  - The ring is drained at the top of `synchronizePlayhead` (`drainCapture`)
    before `setPlayheadSample`, so feed the overlay after that in the same
    tick.
- The clock is published only while capturing, at callback ends.
  `punchInSample()` is valid once `started()`; before the first callback
  the take has no Begin.
- For the "REC" tint on the punched span: the take records its current
  segment's start but not a history. Wraps go back to `loopStartTick`;
  keep the span from punch-in and union with the loop when `segment() > 0`,
  or add a segment list to the take.
- The take always belongs to `m_active` (every path away commits first),
  so the overlay goes on `m_active->view`.
- With Follow Playhead off nothing here scrolls; finishRecording's edit is
  an ordinary document change.

**Negative tests**

Each was broken by a scripted edit, confirmed FAIL with the expected
message, and reversed by a targeted reverse edit; a grep confirmed no
marker was left, and `git diff --stat` matched.

- Take:
  - generation filter;
  - rounding (truncation);
  - minimum duration;
  - tempo map (constant tempo);
  - stop position;
  - the stop's segment check;
  - Boundary closing held notes;
  - the floor clamp;
  - forced ends shifted;
  - later wins (both versions of the loop);
  - Cut ignored;
  - re-press leaves the note open;
  - lost-Boundary detection;
  - the not-started guard.
- Window:
  - Record not on the toolbar;
  - no R default;
  - Begin before the seek;
  - edit-group text removed;
  - per-note pushes (undo leaves notes);
  - status texts (both);
  - stop sample 0;
  - Record-again stops;
  - an unmatched off recorded as a note;
  - sound lock off;
  - commit to the current selection;
  - no republish at commit (empty take);
  - capture at the physical key-up (sustain);
  - recorded velocity 127 (fixed velocity);
  - capture below the early return (live play off);
  - Auto = 0;
  - ms conversion;
  - no wrap Boundary;
  - no seek Boundary;
  - no Cut;
  - no drop count;
  - no auto-stop commit;
  - read-only gate removed.
- The Stop and tab-switch commits are each backed up by
  `synchronizePlayhead`'s safety net, so they were negative-tested together
  with it removed.
- Not falsifiable by one edit: "an empty take pushes no undo entry".
  `addNotes` refuses an empty batch, and the edit group opens lazily.

**Verification**

- Builds: `build`, `build-asan`, `build-nomidi` and `build-dummy` all
  build. reccheck and midicheck are OK on all four.
- `tools/format.sh --check` is clean.
- `tools/run_checks.sh <bin> ~/pokeemerald ~/teamaqua-pokeemerald`:
  **PASS (all harnesses)** on both the normal and ASAN builds, reccheck
  included.
- Not verified:
  - real-device timing and the Auto offset by ear;
  - a real (unparked) device's callback racing punch-out (the
    clock/drain ordering is reasoned, not exercised);
  - the icon on a real display (an offscreen toolbar grab looked right).

  These are for the Windows checkpoint.

### Step 3 follow-up — the take follows its track (2026-10-11)

The user confirmed the four judgment calls (Pause commits; a seek ends
held notes and keeps recording; edits and undo are allowed mid-take; the
read-only reason shows only on hover). The track-deleted-mid-take gap is
fixed. The take used to keep a bare engine slot, so a track add, delete
or move shifted its notes onto a neighbour.

- **Fix:** `MainWindow::followTakeTrack` listens to
  `SongDocument::tracksRemapped`. That signal covers undo and redo too,
  which blocking the UI actions would not have.
  - The take moves with its track (`RecordingTake::setTrack`), and so
    does the sound, through the documentChanged republish.
  - If the take's own track is deleted, the recording ends. Playback
    goes on, the take is dropped with no undo step, and the status bar
    says "Recording stopped: its track was deleted."
- **reccheck:** moves the take's track mid-take, then deletes it
  mid-take.
  - Negative-tested: without `setTrack` the move checks fail; ignoring
    the deletion fails the stop and message checks.
- **Verification:** all 4 builds; format clean; normal and ASAN
  sweeps **PASS (all harnesses)**.
