# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## Added
- Added an Output level control (Settings → Audio) for porydaw's output volume.
- Added ability to copy/paste notes and events between different songs. Before, each song had its own separate clipboard.
- Added a Tempo input to the transport bar for the song's starting tempo. A warning appears next to it when the tempo changes later in the song. Click it to show the Tempo automation lane.
- Added vertical panning with `Alt` + mouse scroll.
- Added key rebinding support for all four scroll actions in `Settings -> Keyboard Shortcuts -> Mouse Wheel`.
- Added ability to create a time selection by left-click dragging on the ruler.
- Added `Alt` + drag inside a time selection to move its contents horizontally, and `Ctrl+Alt` + drag to duplicate and move its contents horizontally.

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

[Unreleased]: https://github.com/huderlem/porydaw/compare/1.1.0...HEAD
[1.1.0]: https://github.com/huderlem/porydaw/compare/1.0.0...1.1.0
[1.0.0]: https://github.com/huderlem/porydaw/releases/tag/1.0.0
