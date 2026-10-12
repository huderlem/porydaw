# Plugins

Porydaw can be customized and extended with JavaScript plugins. A plugin can do many things, including but not limited to:

- Define custom commands triggered by keyboard shortcuts
- Add actions to the **Plugins** menu
- Add actions when right clicking on piano roll notes
- Add new visual and interactable panels
- Read and edit every aspect of a song
- React to audio playback in real time
- Draw overlays onto the piano roll
- Show custom dialogs

## Installing an existing plugin

1. Open **Edit → Settings… → Plugins**.
    - This page shows the plugins folder and a button to open it. Note, you can change the plugins folder to your liking.
2. Copy the plugin's folder into the configured porydaw plugins folder.
    - A plugin is one folder holding a `plugin.json` and a `main.js` (the folder name must exactly match the id in `plugin.json`).
3. The plugin will immediately load. Uncheck it in the same settings page to disable it.  The page also shows why a plugin failed to load, if it did.

Plugins automatically reload when their files are modified, so editing one while Porydaw runs is fine.

!!! tip "Where the folder is"
    By default the plugins folder lives in Porydaw's application-data directory.  **Change…** on the Settings page points Porydaw at any folder you like. **Use Default** resets it to the application-data directory.

## Bundled plugin examples

Porydaw has a bunch of example plugins in its  [`plugins/examples`](https://github.com/huderlem/porydaw/blob/main/plugins/examples) folder on GitHub. Copy any of them into your plugins folder to try them. They are decent references to see what the scripting API can do, as well as nice code references.

| Plugin | What it adds |
|---|---|
| Arpeggiate | the plugin from the [tutorial](writing-plugins.md): a keyboard shortcut and right-click menu action that turns a selected chord into an arpeggio |
| Select Same Pitch | a shortcut that selects every note with the selected note's pitch |
| Note Tools | Legato, Insert chord, Humanize, Strum and Quantize commands |
| VU Meter, Spectrum, Dancer | Visualization panels that follow the audio and the beat |
| MIDI Monitor | a panel that shows what your MIDI keyboard sends: its ports, the keys you hold, bend, mod wheel, sustain and a log of the latest messages |
| Song Report | **Plugins → Song Report**: a text summary of the song, and a WAV render |
| Range Tools | right-click a time selection: duplicate it after itself, echo it with fading copies, reverse its notes |
| Scale Guide | shades the piano-roll rows outside the selected scale |
| Project Tools | **Plugins → Project Tools**: registers every song whose registration files miss a line, and applies one envelope to all sample voices of the song's voicegroup |

## Shortcuts, menus and panels

- Plugin commands appear in **Settings → Keyboard Shortcuts** under the plugin's name and can be rebound to different keys, just like any other command.
- Plugin panels are listed under **View → Plugin Panels**.
- **View → Script Console** shows plugin output, including errors, and it offers a small interactive REPL for trying API calls.

## Safety

Plugins run inside Porydaw with the same access to your project as Porydaw itself, so only install plugins you trust. A plugin that hangs (e.g. due to an infinite loop) is stopped after a few seconds and disabled until you reload it. File access from a plugin is sandboxed to:

1. Its own folder
2. The open project's directory
3. Any files you have selected in a dialog presented by the plugin.

## Writing your own plugin

Start with the [tutorial](writing-plugins.md). It builds a working plugin in a few short steps. The [Scripting API reference](../reference/scripting.md) then documents everything a plugin can do. You can optionally use the type declarations in [`docs/scripting/porydaw.d.ts`](https://github.com/huderlem/porydaw/blob/main/docs/scripting/porydaw.d.ts) for editor autocomplete or even authoring plugins in TypeScript.
