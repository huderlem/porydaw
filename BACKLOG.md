# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M2 editor polish

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
- **`.porydaw` sidecar view state** (SPEC §4.4): per-song zoom, lane
  visibility, last edit position. Specced but not implemented; cosmetic only.

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
