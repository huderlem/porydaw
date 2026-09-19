#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

// Readers for the sound data sources a song bundle carries and a decomp
// project defines, shared by the bundle exporter and importer. Each mirrors
// what external/poryaaaa/plugin/voicegroup_loader.c reads from the same
// file, so a symbol resolves here exactly when the loader resolves it.
namespace SongBundle::Sources {

// The loader's strip_comment + rtrim + ltrim over one line.
QByteArray contentOf(QByteArray line);

// Label -> .incbin path, first definition wins (symbol_map_find). Labels
// consumed by a set_synth_* macro are not samples and are skipped: a new
// label simply replaces a pending one, as in parse_direct_sound_data_file.
void collectIncbins(const QByteArray &content, QHash<QString, QString> *map);

// The source text of each keysplit table in a keysplit_tables.inc, keyed by
// the symbol voice_keysplit lines use. A table runs from its declaring line
// to the next declaration, minus trailing blank / comment-only / .align
// lines (which belong to whatever follows). First definition wins.
QHash<QString, QList<QByteArray>> collectKeysplitTables(const QByteArray &content);

// What a keysplit table assembles to, whichever form declares it: the
// pokeemerald "keysplit NAME[, start]" + "split index, end" macros, or the
// pokefirered ".set NAME, . - start" + ".byte" lines.
struct KeysplitTable {
    bool macroForm = false; // declared with the keysplit macro
    int offset = 0;         // the symbol sits this many bytes before the data
    QByteArray bytes;       // sub-voicegroup index per key, from key `offset` up

    bool sameContent(const KeysplitTable &o) const
    {
        return offset == o.offset && bytes == o.bytes;
    }
};

// Parses one collectKeysplitTables() block. False for anything but the two
// known forms (the block can then only be carried as text).
bool parseKeysplitTable(const QList<QByteArray> &block, KeysplitTable *table);

// The table's source lines (no line endings) under the given symbol. The
// macro form names its symbol "keysplit_<label>", so symbol must carry that
// prefix when macroForm is set.
QList<QByteArray> renderKeysplitTable(const QString &symbol, const KeysplitTable &table,
                                      bool macroForm);

} // namespace SongBundle::Sources
