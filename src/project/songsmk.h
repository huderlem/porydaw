#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Fallback per-song mid2agb flag storage for projects that predate
// sound/songs/midi/midi.cfg: one make rule per song in <root>/songs.mk, e.g.
//
//   $(MID_SUBDIR)/mus_dp_dawn.s: %.s: %.mid
//   	$(MID) $< $@ -E -R$(STD_REVERB) -G191 -V090
//
// Parsing expands $(VAR) references from the file's own "NAME = value"
// assignments so consumers (SongCfg, mid2agb invocations) see concrete
// flags; writing keeps a flag's variable spelling when its expanded value
// is unchanged.
namespace SongsMk {

QString path(const QString &projectRoot);

// label -> expanded mid2agb flags, one entry per per-song rule whose target
// is "$(MID_SUBDIR)/<label>.s" (or the literal midi-dir path) and whose
// recipe runs $(MID).
QHash<QString, QStringList> parseFlags(const QString &mkPath);

// Rewrites the $(MID) recipe line of the song's rule, or appends a new rule
// when the song has none. Byte-conservative for every other line.
bool writeRule(const QString &mkPath, const QString &label, const QStringList &flags,
               QString *error);

} // namespace SongsMk
