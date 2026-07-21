# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M3 onboarding polish

- **Voicegroup browser: expand keysplits/drumkits** into per-key sub-entries
  (overlaps with the M4 drum lane; the browser currently auditions middle C).
- **Import wizard: per-note voice-type polyphony estimate.** The peak-notes
  warning counts all notes; CGB-voiced notes don't consume PCM channels, and
  with the voicegroup + program map known the estimate could be exact.

## Sample Studio (shipped; design: docs/sample-studio/)

The custom-DirectSound-sample feature shipped (all six phases; SPEC §6.2).
Items explicitly deferred out of it:

- **16-bit .wav output toggle** for users who post-process samples in
  external tools (8-bit chosen for audition/build bit-parity).
- **Offer to install wav2agb** into legacy `.aif`/aif2pcm forks (the
  registrar only refuses actionably there; FORMATS.md appendix A has the
  no-DSP `.aif` fallback design if ever wanted). The optional legacy `.aif`
  *writer* (PLAN.md §6 phase-6 stretch) was likewise not built.
- **Sample rename/delete** management UI (creation and in-place editing
  only, for now — "Edit…" keeps the registered name).

## General things

- **Info Window** an "About porydaw" file menu item should show the user
  general info, such as the porydaw version, the link to GitHub, the version/commit
  of poryaaaa that it's using. Any other pertinent info.

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
- **Upstream `voicegroup_loader_set_log_path()` fix** (SPEC §9): it's
  process-global; should be per-config for a multi-project host.
