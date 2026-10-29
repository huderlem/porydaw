# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## Added
- Updated the audio engine (poryaaaa) to its accuracy overhaul: playback and exported WAVs now match the game running in mGBA, including the GBA's PCM DMA timing, the PSG channels and the output stage.
- Added `Settings -> Audio -> HQ mixer (ipatix)` to play PCM channels through ipatix's High-Quality Audio Mixer, for projects that have installed it. Off by default (the games' stock mixer).
   - The HQ mixer has no 7884 Hz mix rate; that choice snaps to 13379 Hz while it is on.
   - Plugins read and set it through `porydaw.audio.engine.hqMixer`.
- Playing from a MIDI keyboard (or the virtual keyboard) and clicking notes in the roll now respond in a few milliseconds instead of one to two GBA frames. They play through a separate low-latency engine instance that follows the song's track state, while the song itself keeps the game's exact timing.
   - Live notes no longer share the song's PCM channels (and no longer show in the polyphony meter).
   - The `Auto` recording offset now includes the engine's own latency, so a take lands where you heard the song.
- Added a GBA CPU readout to the status bar, beside the PCM and CGB channel counts. While the song plays, `CPU` shows how much of the GBA's VBlank time the sound driver (`m4aSoundMain`) would take each frame, estimated from the same engine that plays the song, so you can see what more PCM channels or a higher mix rate cost. Hover it for the cycle counts, the peak since playback started, and the split between the sequencer, the CGB channels and the mixer.
   - It turns amber above the `CPU budget` set in `Settings -> Audio` (100% of the VBlank window by default), and red at or past a whole frame, where the game would skip a frame of sound.
   - The new `CPU` dock (`View -> CPU`) shows it in full: the current frame and the peak since playback started, their cycle counts, a bar of the sequencer, CGB and mixer parts against the VBlank window and the rest of the frame, with your budget and the peak marked, and the engine settings it is estimated for. `Reset` starts the peak over, as does changing the mix rate, mixer or voicegroup.
   - Plugins read the same numbers through `porydaw.audio.cpu()`.
   - A thin `CPU` strip under the ruler shows the whole song's cost without playing it. It is off by default; turn it on with `View -> CPU Profile`. Each column is coloured by its most expensive frame, warming toward amber as it nears your budget, amber above it and red at a whole frame. Hover it for the frame's percent, cycles and parts. It updates shortly after each edit or engine setting change; `View -> CPU Profile` again (or right-click it) hides it. Plugins read it through `porydaw.audio.cpuProfile()`.

## Changed
- Note edits, undo/redo and other song changes no longer stop and restart the audio device. They were stalling the UI for tens of milliseconds per edit on Windows, badly so in WASAPI exclusive mode.
- Removed the `GBA analog output filter` and the `Host rate` PCM mix rate options from `Settings -> Audio`: the engine now models the GBA's output stage itself, and the PCM rate is always one of the game's `m4aSoundInit` rates. A saved host-rate setting falls back to 13379 Hz.
- Added MIDI keyboard support. Play the selected track live from a MIDI keyboard to audition notes. Record with the new `Record` button (keyboard shortcut `R`).
   - Settings are found in `Settings -> MIDI`.
   - Detects midi input devices/keybaords automatically.
   - Knobs and sliders can drive the `MOD`, `VOL`, `PAN`, `BENDR` and `LFOS` lanes, and program change buttons switch the track's voice. They're recorded too. Assign knobs in `Settings -> MIDI -> Controls`, or with `Learn MIDI control...` in an automation lane's right-click menu.
   - A keyboard's transport buttons (Play/Pause, Stop, Record, Loop, Click (metronome), Punch, Rewind, Fast-forward, Save, Undo) are integrated. These use the keyboard's Mackie Control (DAW) port, which is set to `Control surface` in `Settings -> MIDI`.
   - Also supports "punch-in" recording, where only a selected time range is recorded, and the recorded content replaces the previous content.
- Added a virtual MIDI keyboard (`View -> Virtual MIDI Keyboard`, keyboard shortcut `Alt+B`) for playing and recording without a MIDI device.
   - Either click or use your keyboard to play notes with it.
- Added `porydaw.midi` to the plugin API, plus a MIDI Monitor example plugin.
- Added a metronome. Toggle it with the new `Metronome` button on the transport toolbar (keyboard shortcut `C`). It clicks every beat, following the song's tempo and time signature. Set its volume in `Settings -> Audio`.
- Added `Settings -> Audio -> Output device` to configure playback latency (especially useful when recording with a MIDI keyboard).
   - On Windows, you can enable "exclusive mode" to bypass the Windows mixer for much lower latency. However, it means no other program can play audio while this is enabled.
- Added automation lanes for `MODT` (LFO type: vibrato, tremolo or auto-pan), `TUNE` (fine tune) and `LFODL` (LFO delay), now that the audio engine plays them. They're in the `+ Add lane` menu and play back, loop and seek like the other lanes.
   - Plugins can address them as `porydaw.song.CC.LFO_TYPE`, `.TUNE` and `.LFO_DELAY`.
   - The Import MIDI wizard now counts these three as commands that change the sound in the game.
   - Seeking or looping back before a `TUNE`, `MODT` or `LFODL` event now resets it, instead of leaving the later value playing.
- Added a right-click menu to the voicegroup dock's voice list: `Copy`, `Paste` and `Clear` a voice (`Ctrl+C` / `Ctrl+V` also work), and switch a voice between its variants (`Normal`, `No Resample`, `Reversed`, `Detuned`).

## Fixed
- Fixed a crash when a plugin's folder was deleted while Porydaw was running.
- Fixed `LFOS` changes restarting the modulation wave during playback.
- Fixed a bug where reversed directsound voices were saved as `voice_directsound_alt` in projects that only define `voice_directsound_reverse`. Porydaw now writes whichever one the project defines.
- Fixed issue where new drumset voicegroups weren't showing up unless an existing voice used them.
- Fixed issue where `Export Song Bundle` error message could be too large to fit on-screen.

## [1.3.0] - 2026-10-04

## Added
- Added `Merge...` to the track header's right-click menu. It merges the track into a chosen destination track and deletes it. You can choose to merge all events or only notes. This is handy for collapsing an imported MIDI file that has more tracks than the m4a engine allows.
- Added `.porysong` standalone song bundles, which are a single shareable file holding a song's MIDI, its voicegroup and every sample it uses.
   - `File -> Export Song Bundle...` to create a `.porydaw` file
   - `File -> Open Song Bundle...` to open and preview a `.porydaw` file in a readonly tab.
   - Press the `Import into project...` button in the preview tab to easily add the song to your decomp project.
- Added playback support for the pseudo-echo effect (`XCMD xIECV` / `xIECL`, MIDI CC 30 followed by CC 29 or 31).
- Added support for compressed sample voices.

## Changed
- `File -> Import MIDI...` and `File -> New Song...` now open the song as a draft. Nothing is actually written to your decomp project until you actually save the song.

## Fixed
- Fix issue where keysplits whose filename mismatched the actual macro would be ignored and silent during playback.
- Fix a crash when a voicegroup or keysplit table file contained a symbol name longer than 255 characters.

## [1.2.0] - 2026-09-10

## Added
- Added an Output level control (Settings → Audio) for porydaw's output volume.
- Added ability to copy/paste notes and events between different songs. Before, each song had its own separate clipboard.
- Added a Tempo input to the transport bar for the song's starting tempo. A warning appears next to it when the tempo changes later in the song. Click it to show the Tempo automation lane.
- Added vertical panning with `Alt` + mouse scroll.
- Added key rebinding support for all four scroll actions in `Settings -> Keyboard Shortcuts -> Mouse Wheel`.
- Added ability to create a time selection by left-click dragging on the ruler.
- Added `Alt` + drag inside a time selection to move its contents horizontally, and `Ctrl+Alt` + drag to duplicate and move its contents horizontally.
- Added `Shift + Alt` + drag to shift all contents to the right (can drag on either the ruler or piano roll area). Also `right-click -> shift right` on a time selection.
- Added a dancing companion avatar. It moves and grooves to the beat of the song! You can toggle its visibility in `View`, drag it around, resize it, and choose a custom image.
- Added plugin support. Plugins are JavaScript scripts. See `docs/scripting/API.md`.

## Changed
- Unified settings into a main Settings window.

## Fixed
- Fixed issue where deleting every note and event from a track would cause the track to be deleted.

## [1.1.0] - 2026-08-20

## Added
- Added a new Velocity lane, which allows viewing/editing note velocities. This is generally more flexible/powerful than vertically dragging on notes in the piano roll. Toggle the Velocity lane's visibility with `V` or `View -> Velocity Lane`.
   - PSG channels (square 1 & 2, programmable wave, noise) have a special detented view which makes it simply to see and know what final hardware volumes are used for the note's velocity.
- Added buttons for showing/hiding the Automation lanes and the Velocity lane.
- Added buffer area before and after the song's boundaries so that scrolling and snapping to the start or end of the song is easy.
- Added a bunch of general improvements to general Automation lane behavior (see Pencil Mode below).
- Added support for the "alt" square and programmable wave voices. The "alt" voice are slightly detuned to help avoid aliasing, or provide a thicker sound. Note that the noise voice's "alt" variant is identical in the m4a engine, so porydaw hides it.

## Changed
- Editing values in the Automation Lanes is now split into two modes: Pointer and Pencil modes. Toggle between the modes with `B`. If you *hold* `B` -> draw in pencil mode -> then release `B`, it will automatically revert back to Pointer mode.
   - Pointer mode is the default mode and shows the regular mouse cursor when active. It allows more selection-based editing. Hold down `shift` and draw to create a linear ramp of values.
   - Pencil mode is activated with keyboard shortcut `B`. It's used for freehand drawing values. Hold down `shift` to lock the drawing to a horizontal or vertical axis.

## Fixed
- Fixed bug where auditioning a note didn't factor in the track's Volume or Pan at that note's position in the song.
- Fixed the `voice_directsound_alt` voice type, which was mislabeled "Sample (fixed pitch)" and played like a plain sample. It is now "Sample (reversed)" and plays the sample backwards like the m4a engine does (loops are ignored in reverse).
- Improved fidelity of CGB channels
   - Fixed CGB channels' volume envelope emulation.
   - Improved CGB noise channel is now band-limited, which is more accurate to hardware's sound quality.
   - Square and programmable wave channels no longer restart their waveform on every note.
   - m4a's master volume is now set to 12 instead of 15, which matches the Pokemon games. This fixes the loudness imbalance between directsound and CGB channels.
- Fixed some font-rendering smoothness issues.
- Fixed bug where the right edge of a note couldn't be grabbed for resizing when two notes were adjacent.
- Fixed annoyance where no-op note edits would push a pointless Undo action into the edit/undo history.
- Fixed bug where duplicating a track copied every channel's events from its chunk.
- Fixed bug where velocity values could visually bleed out of the note box.
- Fixed some visual stability issues in the MIDI event viewer.
- Fixed some automation lane issues when drawing a gesture that spanned across a time-signature change.
- Fixed issue where the Ctrl center-snap threshold in the automation lanes was scaling with the height of the lane. Now, it's consistent at any zoom level.

## [1.0.0] - 2026-08-01
Initial release.

[Unreleased]: https://github.com/huderlem/porydaw/compare/1.3.0...HEAD
[1.3.0]: https://github.com/huderlem/porydaw/compare/1.2.0...1.3.0
[1.2.0]: https://github.com/huderlem/porydaw/compare/1.1.0...1.2.0
[1.1.0]: https://github.com/huderlem/porydaw/compare/1.0.0...1.1.0
[1.0.0]: https://github.com/huderlem/porydaw/releases/tag/1.0.0
