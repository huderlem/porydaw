# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M2 editor polish

- **`.porydaw` sidecar view state** (SPEC §4.4): per-song zoom, lane
  visibility, last edit position, roll/lanes splitter position. Specced but
  not implemented; cosmetic only.
- **Per-lane height handle.** The lanes area as a whole is resizable via the
  roll/lanes QSplitter, but individual rows are fixed kLaneH = 48; a drag
  handle per row (persisted in the `.porydaw` sidecar) would let a bend lane
  be taller than a volume lane.

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
