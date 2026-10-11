# Vendored MIDI input library

Covers `external/rtmidi/`.

| File | Project | Version | Upstream release | License |
|---|---|---|---|---|
| RtMidi.h, RtMidi.cpp | [thestk/rtmidi](https://github.com/thestk/rtmidi) | 6.0.0 | tag `6.0.0`, fetched from `https://raw.githubusercontent.com/thestk/rtmidi/6.0.0/` 2026-10-11 (sha256 `RtMidi.h` `74f98ca172243b0b9c0c59f53c4e20285d879ffa86b3d61692109b013f4606f9`, `RtMidi.cpp` `184d775e88e5d73b079897363fb42af155122e879f0bc9254a1c7edae4eadb3c`) | MIT-style (see `LICENSE`) |

Upstream files with one porydaw patch (marked `porydaw patch` in
`RtMidi.cpp`): `MidiInWinMM::closePort` no longer returns half-closed when
`midiInUnprepareHeader` fails, which is what happens when the device was
unplugged. Upstream freed sysex buffer 0, kept `connected_` true and the
critical section held, and the destructor's second `closePort()` then
double-freed it, so every unplug crashed. The sha256 values above are of
the unpatched upstream files. Build configuration lives in `CMakeLists.txt`
(option `PORYDAW_MIDI`): one backend define per platform —
`__WINDOWS_MM__` (links `winmm`), `__MACOSX_CORE__` (links CoreMIDI,
CoreAudio, CoreFoundation), `__LINUX_ALSA__` (links ALSA; needs
`libasound2-dev`) — falling back to `__RTMIDI_DUMMY__` when the platform
backend is missing. The only consumer is `src/audio/midiinput.cpp`.
