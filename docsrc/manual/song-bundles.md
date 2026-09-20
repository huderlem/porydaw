# Sharing Songs with Song Bundles

A song bundle (`.porysong`) is a single file that holds one song together with everything needed to play it: the MIDI, the song's voicegroup, and every instrument sample it uses. Anyone with Porydaw can listen to a bundle without having your decomp project, and can import it into their own project.

A `.porysong` file is a plain zip. Inside, it is laid out like a tiny decomp project (`sound/songs/midi/`, `sound/voicegroups/`, `sound/direct_sound_samples/`, …) plus a `porysong.json` describing the song, so you can inspect one with any zip tool.

## Exporting a bundle

1. Open the song you want to share.
2. Choose **File → Export Song Bundle…** and pick where to save the `.porysong` file.

The bundle holds the song as it currently is in Porydaw, unsaved edits included (just like **Export WAV**).

What goes into the bundle:

- The song's `.mid` and its `midi.cfg` settings (volume, reverb, etc.).
- The song's voicegroup, **trimmed** to the instruments the song actually selects. Every other slot is replaced with a placeholder square-wave voice, and its samples are left out.
- Every sample, programmable wave, keysplit table, drumkit and keysplit voicegroup those instruments need.

What never goes into a bundle: anything in Porydaw's `.porydaw/` folder (view settings, the hi-res sources of samples made in the Sample Studio), and the rest of your project.

Export refuses, and names what is wrong, when:

- An instrument refers to a sample, wave or table that can't be found in the project. (In-game that voice would just be silent, but a bundle has to be complete.)
- A sample the song uses is stored as `.aif`. Convert it to `.wav` first (open and re-save it in the [Sample Studio](sample-studio.md)).

## Listening to a bundle

Open a bundle any of these ways. No decomp project needs to be open.

- **File → Open Song Bundle…**
- Drag a `.porysong` file onto the Porydaw window.
- Pass it on the command line: `porydaw my_song.porysong`

The bundle opens in its own tab, titled `[bundle] <song>`, with a banner across the top. A bundle tab is **read-only**: you can play it, scrub around, browse its tracks and its voicegroup, audition its voices and **Export WAV**, but nothing can be edited or saved. Opening a bundle never writes anything into your project. To edit the song, import it.

Bundle tabs stay open when you switch projects, and they aren't reopened the next time Porydaw starts.

## Importing a bundle into your project

1. Open your decomp project.
2. Open the bundle, then press **Import into project…** in the tab's banner.
3. Review the summary. Change the song's **label**, **constant** and **music player** if you like, then press **Import**.

Porydaw copies the song into the project, registers it like **New Song** does, and opens it in a normal, editable tab. The read-only bundle tab closes, and a message confirms that the song was imported.

!!! warning "Import writes immediately"
    Like a sample import, a bundle import writes straight into your project and is **not undoable**. Commit your project to git first, then review the diff afterwards.

### Reused, added and renamed

Porydaw never makes a second copy of something your project already has. The import summary sorts everything the bundle carries into three groups:

| | When | What happens |
|---|---|---|
| **Reused** | Your project already has the same content (a sample with identical bytes, a keysplit table with the same keys, a drumkit with the same voices, …), whatever it is called. | Nothing is written. The imported voicegroup points at your project's existing one. |
| **Added** | Your project has nothing like it, and the name is free. | It is written under the bundle's name. |
| **Renamed** | The name is taken by something *different*, or doesn't fit your project's naming rules. | It is written as `<name>_2` (then `_3`, …) and the imported voicegroup points at the new name. |

The song's own voicegroup is always added, never reused, because it has been trimmed to this one song. If the song's label is already taken, the import suggests `<label>_2`.

Importing a bundle back into the project it came from therefore reuses every sample and only adds the song and its voicegroup.

### Warnings

Warnings don't stop the import. They tell you about something to check afterwards.

- **Engine extensions.** The song uses m4a engine extensions (for example portamento, `PORTAMENTO`, or the pulse-width commands `PWMC`/`PWMS`) that your project's sound engine doesn't define. The song imports and Porydaw plays it in full, but in-game those commands won't do anything until your project's engine supports them. The warning lists the commands and the tracks that use them.
- **Raw `.bin` samples.** A few voices (`cry` voices) play a raw `.bin` sample rather than a `.wav`. The import copies that `.bin` into `sound/direct_sound_samples/`. Many projects treat `.bin` files there as build leftovers: check that your `.gitignore` and `make clean` won't delete it.
- **A new `keysplit_tables.inc`.** If your project has no `sound/keysplit_tables.inc`, the import creates one, and you need to make sure your build includes it.

### When import refuses

The **Import** button stays disabled, and the dialog says why, when the result could not build or play:

- **Monolithic voicegroups.** Projects that keep every voicegroup in a single `sound/voice_groups.inc` file, without a `sound/voicegroups/` folder (older pokefirered-style layouts), can't be imported into. Porydaw only creates voicegroups as separate files.
- **Legacy `.aif` sample projects.** If your project still builds its samples from `.aif` files, new samples can't be added to it. A bundle whose samples your project *already has* (all reused) still imports.
- **Synth voices without synth support.** The song uses Golden Sun-style synth voices and your project has no `set_synth_*` macros to define them.
- **A programmable wave must be added** and your project has no `sound/programmable_wave_data.inc`.
- The label or constant you typed is already in use, or the bundle is damaged or contains something unsafe.

### A note for older, label-style projects

In projects whose voicegroups are plain labels (`voicegroup005::`) rather than the `voice_group` macro, a drumkit or keysplit voicegroup shorter than 128 voices keeps reading into whatever is assembled after it in the ROM. Bundles exported from such a project include those extra voices so the song sounds the same. When importing *into* such a project, a newly added short drumkit or keysplit voicegroup will likewise run on into whatever your project places after it, so keys that were silent in the original can make a sound. If you hear stray notes on unusual keys, that is why. Modern pokeemerald-style projects don't have this behaviour.

## Bundles and plugins

Plugins can export and import bundles with `porydaw.project.exportBundle(label, path)` and `porydaw.project.importBundle(path, {label, constant, player})`; see the [Scripting API](../reference/scripting.md). While a bundle tab is active, the editing API (`porydaw.edit`, `porydaw.song.save()`, writes to `porydaw.storage.song`) refuses, because the song is read-only; `porydaw.storage.song` reads return the fallback.
