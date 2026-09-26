# Importing MIDI Files

<!-- Big audience: people composing in other tools (or downloading MIDIs)
who want them in-game. Set expectations honestly — a MIDI made for a
full General MIDI synth won't sound the same on a GBA. -->

## When to import

<!-- TODO: You composed in another DAW / found a MIDI; Porydaw turns it into
a project song. What a MIDI does and doesn't contain (notes yes, actual
sounds no — instruments come from the voicegroup you pick). -->

## Running the import wizard

<!-- TODO: File → Import MIDI; walk each wizard page in order with
screenshots. -->

## The analysis step

<!-- TODO: What the wizard checks and what each warning means in practice:
- More than 16 channels/tracks — what gets dropped or merged
- Polyphony budget — chords the GBA can't hold (link to Polyphony)
- Unmapped/unsupported CCs — which controls the GBA engine understands
- Format conversion notes (SMF format 0 vs 1)
-->

## Automatic cleanup

<!-- TODO: One friendly paragraph: exported MIDIs often stack redundant
setup events; the wizard quietly drops same-tick duplicates and keeps the
one that would actually sound. Notes and loop/text markers are never
touched. Reassure: your original .mid file is never modified. -->

## Choosing instruments

<!-- TODO: Mapping MIDI programs to a voicegroup; starting from a vanilla
voicegroup vs. building one; drum-channel handling (channel 10). -->

## After the import

The imported song opens as a [draft](new-song.md#draft-songs). Nothing is written to your project until you save it, so you can listen to how a MIDI converts and simply close the tab (choosing **Discard**) if you don't want to keep it.

<!-- TODO: The song is registered like a New Song (link); a listen-through
checklist: polyphony meter, missing percussion, volume balance, loop point. -->

## Tips for MIDIs that convert well

<!-- TODO: Compose ≤ 16 tracks / one instrument per track, mind chord
sizes, set a loop, avoid dense CC automation, etc. -->
