# porydaw

A simple, cross-platform DAW for Gen 3 Pokémon decompilation projects
(pokeemerald, pokefirered, pokeruby, and their forks).

Point porydaw at your decomp project directory and it loads your voicegroups,
samples, and songs; plays them back GBA-accurately using the embedded
[poryaaaa](https://github.com/huderlem/pory4a) synthesizer engine; and saves
drop-in `.mid` files that your project's existing `mid2agb` build pipeline
consumes unchanged.

porydaw is designed for ROM hackers who aren't DAW power users. If you love
your existing DAW, keep it — the poryaaaa CLAP plugin serves that workflow.

**Status:** early development. See [SPEC.md](SPEC.md) for the full design
specification and roadmap.

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

porydaw is licensed under [GPL-3.0](LICENSE). The embedded poryaaaa engine is
licensed under MIT.
