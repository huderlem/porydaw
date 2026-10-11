# Porydaw

[![Actions Status](https://github.com/huderlem/porydaw/workflows/Build/badge.svg)](https://github.com/huderlem/porydaw/actions)

A music editor for the Pokémon generation 3 decompilation projects ([pokeruby][pokeruby], [pokeemerald][pokeemerald], and [pokefirered][pokefirered]).

In Porydaw, load your decomp project directory to load the music-related project data. Then, play, edit, and create music. It sounds just like it does in-game.  When saving, Porydaw writes and creates the necessary files directly into the decomp project.  It also supports importing MIDI files, making it easy to whip up songs and voicegroups for brand new songs.

Porydaw is designed for both music beginners and power users who are familiar with DAW programs.  If you've used Sappy or Anvil Studio for your musical needs in the past, then Porydaw is for you!  If you're a power user who loves your existing DAW (FL Studio, Reaper, etc.), give Porydaw a try--but if you can't be pulled away, the [poryaaaa CLAP plugin](https://github.com/huderlem/poryaaaa) helps serve that power-user workflow.

View the work-in-progress documentation: https://huderlem.github.io/porydaw

View the [Changelog][changelog] to see what's new.

<img src="docsrc/img/introduction-porydaw-screen.png" width="600" alt="Porydaw main window">

## Download

Download Porydaw below to start using it immediately. Older versions of Porydaw may be downloaded from the [Releases][releases] page.

 - [Download Porydaw for Windows](https://github.com/huderlem/porydaw/releases/latest/download/porydaw-windows.zip).
 - [Download Porydaw for macOS (arm)](https://github.com/huderlem/porydaw/releases/latest/download/porydaw-macos-arm64.zip).
 - [Download Porydaw for macOS (intel)](https://github.com/huderlem/porydaw/releases/latest/download/porydaw-macos-x86_64.zip).
 - [Download Porydaw for Linux](https://github.com/huderlem/porydaw/releases/latest/download/porydaw-linux.zip) (AppImage).

Read [INSTALL.md](INSTALL.md) for instructions on how to compile Porydaw from source.

## Contributing

Code formatting is enforced with clang-format (major version 22 — output differs between major versions) using the repo's [.clang-format](.clang-format). Before opening a pull request, run:

```bash
tools/format.sh          # reformat src/ and tools/ in place
tools/format.sh --check  # what CI runs
```

## License

Porydaw is licensed under [GPL-3.0](LICENSE). The embedded poryaaaa engine is
licensed under MIT. MIDI keyboard input uses [RtMidi](https://github.com/thestk/rtmidi)
(MIT-style license, see [external/rtmidi/LICENSE](external/rtmidi/LICENSE)).

## AI Disclaimer

Porydaw's code has been built with heavy usage of Claude Code--bootstrapped with the Fable model.

[pokeruby]: https://github.com/pret/pokeruby
[pokeemerald]: https://github.com/pret/pokeemerald
[pokefirered]: https://github.com/pret/pokefirered
[changelog]: https://github.com/huderlem/porydaw/blob/main/CHANGELOG.md
[releases]: https://github.com/huderlem/porydaw/releases
