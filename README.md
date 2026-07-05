# Porydaw

It's like [Porymap](https://github.com/huderlem/porymap), but for music.  An easy-to-use DAW for Gen 3 Pokémon decompilation projects ([pokeemerald][pokeemerald], [pokefirered][pokefirered], and [pokeruby][pokeruby]).

In Porydaw, load your decomp project directory to load the music-related project data. Then, play, edit, and create music. It sounds just like it does in-game.  When saving, Porydaw writes and creates the necessary files directly into the decomp project.  It also supports importing MIDI files, making it easy to whip up songs and voicegroups for brand new songs.

Porydaw is designed for people who *aren't* DAW power users--maybe they've used tools like Sappy and Anvil Studio for their music needs. If you love
your existing DAW (FL Studio, Reaper, etc.), keep using it--the [poryaaaa CLAP plugin](https://github.com/huderlem/poryaaaa) serves that power-user workflow.


## Building

Requires CMake 3.21+, a C11/C++17 compiler, and Qt 6 (Widgets).

```sh
git clone --recursive https://github.com/huderlem/porydaw.git
cd porydaw
cmake -B build
cmake --build build
```

If you cloned without `--recursive`, run `git submodule update --init` first.

## License

Porydaw is licensed under [GPL-3.0](LICENSE). The embedded poryaaaa engine is
licensed under MIT.

## AI Disclaimer

Porydaw's code has been built with heavy usage of Claude Code--bootstrapped with the Fable model.

[pokeruby]: https://github.com/pret/pokeruby
[pokeemerald]: https://github.com/pret/pokeemerald
[pokefirered]: https://github.com/pret/pokefirered
