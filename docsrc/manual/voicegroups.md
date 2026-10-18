# Instruments & Voicegroups

<!-- Likely the most-needed conceptual page after polyphony. Assume the
reader has never heard the word "voicegroup". -->

## What's a voicegroup?

<!-- TODO: The song's instrument bank — a numbered list of up to 128 voices;
a track's "instrument" setting is just an index into this list. Songs can
share voicegroups (edits affect every song using it — warn!). -->

## The voice types

<!-- TODO: Friendly table:
- Sampled (DirectSound) — a recording played at different pitches
- Square 1 / Square 2 — classic chiptune pulse channels
- Programmable wave — drawable waveform channel
- Noise — percussion/hiss channel
- Keysplit — different voices per key range (how drum kits work)
- Percussion — the every-key-is-a-different-sound variant
Which are "the GBA's built-in channels" vs. sampled — link to
[Polyphony](polyphony.md). -->

## Browsing and auditioning voices

<!-- TODO: The voicegroup dock; type icons, ADSR summary; click to audition;
used-by-this-song highlighting; "Show voice in voicegroup" from a note. -->

## Editing a voice

<!-- TODO: The editable fields per type; all edits undoable and saved with
the song (one document, one save). -->

### ADSR envelopes

<!-- TODO: Attack/Decay/Sustain/Release in plain words with a picture;
the GBA's quantized envelope steps; PSG channels have coarser volume
levels than sampled voices. -->

### Choosing a sample or wave

<!-- TODO: The picker button: search filter, keysplit/sample/wave sections,
loop badges with rate/length details, audition-on-highlight through the
voice's own envelope; typing a symbol the scan didn't find still works. -->

### Copy, paste, clear, and variants

<!-- TODO: The voice list's right-click menu (right-click selects the row and
doesn't audition):
- Copy / Paste (Ctrl+C / Ctrl+V with the voice list focused; rebindable in
  Settings → Keyboard Shortcuts) — one voice at a time; Paste replaces the
  whole voice, key and pan included; works on any editable slot.
- Clipboard is plain text: the voice's .inc macro line, so it pastes into a
  text editor, and a line copied out of a .inc file pastes in. Works across
  tabs and between Porydaw windows (synth voices carry their definition).
- When Paste refuses (status-bar message, nothing changes): a sample, wave,
  drumkit, keysplit, or cry the project doesn't have; a macro the project
  doesn't define (e.g. compressed voices in vanilla pokeemerald); a number
  out of range. A keysplit adopts the project's keysplit table.
- Clear — writes the unused-slot placeholder (voice_square_1 60, 0, 0, 2, 0,
  0, 15, 0); no shortcut.
- Variants — Normal / No Resample / Reversed (samples), Normal / Reversed
  (compressed samples), Normal / Detuned (Square 1, Square 2, Wave). Only
  variants the project defines; the envelope, sample, key, and pan stay.
  Reversed writes voice_directsound_alt or voice_directsound_reverse,
  whichever the project defines.
- Undo — each Paste / Clear / variant change is one undo step; view-only
  tabs (song bundles) allow Copy only. -->

## Assigning instruments to tracks

<!-- TODO: How a track picks its voice (program change / instrument
automation); changing instrument mid-song. -->

## Creating and registering voicegroups

<!-- TODO: When you need a new voicegroup vs. editing an existing one;
how New Song / MIDI Import can create one; where the files land. -->
