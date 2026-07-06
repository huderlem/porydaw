#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

#include "project/decompproject.h"
#include "project/songregistry.h"
#include "project/songsmk.h"

// --mkcheck <projectRoot> <song>: songs.mk-fallback check for projects with
// no sound/songs/midi/midi.cfg. Verifies the song's flags parse out of its
// songs.mk rule (with $(VAR) references expanded), that a settings change
// written through SongRegistry::writeSongFlags lands in songs.mk — variable
// spellings and every other line untouched, and no midi.cfg created — that
// the change parses back on a fresh project open, and that an unknown label
// appends a well-formed rule. Run against a scratch copy — it writes into
// the project.

namespace {

QStringList readAllLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
}

} // namespace

int runMkCheck(const QString &projectRoot, const QString &songLabel)
{
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "mkcheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    const QString mkPath = SongsMk::path(projectRoot);
    const QString midiDir = projectRoot + QStringLiteral("/sound/songs/midi");
    if (QFile::exists(QDir(midiDir).filePath(QStringLiteral("midi.cfg")))) {
        std::fprintf(stderr,
                     "mkcheck: project has a midi.cfg — run this against a "
                     "songs.mk-only project\n");
        return 1;
    }
    if (!QFile::exists(mkPath)) {
        std::fprintf(stderr, "mkcheck: no songs.mk at %s\n", qUtf8Printable(mkPath));
        return 1;
    }

    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "mkcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *song = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == songLabel)
            song = &s;
    }
    if (!song) {
        std::fprintf(stderr, "mkcheck: song '%s' not found\n", qUtf8Printable(songLabel));
        return 1;
    }
    check(song->hasCfg, "song has no cfg parsed from songs.mk");
    check(!song->cfg.voicegroupArg.isEmpty(), "voicegroup arg empty");
    check(!song->cfg.rawFlags.join(QLatin1Char(' ')).contains(QLatin1Char('$')),
          "rawFlags still contain unexpanded $(VAR) references");
    std::printf("mkcheck: %s flags: %s\n", qUtf8Printable(songLabel),
                qUtf8Printable(song->cfg.rawFlags.join(QLatin1Char(' '))));

    // A volume change must land in songs.mk, touching only the song's own
    // recipe line.
    const QStringList before = readAllLines(mkPath);
    SongCfg cfg = song->cfg;
    cfg.masterVolume = 111;
    if (!SongRegistry::writeSongFlags(midiDir, songLabel, SongRegistry::mergeCfgFlags(cfg),
                                      &error)) {
        std::fprintf(stderr, "mkcheck: write: %s\n", qUtf8Printable(error));
        return 1;
    }
    check(!QFile::exists(QDir(midiDir).filePath(QStringLiteral("midi.cfg"))),
          "writeSongFlags created a midi.cfg instead of using songs.mk");

    const QStringList after = readAllLines(mkPath);
    if (check(before.size() == after.size(), "songs.mk line count changed")) {
        const QString target = QStringLiteral("/%1.s").arg(songLabel);
        int changed = 0;
        for (int i = 0; i < before.size(); i++) {
            if (before[i] == after[i])
                continue;
            changed++;
            const bool isSongRecipe = i > 0 && before[i - 1].contains(target)
                                      && after[i].contains(QStringLiteral("$(MID)"));
            if (!isSongRecipe) {
                std::fprintf(stderr, "mkcheck: FAIL: unrelated line %d changed: '%s'\n",
                             i + 1, qUtf8Printable(after[i]));
                failures++;
            } else {
                std::printf("mkcheck: recipe now: %s\n", qUtf8Printable(after[i]));
            }
        }
        check(changed == 1, "expected exactly the song's recipe line to change");
    }

    // The change must parse back on a fresh open, with everything else intact.
    DecompProject project2;
    if (!project2.open(projectRoot, &error)) {
        std::fprintf(stderr, "mkcheck: reopen: %s\n", qUtf8Printable(error));
        return 1;
    }
    const SongInfo *song2 = nullptr;
    for (const SongInfo &s : project2.songs()) {
        if (s.label == songLabel)
            song2 = &s;
    }
    if (check(song2 != nullptr, "song missing after reopen")) {
        check(song2->cfg.masterVolume == 111, "volume 111 not persisted");
        check(song2->cfg.voicegroupArg == song->cfg.voicegroupArg,
              "voicegroup arg changed by the volume write");
        check(song2->cfg.reverb == song->cfg.reverb,
              "reverb changed by the volume write");
    }

    // A song whose recipe spells reverb as -R$(STD_REVERB) must keep that
    // spelling through an unrelated (volume) change.
    QString varLabel;
    for (int i = 0; i + 1 < before.size(); i++) {
        if (!before[i + 1].contains(QStringLiteral("-R$(STD_REVERB)")))
            continue;
        const QString text = before[i];
        const int slash = text.indexOf(QStringLiteral(")/"));
        const int dot = text.indexOf(QStringLiteral(".s:"));
        if (slash < 0 || dot <= slash)
            continue;
        varLabel = text.mid(slash + 2, dot - slash - 2);
        break;
    }
    if (varLabel.isEmpty()) {
        std::printf("mkcheck: note: no -R$(STD_REVERB) recipe found, "
                    "spelling-preservation check skipped\n");
    } else {
        const SongInfo *varSong = nullptr;
        for (const SongInfo &s : project2.songs()) {
            if (s.label == varLabel)
                varSong = &s;
        }
        if (check(varSong != nullptr, "no song entry for the $(STD_REVERB) rule")) {
            SongCfg varCfg = varSong->cfg;
            varCfg.masterVolume = 99;
            if (check(SongRegistry::writeSongFlags(midiDir, varLabel,
                                                   SongRegistry::mergeCfgFlags(varCfg),
                                                   &error),
                      "writing the $(STD_REVERB) song failed")) {
                bool kept = false;
                const QStringList lines = readAllLines(mkPath);
                for (int i = 0; i + 1 < lines.size(); i++) {
                    if (lines[i].contains(QStringLiteral("/%1.s").arg(varLabel))
                        && lines[i + 1].contains(QStringLiteral("-R$(STD_REVERB)"))
                        && lines[i + 1].contains(QStringLiteral("-V099")))
                        kept = true;
                }
                check(kept, "-R$(STD_REVERB) spelling not preserved");
                std::printf("mkcheck: %s keeps -R$(STD_REVERB)\n",
                            qUtf8Printable(varLabel));
            }
        }
    }

    // A label with no rule yet appends one that parses back.
    const QString newLabel = QStringLiteral("mus_mkcheck_new");
    const QStringList newFlags = {QStringLiteral("-E"), QStringLiteral("-R50"),
                                  QStringLiteral("-G%1").arg(song->cfg.voicegroupArg),
                                  QStringLiteral("-V100")};
    if (check(SongRegistry::writeSongFlags(midiDir, newLabel, newFlags, &error),
              "appending a new rule failed")) {
        const QHash<QString, QStringList> parsed = SongsMk::parseFlags(mkPath);
        check(parsed.value(newLabel) == newFlags, "appended rule does not parse back");
    }

    std::printf("mkcheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
