# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M2 editor polish

- **Velocity lane / drag handles.** Velocity is edit-able only via
  right-click → "Set velocity…" on a selection. A per-note drag handle or a
  DAW-style velocity strip under the roll would be quicker for sweeps.
- **Copy/paste of notes** (and paste at playhead), plus Ctrl+A select-all on
  the selected track.
- **Voicegroup dropdown in Song Settings.** The -G field is a free-text line
  edit; it should list the project's voicegroups (natural to do together with
  the M3 voicegroup browser, which needs the same enumeration).
- **`.porydaw` sidecar view state** (SPEC §4.4): per-song zoom, lane
  visibility, last edit position. Specced but not implemented; cosmetic only.
- **Playhead seek** by clicking/dragging in the ruler. Needs a UI→audio seek
  command (TimelinePlayer::seek exists but is only used for edit-time
  timeline swaps); note the engine state after a jump is approximate until
  the next CC/program events replay.
- **Audible draw feedback on double-click.** The preview note for a newly
  drawn note is released on mouse-up, so a quick double-click barely sounds.

## Deferred infrastructure

- **GitHub Actions CI** for Win/macOS/Linux builds (SPEC §8 M0 mentions it;
  deliberately deferred — repo is private and CI is unverifiable from WSL).
- **poryaaaa LICENSE file** — release blocker (SPEC §9); porydaw links the
  engine statically and cannot ship a release until poryaaaa declares a
  license upstream.
- **Upstream `voicegroup_loader_set_log_path()` fix** (SPEC §9): it's
  process-global; should be per-config for a multi-project host.
