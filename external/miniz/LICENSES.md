# Vendored zip library

Covers `external/miniz/`.

| File | Project | Version | Upstream release | License |
|---|---|---|---|---|
| miniz.h, miniz.c | [richgel999/miniz](https://github.com/richgel999/miniz) | 3.1.2 | `miniz-3.1.2.zip` release amalgamation (sha256 `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`), 2026-09-17 | MIT (see `LICENSE`) |

Build configuration lives in `CMakeLists.txt` (compile definitions on
`porydaw_app`): `MINIZ_NO_STDIO` (porydaw does all file I/O through Qt and
feeds miniz memory buffers), `MINIZ_NO_TIME` (bundle entries carry a fixed
timestamp so re-exports are byte-identical), and
`MINIZ_NO_ZLIB_COMPATIBLE_NAMES` (Qt may link a real zlib).
