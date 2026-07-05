# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

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

- **Audio device selection page** (SPEC §7: "device output via miniaudio
  ... with a small device-selection settings page"). The Engine Settings
  dialog (Edit menu) is the natural home; needs ma_context device
  enumeration and a device-switch path in AudioEngine — init() currently
  opens the default device once for the app's lifetime, and sampleRate()
  feeds every built timeline, so switching devices must rebuild the engines
  and the loaded timeline the way a song reload does.

## Deferred infrastructure

- **GitHub Actions CI** for Win/macOS/Linux builds (SPEC §8 M0 mentions it;
  deliberately deferred — repo is private and CI is unverifiable from WSL).
- **poryaaaa LICENSE file** — release blocker (SPEC §9); porydaw links the
  engine statically and cannot ship a release until poryaaaa declares a
  license upstream.
- **Upstream `voicegroup_loader_set_log_path()` fix** (SPEC §9): it's
  process-global; should be per-config for a multi-project host.
