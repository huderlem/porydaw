# Low-latency live play (keyboard monitoring)

2026-10-28. The accuracy engine (poryaaaa f62b8d1) models the GBA's PCM
path: a note keyed now is processed at the next VBlank SoundMain (0–17 ms),
mixed into the DMA ring slot the DMA reads one frame later (17 ms), and
reaches the FIFO/resampler after that (~2.5 ms). PCM keyboard notes sound
18–31 ms late plus the device buffer; PSG notes 1–13 ms. That is faithful
for the song and unusable for playing or recording from a MIDI keyboard.

## Decisions (user, 2026-10-28)

- D1 Live play (keyboard, auditions) goes through a **separate engine** in
  a low-latency mode; the song keeps playing through the main engine,
  bit-exact. Live notes no longer share the song's channel pool (the
  fallback docs/midi-input/PLAN.md §2 already names).
- D2 Low latency applies whenever the user plays by hand: stopped, playing
  along, and recording.
- D3 Accuracy of the live engine does not matter; the least invasive
  mechanism wins.

## Mechanism (poryaaaa: "live mode")

`m4a_engine_set_live_mode(engine, on)`, off by default, vanilla mixer only
(HQ is forced off while live). The frame tick (MPlayMain, CgbSound, the
LFO, the PCM envelope step) still runs once per VBlank. What changes is
*when the PCM ring is mixed*: instead of a whole frame into the next slot
at SoundMain, `m4a_engine_process` mixes the ring **lazily, just ahead of
the DMA's fetch position**, in 16-sample pieces, between its 1024-cycle
scheduler steps. The mixer is already parameterized by its sample count
(SoundInfo.pcmSamplesPerVBlank is just a register), so a piece is a short
frame with the per-call state (count, pointer, fw) carried across.

- A PCM channel keyed between ticks (CHN_START) gets its START envelope
  step at the next piece, so it sounds ~32 samples after the key.
- A PSG channel keyed between ticks gets its CgbSound start pass at the
  next scheduler step (registers written now), instead of the next tick.
- The reverb pass runs per piece over the same range; its second tap is the
  slot after the one being mixed, as SoundMainRAM's.
- PCM latency ≈ 32 samples of look-ahead + the FIFO's 16–32 samples ≈ 4 ms
  at 13379 Hz (1.3 ms at 42048 Hz), plus the device buffer. PSG ≈ 0.1 ms.

Unit tests (test/test_engine.c): piecewise mixing is bit-exact against the
whole-frame mix when nothing is keyed mid-frame; a live-mode note sounds
within 6 ms of the key (PCM and PSG) where the normal mode takes > 15 ms;
live mode forces the vanilla mixer.

## porydaw

- A third engine instance (`m_liveEngine`), in live mode, plays
  *everything the user triggers by hand* — the MIDI/virtual keyboard and
  the timed previews (roll clicks, band sweeps). The main engine plays the
  song only; the preview engine keeps the Sample Editor and voicegroup-dock
  auditions as before (they own fixed tracks there, which a live target
  could collide with).
- The live engine mirrors the song's track state: chase/primeVoices at the
  same points as the main engine, and `TimelinePlayer` dispatches every
  non-note event (CC, program, bend, XCMD, tempo) to a mirror engine as
  the song plays, so the live target's voice, VOL, PAN, MOD... follow the
  song. The live latch's held lanes are skipped for both engines.
- Live CC/bend/program moves go to both engines (the song's own notes on
  the live track still follow the knob, as the take records it).
- The "Auto" recording offset gains the main engine's latency (1.5 VBlank
  frames, the mean of the measured 18–31 ms for PCM), so a take lands where
  the player heard the song; the metronome's clicks are delayed by the same
  amount so they stay with the song.
- Polyphony: live notes no longer count against the song's PCM channels
  (meter, poly debugger); docs say so.
