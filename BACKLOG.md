# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M2 editor polish

- **`.porydaw` sidecar view state** (SPEC §4.4): per-song zoom, lane
  visibility, last edit position. Specced but not implemented; cosmetic only.
- **Reaper-style transport: edit cursor + spacebar play/pause toggle.**
  Two cursors, as in Reaper: an *edit cursor* placed by clicking in the
  ruler/roll, distinct from the moving *playback cursor*. Spacebar toggles
  play/pause and playback starts from the edit cursor (today Space is bound
  to the Play action only, and playing from Stopped always resets to 0).
  Needs a UI→audio seek/start-at command (TimelinePlayer::seek exists but
  is only used for edit-time timeline swaps), the edit-cursor rendering in
  the ruler/roll, and the Space rebind; pair the jump with
  TimelinePlayer::chase (already used by edit-time swaps) so CC/program/
  bend state is exact at the landing position.
- **Audible draw feedback on double-click.** The preview note for a newly
  drawn note is released on mouse-up, so a quick double-click barely sounds.
- **Right-click drag for rubber-band selection.** Multi-select is currently
  left-drag on empty space (Drag::Band in songview.cpp); move it to
  right-drag, freeing plain left-click for note drawing. (Right-click today
  deletes/context-acts on notes — the two need reconciling, e.g. right-click
  = context/delete, right-drag = band.)
- **Left-click draws a note, hold-and-drag sizes it.** Drawing currently
  requires a double-click (mouseDoubleClickEvent), and the new note's length
  is fixed until a separate right-edge resize. Reaper/FL-style instead:
  left-press on empty space creates the note at the snapped tick, and
  dragging before release sets its duration (audition already previews
  during draw).
- **Resize notes from the left edge too.** Only the right edge is a resize
  handle (nearRightEdge, songview.cpp:423,457); a left-edge handle should
  move the note-on while pinning the note-off (adjusting tick + duration
  together).
- **Overhaul mouse-wheel behavior.** Today on the roll: plain scroll =
  vertical note-range scroll, Shift = horizontal scroll, Ctrl = horizontal
  zoom (wheelEvent, songview.cpp). Wanted, Reaper-style: (A) plain scroll
  over the notes area = horizontal zoom (ruler granularity); (B) Ctrl+scroll
  over the notes area = vertical zoom — row/key height, the analog of
  Reaper's track-height scaling; (C) plain scroll over the piano-key column
  keeps today's note-range scrolling. Horizontal scroll stays on Shift (and
  trackpad horizontal delta).
- **Finer-grained automation editing + multi-point drag.** Lane edits are
  one point per click and snap to the clock grid; there's no way to sweep a
  curve. Add freehand drag across a lane writing a stream of points (thinned
  to the grid, replacing overwritten ones — one undo command for the whole
  gesture), plus a line-segment drag for ramps. Also allow finer time
  placement when the grid is coarse (the document supports any tick;
  mid2agb quantizes to clocks on compile, so sub-clock points are mostly
  useful with -X / higher divisions).
- **Vertically resizable automation area.** The lanes sit in a QScrollArea
  with a fixed height (kLanesAreaH, songview.cpp buildUi) under the roll, so
  they can never be enlarged. Put the roll and the lanes area in a vertical
  QSplitter so the user can drag the boundary; consider a per-lane height
  handle too (rows are fixed kLaneH = 48). Persist the split (and lane
  heights) in the `.porydaw` sidecar view state once that lands.
- **Sub-beat ruler/grid subdivisions.** SongView::forEachGridLine enumerates
  beats only, so the ruler ticks and the roll's vertical grid bottom out at
  quarter notes at any zoom — even though editing snaps to the finer
  ticksPerClock grid. Add zoom-adaptive subdivisions (8th/16th… down to the
  mid2agb clock grid, the real snap resolution) fading in the way beat lines
  already gate on pxPerBeat >= 10, with lighter strokes per level.

## M3 onboarding polish

- **Voicegroup browser: expand keysplits/drumkits** into per-key sub-entries
  (overlaps with the M4 drum lane; the browser currently auditions middle C).
- **Import wizard: per-note voice-type polyphony estimate.** The peak-notes
  warning counts all notes; CGB-voiced notes don't consume PCM channels, and
  with the voicegroup + program map known the estimate could be exact.
- **Import wizard: division rescale option.** Files whose division isn't a
  multiple of 24 are imported as-is (mid2agb quantizes); an optional rescale
  to 24/48 clocks would make the editor grid exact for them.

## App settings

- **Engine settings page: view/modify global poryaaaa knobs** (SPEC §7:
  "GBA-accuracy knobs surfaced in app settings, defaulted to
  hardware-accurate"). maxPcmChannels (polyphony, default 5) and pcmMixRate
  (13379 Hz) are hardcoded SongSettings defaults in audioengine.h — only the
  polyphony meter even reads the limit — and the engine's analogFilter
  toggle isn't surfaced at all. Needs an app settings dialog persisting via
  QSettings, applied through AudioEngine::updateSettings (which already
  handles mix-rate changes) and mirrored to the preview engine; SPEC §7's
  audio-device selection page is a natural roommate. Reverb stays per-song
  (midi.cfg -R), not global.

## Deferred infrastructure

- **GitHub Actions CI** for Win/macOS/Linux builds (SPEC §8 M0 mentions it;
  deliberately deferred — repo is private and CI is unverifiable from WSL).
- **poryaaaa LICENSE file** — release blocker (SPEC §9); porydaw links the
  engine statically and cannot ship a release until poryaaaa declares a
  license upstream.
- **Upstream `voicegroup_loader_set_log_path()` fix** (SPEC §9): it's
  process-global; should be per-config for a multi-project host.
