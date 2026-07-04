# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M2 editor polish

- **Add automation lanes for parameters with no events** (SPEC §6.1:
  "addable from the m4a parameter list"). The lane list is built solely
  from events already in the file (songviewmodel.cpp laneFor), so a
  parameter can't be automated from scratch. Needs an "+ Add lane" picker
  offering the §4.2 audible parameters, with the empty lane surviving
  until it gets a point; SongDocument::addLanePoint already handles
  creating the first point for any (track, CC).
- **Edit a track's Voice/Program** (SPEC §4.2 "instrument picker", §6.1
  track-header instrument). Program changes render as read-only markers;
  SongDocument has no edit op for program-change events (insert/modify/
  delete), so a track's instrument can't be changed at all. Needs the
  document op plus a picker listing the voicegroup's entries — the M3
  import wizard's mapping combo already renders exactly that list, and
  AudioEngine::previewVoice can audition the selection.
- **Velocity lane / drag handles.** Velocity is edit-able only via
  right-click → "Set velocity…" on a selection. A per-note drag handle or a
  DAW-style velocity strip under the roll would be quicker for sweeps.
- **Copy/paste of notes** (and paste at playhead), plus Ctrl+A select-all on
  the selected track.
- **`.porydaw` sidecar view state** (SPEC §4.4): per-song zoom, lane
  visibility, last edit position. Specced but not implemented; cosmetic only.
- **Playhead seek** by clicking/dragging in the ruler. Needs a UI→audio seek
  command (TimelinePlayer::seek exists but is only used for edit-time
  timeline swaps); note the engine state after a jump is approximate until
  the next CC/program events replay.
- **Audible draw feedback on double-click.** The preview note for a newly
  drawn note is released on mouse-up, so a quick double-click barely sounds.

## M3 onboarding polish

- **Voicegroup browser: expand keysplits/drumkits** into per-key sub-entries
  (overlaps with the M4 drum lane; the browser currently auditions middle C).
- **Import wizard: per-note voice-type polyphony estimate.** The peak-notes
  warning counts all notes; CGB-voiced notes don't consume PCM channels, and
  with the voicegroup + program map known the estimate could be exact.
- **Import wizard: division rescale option.** Files whose division isn't a
  multiple of 24 are imported as-is (mid2agb quantizes); an optional rescale
  to 24/48 clocks would make the editor grid exact for them.

## Deferred infrastructure

- **GitHub Actions CI** for Win/macOS/Linux builds (SPEC §8 M0 mentions it;
  deliberately deferred — repo is private and CI is unverifiable from WSL).
- **poryaaaa LICENSE file** — release blocker (SPEC §9); porydaw links the
  engine statically and cannot ship a release until poryaaaa declares a
  license upstream.
- **Upstream `voicegroup_loader_set_log_path()` fix** (SPEC §9): it's
  process-global; should be per-config for a multi-project host.
