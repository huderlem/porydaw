# Sharing Songs with Song Bundles

A song bundle (`.porysong`) is a single file that contains all of the data that makes up a single song: the MIDI, the song's voicegroup, and every instrument sample it uses. Anyone with Porydaw can listen to a `.porysong` song bundle, and can easily import it into their own project.

A `.porysong` file is a plain zip archive. Inside, it is laid out like a tiny decomp project (`sound/songs/midi/`, `sound/voicegroups/`, `sound/direct_sound_samples/`, …) plus a `porysong.json` describing the song, so you can inspect one with any zip tool.

## Exporting a bundle

1. Open the song you want to share.
2. Choose **File → Export Song Bundle…** and pick where to save the `.porysong` file.

The bundle holds the song as it currently is in Porydaw, unsaved edits included (just like **Export WAV**).

What goes into the bundle:

- The song's `.mid` and its `midi.cfg` settings (volume, reverb, etc.).
- The song's voicegroup, **trimmed** to the instruments the song actually selects. Every other slot is replaced with a placeholder square-wave voice, and its samples are left out.
- Every sample, programmable wave, keysplit table, drumkit and keysplit voicegroup those instruments need.

## Previewing and Importing a `.porysong`

Open a bundle in either of these two ways:

- **File → Open Song Bundle…**
- Pass it on the command line: `porydaw my_song.porysong`

The bundle opens in its own song tab, titled `[bundle] <song>`, with a banner across the top. A bundle tab is **read-only**: you can play it to listen, but you can't edit it. Opening a bundle never writes anything into your project. 

To import the song into your own decomp project, press the **Import into project…** button in the tab's banner.

Porydaw copies the song into the project, registers it like **New Song** does, and opens it in a normal, editable tab. The read-only bundle tab closes, and a message confirms that the song was imported.

Porydaw avoids making duplicate data for something your project already has. For example, if your project already contains the song's instrument samples or keysplit voices, it will reuse them instead of duplicating them.

