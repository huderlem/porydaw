# Backlog

Small deferred items that don't warrant a SPEC.md change. Milestone scope
lives in SPEC.md §8; this is the loose-ends list between milestones.

## M2 polish

- **time signature** - the user should be able to see and edit the time signature
  on the timeline. This should exist as some kind of marker in the ruler area, not
  as an automation lane.
- **ruler subdivisions** - the user should have greater editing precision by being
  able to select "triplets" or "straight". The user should also be able to specify
  the minimum ruler subdivision (1 is the most granular). For example, the user might
  only care about drawing in quarter notes, so they'd want to select a less-granular
  sub-division.

## M3 onboarding polish

- **Voicegroup browser: expand keysplits/drumkits** into per-key sub-entries
  (overlaps with the M4 drum lane; the browser currently auditions middle C).
- **Import wizard: per-note voice-type polyphony estimate.** The peak-notes
  warning counts all notes; CGB-voiced notes don't consume PCM channels, and
  with the voicegroup + program map known the estimate could be exact.

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
