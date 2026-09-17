# Working with Tracks

## Tracks in a GBA song

<!-- TODO: Up to 16 tracks; one instrument sounding per track at a time
(instrument changes via program-change automation); typical layouts in
vanilla songs (melody, harmony, bass, percussion). -->

## The track header

<!-- TODO: Anatomy screenshot: name, instrument readout, M/S buttons,
volume/pan mini-sliders. -->

## Mute and solo

<!-- TODO: Header buttons + M / S keys (rebindable); multi-track scope with
Ctrl/Shift selection; great for picking apart how a vanilla song works. -->

## Track volume and pan

<!-- TODO: The mini-controls edit the track's MIDI volume/pan — these ARE
saved into the song (unlike master volume); relationship to the volume/pan
automation lanes. -->

## Merging tracks

Right-click a track header and choose `Merge...` to fold that track into
another one. Pick the destination track, then choose whether to merge all
events (notes, controllers, pitch bends, and voice changes) or notes only.
The merged track is deleted afterwards. The destination keeps its own starting
voice, and merged notes replace any destination notes they overlap.

This is handy after importing a MIDI file with more tracks than the m4a
engine's limit of 16 (or fewer, depending on the song's music player).

## Renaming tracks

<!-- TODO: Track names, where they're stored, and that they're for your own
organization — the game doesn't see them. -->
