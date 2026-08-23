#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

#include "core/midiimport.h"
#include "core/smf.h"
#include "core/songdocument.h"
#include "mainwindow.h"
#include "project/decompproject.h"
#include "project/songregistry.h"
#include "ui/newsongwizard.h"
#include "ui/songlistpanel.h"
#include "ui/songsettingsdialog.h"

// --onboardcheck <projectRoot> [mid2agbPath]: M3 onboarding check. Exercises
// the New Song and Import backends headlessly against a scratch copy of a
// project — it writes into it. Creates a song, verifies its files and sidecar,
// registers it (porydaw writes song_table.inc / songs.h / ld_script.ld /
// charmap.txt / src/debug.c directly), verifies idempotency and stale-ID
// correction, and runs an external-MIDI import (analysis + division rescale),
// compiling both songs through the project's real mid2agb.

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "onboardcheck: FAIL: %s\n", what);
        g_failures++;
    }
}

QByteArray readAllBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool compilesThroughMid2agb(const QString &mid2agb, const QString &midPath,
                            const QStringList &flags)
{
    QProcess proc;
    const QString outS = midPath.left(midPath.size() - 4) + ".s";
    proc.start(mid2agb, QStringList() << flags << midPath << outS);
    proc.waitForFinished(15000);
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        std::fprintf(stderr, "onboardcheck: mid2agb: %s\n",
                     qUtf8Printable(QString::fromLocal8Bit(proc.readAllStandardError())));
        return false;
    }
    return QFileInfo(outS).size() > 0;
}

// A plausible external MIDI: format 1, division 400 (not a multiple of 24, to
// trip the quantization warning), tempo track plus two instrument tracks with
// programs, audible + inert CCs, and a chord thick enough to trip the
// polyphony warning.
SmfFile makeExternalMidi()
{
    SmfFile smf;
    smf.format = 1;
    smf.division = 400;

    SmfTrack tempo;
    SmfEvent t;
    t.status = 0xFF;
    t.metaType = 0x51;
    t.blob = QByteArray("\x06\x1A\x80", 3); // 150 BPM
    tempo.events.push_back(t);
    tempo.endTick = 480 * 8;
    smf.tracks.push_back(tempo);

    const auto channelEvent = [](uint8_t status, uint64_t tick, uint8_t d0, uint8_t d1) {
        SmfEvent ev;
        ev.status = status;
        ev.tick = tick;
        ev.data0 = d0;
        ev.data1 = d1;
        return ev;
    };

    SmfTrack lead; // channel 0: program 5, mod + volume + an inert CC 91
    lead.events.push_back(channelEvent(0xC0, 0, 5, 0));
    lead.events.push_back(channelEvent(0xB0, 0, 7, 110));
    lead.events.push_back(channelEvent(0xB0, 0, 1, 20));
    lead.events.push_back(channelEvent(0xB0, 0, 91, 64));
    // A 7-note chord: peak polyphony above the 5-channel PCM budget. Ticks
    // must be non-decreasing within the track, so all ons precede all offs.
    for (int i = 0; i < 7; i++)
        lead.events.push_back(channelEvent(0x90, 480, uint8_t(60 + i), 100));
    for (int i = 0; i < 7; i++)
        lead.events.push_back(channelEvent(0x80, 960, uint8_t(60 + i), 0));
    lead.endTick = 480 * 8;
    smf.tracks.push_back(lead);

    SmfTrack bass; // channel 1: two programs, so the mapping table has rows
    bass.events.push_back(channelEvent(0xC1, 0, 20, 0));
    bass.events.push_back(channelEvent(0x91, 0, 36, 90));
    bass.events.push_back(channelEvent(0x81, 480, 36, 0));
    bass.events.push_back(channelEvent(0xC1, 960, 33, 0));
    bass.events.push_back(channelEvent(0x91, 960, 40, 90));
    bass.events.push_back(channelEvent(0x81, 1440, 40, 0));
    bass.endTick = 480 * 8;
    smf.tracks.push_back(bass);
    return smf;
}

} // namespace

int runOnboardCheck(const QString &projectRoot, const QString &mid2agbPath)
{
    QString error;
    DecompProject project;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "onboardcheck: %s\n", qUtf8Printable(error));
        return 1;
    }
    // Only song_table entries count toward the proposed ID; the project may
    // already contain stray unregistered .mid files.
    int registeredCount = 0;
    for (const SongInfo &s : project.songs())
        registeredCount += s.registered ? 1 : 0;
    const QString midiDir = projectRoot + QStringLiteral("/sound/songs/midi");

    QString mid2agb = mid2agbPath;
    if (mid2agb.isEmpty())
        mid2agb = projectRoot + QStringLiteral("/tools/mid2agb/mid2agb");
    const bool haveMid2agb = QFileInfo::exists(mid2agb);
    if (!haveMid2agb)
        std::printf("onboardcheck: note: mid2agb not found, compile checks skipped\n");

    // ---- Project enumeration ------------------------------------------------
    const QStringList vgArgs = SongRegistry::voicegroupArgs(projectRoot);
    check(!vgArgs.isEmpty(), "no voicegroups enumerated");
    std::printf("onboardcheck: %d voicegroups, e.g. %s\n", int(vgArgs.size()),
                vgArgs.isEmpty() ? "-" : qUtf8Printable(vgArgs.first()));
    const QVector<MusicPlayer> players = SongRegistry::musicPlayers(projectRoot);
    check(!players.isEmpty(), "no music players parsed from song_table.inc");

    // ---- Music-player track budgets -----------------------------------------
    // Deterministic fixture regardless of the checkout: swap in a known
    // music_player_table.inc, assert parsing + budget resolution, restore.
    {
        const QString tablePath = projectRoot + QStringLiteral("/sound/music_player_table.inc");
        const QByteArray original = readAllBytes(tablePath);
        QFile table(tablePath);
        check(table.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "rewrite music_player_table.inc fixture");
        // BGM overridden to 12 via equiv, SE1 literal, SE2 clamped from a
        // NUM_TRACKS beyond the engine's 16, SE3 via an unknown symbol.
        table.write("\t.equiv NUM_TRACKS_BGM, 12\n"
                    "\t.equiv NUM_TRACKS_SE2, 20\n\n"
                    "gMPlayTable::\n"
                    "\tmusic_player gMPlayInfo_BGM, gMPlayTrack_BGM, NUM_TRACKS_BGM, 0\n"
                    "\tmusic_player gMPlayInfo_SE1, gMPlayTrack_SE1, 3, 1\n"
                    "\tmusic_player gMPlayInfo_SE2, gMPlayTrack_SE2, NUM_TRACKS_SE2, 1\n"
                    "\tmusic_player gMPlayInfo_SE3, gMPlayTrack_SE3, NUM_TRACKS_WHO, 0\n");
        table.close();

        const QVector<MusicPlayer> budgeted = SongRegistry::musicPlayers(projectRoot);
        auto countFor = [&budgeted](const QString &name) {
            for (const MusicPlayer &p : budgeted) {
                if (p.name == name)
                    return p.trackCount;
            }
            return -2;
        };
        check(countFor(QStringLiteral("MUSIC_PLAYER_BGM")) == 12,
              "BGM budget follows the project's NUM_TRACKS override");
        check(countFor(QStringLiteral("MUSIC_PLAYER_SE1")) == 3, "literal track count parsed");
        check(countFor(QStringLiteral("MUSIC_PLAYER_SE2")) == 16,
              "budget clamped to the engine's 16 like MPlayOpen");
        check(countFor(QStringLiteral("MUSIC_PLAYER_SE3")) == -1,
              "unresolvable count stays unknown");

        DecompProject budgetProject;
        check(budgetProject.open(projectRoot, &error), "reopen for budgets");
        SongInfo bgmSong;
        bgmSong.player = QStringLiteral("MUSIC_PLAYER_BGM");
        check(budgetProject.trackBudgetFor(bgmSong) == 12,
              "trackBudgetFor resolves the song's player");
        bgmSong.player = QStringLiteral("MUSIC_PLAYER_SE3");
        check(budgetProject.trackBudgetFor(bgmSong) == 16,
              "unknown budget falls back to the engine ceiling");

        if (original.isEmpty()) {
            QFile::remove(tablePath);
        } else {
            check(table.open(QIODevice::WriteOnly | QIODevice::Truncate),
                  "restore music_player_table.inc");
            table.write(original);
            table.close();
        }
    }

    // ---- New Song flow ------------------------------------------------------
    const QString label = QStringLiteral("mus_onboardcheck");
    const QString constant = SongRegistry::constantForLabel(label);
    check(constant == QStringLiteral("MUS_ONBOARDCHECK"), "constantForLabel");

    SongCfg cfg;
    cfg.exactGate = true;
    cfg.reverb = 50;
    cfg.masterVolume = 100;
    cfg.voicegroupArg = vgArgs.isEmpty() ? QStringLiteral("_dummy") : vgArgs.first();
    cfg.rawFlags = SongRegistry::mergeCfgFlags(cfg);

    const SmfFile blank = SongRegistry::blankSong();
    const QString midPath = midiDir + QStringLiteral("/%1.mid").arg(label);
    check(blank.writeFile(midPath, &error), "write blank .mid");
    check(SongRegistry::writeMidiCfgLine(midiDir, label, cfg.rawFlags, &error),
          "write midi.cfg line");
    check(SongRegistry::saveRegistrationMeta(projectRoot, label, constant,
                                             QStringLiteral("MUSIC_PLAYER_BGM")),
          "save sidecar meta");

    // The new song must surface on reload: unregistered, playable, cfg parsed.
    check(project.reload(&error), "project reload after create");
    const SongInfo *created = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == label)
            created = &s;
    }
    check(created != nullptr, "created song not discovered on reload");
    if (created) {
        check(!created->registered, "created song should be unregistered");
        check(created->isPlayable(), "created song not playable");
        check(created->hasCfg && created->cfg.voicegroupArg == cfg.voicegroupArg,
              "created song cfg line not parsed back");
        check(created->constant == constant &&
                  created->player == QStringLiteral("MUSIC_PLAYER_BGM"),
              "sidecar registration meta not recalled");

        SongDocument doc;
        check(doc.load(*created, &error), "created song fails to open as a document");
    }

    RegistrationStatus status = SongRegistry::checkRegistration(projectRoot, label, constant);
    check(!status.inSongTable && !status.inSongsH && !status.inCharmap && !status.complete(),
          "fresh song already looks registered");

    RegistrationPlan plan =
        SongRegistry::makePlan(projectRoot, label, constant, QStringLiteral("MUSIC_PLAYER_BGM"));
    check(plan.songId == registeredCount, "proposed song ID != registered song count");
    check(plan.songTableLine.contains(QStringLiteral("song mus_onboardcheck, MUSIC_PLAYER_BGM, 0")),
          "song_table line malformed");
    check(plan.songsHLine.startsWith(QStringLiteral("#define MUS_ONBOARDCHECK")) &&
              plan.songsHLine.endsWith(QString::number(plan.songId)),
          "songs.h line malformed");

    // charmap.txt: the constant maps to the ID as little-endian hex bytes.
    const QString charmapPath = projectRoot + QStringLiteral("/charmap.txt");
    const QString charmapBytes = QStringLiteral("%1 %2")
                                     .arg(plan.songId & 0xFF, 2, 16, QLatin1Char('0'))
                                     .arg((plan.songId >> 8) & 0xFF, 2, 16, QLatin1Char('0'))
                                     .toUpper();
    check(plan.charmapApplicable, "charmap.txt song section not detected");
    check(plan.charmapLine.startsWith(constant) &&
              plan.charmapLine.endsWith(QStringLiteral("= ") + charmapBytes),
          "charmap line malformed");

    // A column-aligned sound section (pokeruby, pokefirered) pads "=" into a
    // shared column, and non-song two-byte entries don't disturb the anchor
    // or the alignment. Fixture-swap a tiny aligned charmap and re-plan.
    {
        const QByteArray original = readAllBytes(charmapPath);
        const QByteArray fixtureLine = "MUS_DUMMY                 = 00 00";
        QFile cm(charmapPath);
        check(cm.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite charmap.txt fixture");
        cm.write(fixtureLine + "\n"
                               "MUS_LITTLEROOT_TEST       = 5E 01\n"
                               "PKMN = 53 54\n");
        cm.close();
        const RegistrationPlan aligned = SongRegistry::makePlan(projectRoot, label, constant,
                                                                QStringLiteral("MUSIC_PLAYER_BGM"));
        check(aligned.charmapApplicable, "aligned fixture: section not detected");
        const int equalsColumn = fixtureLine.indexOf('=');
        check(aligned.charmapLine == constant +
                                         QString(equalsColumn - constant.size(), QLatin1Char(' ')) +
                                         QStringLiteral("= ") + charmapBytes,
              "aligned fixture: charmap line not padded to the '=' column");
        check(cm.open(QIODevice::WriteOnly | QIODevice::Truncate), "restore charmap.txt");
        cm.write(original);
        cm.close();
    }

    // porydaw writes the registration files itself.
    QString regError;
    int songId = -1;
    check(SongRegistry::registerSong(projectRoot, label, constant,
                                     QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
          "registerSong failed");
    if (!regError.isEmpty())
        std::fprintf(stderr, "onboardcheck: registerSong: %s\n", qUtf8Printable(regError));
    check(songId == registeredCount, "registered song ID != registered song count");

    status = SongRegistry::checkRegistration(projectRoot, label, constant);
    check(status.complete(), "registration incomplete after registerSong");

    const QString tablePath = projectRoot + QStringLiteral("/sound/song_table.inc");
    const QString songsHPath = projectRoot + QStringLiteral("/include/constants/songs.h");
    const QString ldPath = projectRoot + QStringLiteral("/ld_script.ld");
    if (plan.ldApplicable)
        check(readAllBytes(ldPath).contains(
                  QStringLiteral("sound/songs/midi/%1.o").arg(label).toUtf8()),
              "ld_script.ld missing the song's object line");
    if (plan.charmapApplicable)
        check(readAllBytes(charmapPath).contains(plan.charmapLine.toUtf8()),
              "charmap.txt missing the song's ID mapping");

    // Registering again must be a byte-level no-op.
    const QByteArray tableBefore = readAllBytes(tablePath);
    const QByteArray songsHBefore = readAllBytes(songsHPath);
    const QByteArray ldBefore = readAllBytes(ldPath);
    const QByteArray charmapBefore = readAllBytes(charmapPath);
    check(SongRegistry::registerSong(projectRoot, label, constant,
                                     QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
          "second registerSong failed");
    check(songId == registeredCount, "song ID drifted on re-register");
    check(readAllBytes(tablePath) == tableBefore && readAllBytes(songsHPath) == songsHBefore &&
              readAllBytes(ldPath) == ldBefore && readAllBytes(charmapPath) == charmapBefore,
          "re-register was not byte-identical");

    // A songs.h define whose ID drifted from the table index gets corrected.
    {
        QByteArray tampered = songsHBefore;
        const QByteArray goodDefine = QStringLiteral("#define %1").arg(constant).toUtf8();
        const int at = tampered.indexOf(goodDefine);
        check(at >= 0, "tamper: define not found");
        int digits = tampered.indexOf('\n', at);
        QByteArray line = tampered.mid(at, digits - at);
        line.replace(QByteArray::number(songId), QByteArray::number(songId + 500));
        tampered.replace(at, digits - at, line);
        QFile out(songsHPath);
        check(out.open(QIODevice::WriteOnly) && out.write(tampered) == tampered.size(),
              "tamper: rewrite songs.h");
        out.close();

        status = SongRegistry::checkRegistration(projectRoot, label, constant);
        check(!status.inSongsH, "stale define not detected");
        check(SongRegistry::registerSong(projectRoot, label, constant,
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "registerSong after tamper failed");
        check(readAllBytes(songsHPath) == songsHBefore, "stale define not corrected");
    }

    // Likewise a charmap.txt entry whose ID bytes drifted.
    if (plan.charmapApplicable) {
        QByteArray tampered = charmapBefore;
        const int at = tampered.indexOf(plan.charmapLine.toUtf8());
        check(at >= 0, "charmap tamper: entry not found");
        QByteArray line = plan.charmapLine.toUtf8();
        line.replace(charmapBytes.toUtf8(), QByteArrayLiteral("FF 7F"));
        tampered.replace(at, plan.charmapLine.size(), line);
        QFile out(charmapPath);
        check(out.open(QIODevice::WriteOnly) && out.write(tampered) == tampered.size(),
              "charmap tamper: rewrite charmap.txt");
        out.close();

        status = SongRegistry::checkRegistration(projectRoot, label, constant);
        check(!status.inCharmap, "stale charmap bytes not detected");
        check(SongRegistry::registerSong(projectRoot, label, constant,
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "registerSong after charmap tamper failed");
        check(readAllBytes(charmapPath) == charmapBefore, "stale charmap bytes not corrected");
    }

    check(project.reload(&error), "project reload after registration");
    const SongInfo *registered = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == label)
            registered = &s;
    }
    check(registered && registered->registered, "song not registered after registerSong");
    check(registered && registered->id == registeredCount, "registered song ID wrong");
    check(registered && registered->constant == constant, "constant not matched from songs.h");

    // ---- charmap ID-ordered backfill -----------------------------------------
    // A mid-table song whose charmap line went missing gets it reinserted at
    // its ID position between its neighbors, not appended — proven by the
    // file round-tripping byte-identically through strip + re-register.
    if (plan.charmapApplicable) {
        const QByteArray original = readAllBytes(charmapPath);
        QList<QByteArray> lines = original.split('\n');
        const SongInfo *midSong = nullptr;
        int lineAt = -1;
        for (int id = registeredCount / 2; id < registeredCount && !midSong; id++) {
            const SongInfo &s = project.songs().at(id);
            if (!s.registered || s.constant.isEmpty())
                continue;
            // The song's own line, and only one of it (alias constants that
            // share an ID would make the strip ambiguous).
            int found = -1, hits = 0;
            for (int i = 0; i < lines.size(); i++) {
                if (lines[i].startsWith(s.constant.toUtf8() + ' ')) {
                    found = i;
                    hits++;
                }
            }
            if (hits == 1) {
                midSong = &s;
                lineAt = found;
            }
        }
        check(midSong != nullptr, "ordered backfill: no mid-table candidate song");
        if (midSong) {
            lines.removeAt(lineAt);
            QFile out(charmapPath);
            check(out.open(QIODevice::WriteOnly), "ordered backfill: rewrite charmap.txt");
            out.write(lines.join('\n'));
            out.close();
            check(readAllBytes(charmapPath) != original, "ordered backfill: strip was a no-op");
            check(SongRegistry::registerSong(projectRoot, midSong->label, midSong->constant,
                                             midSong->player.isEmpty()
                                                 ? QStringLiteral("MUSIC_PLAYER_BGM")
                                                 : midSong->player,
                                             &regError, &songId),
                  "ordered backfill: registerSong failed");
            check(readAllBytes(charmapPath) == original,
                  "backfilled charmap line not restored at its ID position");
        }
    }

    // ---- songs.h ID-ordered backfill -----------------------------------------
    // The same strip + re-register proof for songs.h: a mid-table song's
    // define must return to its ID position between its neighbors, not be
    // appended. Vanilla files end with hex-valued sentinels after the last
    // real ID (MUS_NONE 0xFFFF, PHONEME_ID_NONE 0xFF); a backfill must not
    // be dragged past them — their leading digit once parsed as value 0.
    {
        const QByteArray original = readAllBytes(songsHPath);
        QList<QByteArray> lines = original.split('\n');
        const SongInfo *midSong = nullptr;
        int lineAt = -1;
        for (int id = registeredCount / 2; id < registeredCount && !midSong; id++) {
            const SongInfo &s = project.songs().at(id);
            if (!s.registered || s.constant.isEmpty())
                continue;
            // The song's own define, in exactly the shape registerSong would
            // rewrite (no trailing comment), and only one of it. The line
            // directly above must be the ID-1 define — reinserting after the
            // last smaller value is only byte-identical when no section
            // comment or blank line sits between the two (the freed-slot
            // scenario this proves).
            const QRegularExpression exactRe(
                QStringLiteral("^#define %1\\s+%2$").arg(s.constant).arg(id));
            const QRegularExpression prevRe(
                QStringLiteral("^\\s*#define\\s+\\w+\\s+%1\\b").arg(id - 1));
            int found = -1, hits = 0;
            for (int i = 0; i < lines.size(); i++) {
                if (exactRe.match(QString::fromUtf8(lines[i])).hasMatch()) {
                    found = i;
                    hits++;
                }
            }
            if (hits == 1 && found > 0 &&
                prevRe.match(QString::fromUtf8(lines[found - 1])).hasMatch()) {
                midSong = &s;
                lineAt = found;
            }
        }
        check(midSong != nullptr, "songs.h backfill: no mid-table candidate song");
        if (midSong) {
            lines.removeAt(lineAt);
            QFile out(songsHPath);
            check(out.open(QIODevice::WriteOnly), "songs.h backfill: rewrite songs.h");
            out.write(lines.join('\n'));
            out.close();
            check(readAllBytes(songsHPath) != original, "songs.h backfill: strip was a no-op");
            check(SongRegistry::registerSong(projectRoot, midSong->label, midSong->constant,
                                             midSong->player.isEmpty()
                                                 ? QStringLiteral("MUSIC_PLAYER_BGM")
                                                 : midSong->player,
                                             &regError, &songId),
                  "songs.h backfill: registerSong failed");
            check(readAllBytes(songsHPath) == original,
                  "backfilled songs.h define not restored at its ID position");
        }
    }

    // ---- Aliased table entries -----------------------------------------------
    // Forks fill new table slots with copies of real songs (pokezelda field
    // report: mus_rg_mt_moon at both 455 and 498), so a label can own several
    // indices. A songs.h define / charmap entry naming ANY of them is
    // correctly registered — the checker must not flag it, and registerSong
    // must not "correct" the define to the duplicate's index. A define
    // naming none of them still heals, to the label's first entry.
    {
        const QByteArray table0 = readAllBytes(tablePath);
        const QByteArray songsH0 = readAllBytes(songsHPath);
        const QByteArray charmap0 = readAllBytes(charmapPath);
        QFile table(tablePath);
        check(table.open(QIODevice::Append), "alias: append duplicate entry");
        table.write(plan.songTableLine.toUtf8() + "\n");
        table.close();

        status = SongRegistry::checkRegistration(projectRoot, label, constant);
        check(status.complete(), "aliased table entry flagged the song");
        const RegistrationPlan aliased = SongRegistry::makePlan(projectRoot, label, constant,
                                                                QStringLiteral("MUSIC_PLAYER_BGM"));
        check(aliased.songId == plan.songId, "aliased plan abandoned the define's own index");
        check(SongRegistry::registerSong(projectRoot, label, constant,
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "aliased registerSong failed");
        check(readAllBytes(songsHPath) == songsH0 && readAllBytes(charmapPath) == charmap0,
              "aliased registerSong rewrote the define or charmap entry");

        // Genuine drift heals to the label's FIRST entry, not the alias.
        {
            QByteArray tampered = songsH0;
            const QByteArray goodDefine = QStringLiteral("#define %1").arg(constant).toUtf8();
            const int at = tampered.indexOf(goodDefine);
            check(at >= 0, "alias drift: define not found");
            const int end = tampered.indexOf('\n', at);
            QByteArray line = tampered.mid(at, end - at);
            line.replace(QByteArray::number(plan.songId), QByteArrayLiteral("9999"));
            tampered.replace(at, end - at, line);
            QFile out(songsHPath);
            check(out.open(QIODevice::WriteOnly) && out.write(tampered) == tampered.size(),
                  "alias drift: rewrite songs.h");
            out.close();
            status = SongRegistry::checkRegistration(projectRoot, label, constant);
            check(!status.inSongsH, "drifted define not flagged despite alias");
            check(SongRegistry::registerSong(projectRoot, label, constant,
                                             QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                             &songId),
                  "alias drift: registerSong failed");
            check(readAllBytes(songsHPath) == songsH0,
                  "drifted define not healed to the label's first entry");
        }

        check(table.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                  table.write(table0) == table0.size(),
              "alias: restore song_table.inc");
        table.close();
    }

    // ---- src/debug.c sound lists ---------------------------------------------
    // pokeemerald-expansion's debug menu lists every song as an X-macro entry
    // in src/debug.c's SOUND_LIST_BGM / SOUND_LIST_SE. Vanilla has no such
    // file, so the flow runs against a fixture: entries land in the
    // prefix-matching list at their ID position with the macro's '\'
    // continuations rewired at the list ends, and deletion is the exact
    // inverse.
    {
        const QString debugCPath = projectRoot + QStringLiteral("/src/debug.c");
        const QByteArray originalDebug = readAllBytes(debugCPath);
        if (originalDebug.isEmpty())
            check(!plan.debugApplicable, "src/debug.c leg not inapplicable without the file");

        // Entry lines pad their '\' into a shared column like the real file.
        const auto dbgLine = [](const char *text, bool continued) {
            QByteArray line(text);
            if (continued)
                line += QByteArray(36 - line.size(), ' ') + "\\";
            return line + "\n";
        };
        const auto writeDebug = [&](const QByteArray &bytes) {
            QFile out(debugCPath);
            check(out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                      out.write(bytes) == bytes.size(),
                  "rewrite src/debug.c fixture");
        };
        // Vanilla songs (IDs 2, 3, 4) with ID 1 unlisted, so both a mid-list
        // and a before-first backfill have a home.
        const QByteArray fixture =
            dbgLine("#define SOUND_LIST_BGM", true) + dbgLine("    X(MUS_GSC_ROUTE38)", true) +
            dbgLine("    X(MUS_CAUGHT)", true) + dbgLine("    X(MUS_VICTORY_WILD)", false) + "\n" +
            dbgLine("#define SOUND_LIST_SE", true) + dbgLine("    X(SE_USE_ITEM)", true) +
            dbgLine("    X(SE_PC_LOGIN)", false);
        writeDebug(fixture);

        const RegistrationPlan dbgPlan = SongRegistry::makePlan(projectRoot, label, constant,
                                                                QStringLiteral("MUSIC_PLAYER_BGM"));
        check(dbgPlan.debugApplicable, "debug.c sound lists not detected");
        check(dbgPlan.debugLine.toUtf8() == dbgLine("    X(MUS_ONBOARDCHECK)", true).chopped(1),
              "debug.c entry line not padded to the '\\' column");

        status = SongRegistry::checkRegistration(projectRoot, label, constant);
        check(status.debugApplicable && !status.inDebugMenu,
              "unlisted song reads as present in the debug menu");
        check(!status.complete(), "missing debug.c entry does not gate completeness");

        // Registering the (otherwise fully registered) song appends its entry
        // to the BGM list alone: the old final entry gains a continuation,
        // the new final line is bare, and no other file changes a byte.
        const QByteArray tableR = readAllBytes(tablePath);
        const QByteArray songsHR = readAllBytes(songsHPath);
        const QByteArray ldR = readAllBytes(ldPath);
        const QByteArray charmapR = readAllBytes(charmapPath);
        check(SongRegistry::registerSong(projectRoot, label, constant,
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "registerSong with debug.c fixture failed");
        const QByteArray afterMain =
            dbgLine("#define SOUND_LIST_BGM", true) + dbgLine("    X(MUS_GSC_ROUTE38)", true) +
            dbgLine("    X(MUS_CAUGHT)", true) + dbgLine("    X(MUS_VICTORY_WILD)", true) +
            dbgLine("    X(MUS_ONBOARDCHECK)", false) + "\n" +
            dbgLine("#define SOUND_LIST_SE", true) + dbgLine("    X(SE_USE_ITEM)", true) +
            dbgLine("    X(SE_PC_LOGIN)", false);
        check(readAllBytes(debugCPath) == afterMain,
              "debug.c entry not appended with the continuation handover");
        check(readAllBytes(tablePath) == tableR && readAllBytes(songsHPath) == songsHR &&
                  readAllBytes(ldPath) == ldR && readAllBytes(charmapPath) == charmapR,
              "debug.c registration touched another file");
        check(SongRegistry::registerSong(projectRoot, label, constant,
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId) &&
                  readAllBytes(debugCPath) == afterMain,
              "debug.c re-register was not byte-identical");
        status = SongRegistry::checkRegistration(projectRoot, label, constant);
        check(status.inDebugMenu && status.complete(),
              "debug.c entry not reflected in the registration status");
        check(SongRegistry::makeRemovalPlan(projectRoot, label, constant).inDebugMenu,
              "removal plan misses the debug menu entry");

        // The model's per-song gaps name the file — for unlisted songs only.
        check(project.reload(&error), "reload with debug.c fixture");
        const auto gapsFor = [&project](const QString &wanted) {
            for (const SongInfo &s : project.songs()) {
                if (s.label == wanted)
                    return s.registrationGaps;
            }
            return QStringList{QStringLiteral("<absent>")};
        };
        check(gapsFor(QStringLiteral("mus_littleroot_test")) ==
                  QStringList{QStringLiteral("src/debug.c")},
              "unlisted song's gaps do not name src/debug.c");
        check(!gapsFor(QStringLiteral("mus_caught")).contains(QStringLiteral("src/debug.c")),
              "listed song's gaps name src/debug.c anyway");

        // An SE_-prefixed constant routes to SOUND_LIST_SE; unregistering it
        // is the exact inverse, down to the '\' the old final entry sheds.
        const QString seLabel = QStringLiteral("se_onboardcheck");
        const QString seConstant = SongRegistry::constantForLabel(seLabel);
        check(SongRegistry::registerSong(projectRoot, seLabel, seConstant,
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "SE registerSong failed");
        const QByteArray afterSe =
            dbgLine("#define SOUND_LIST_BGM", true) + dbgLine("    X(MUS_GSC_ROUTE38)", true) +
            dbgLine("    X(MUS_CAUGHT)", true) + dbgLine("    X(MUS_VICTORY_WILD)", true) +
            dbgLine("    X(MUS_ONBOARDCHECK)", false) + "\n" +
            dbgLine("#define SOUND_LIST_SE", true) + dbgLine("    X(SE_USE_ITEM)", true) +
            dbgLine("    X(SE_PC_LOGIN)", true) + dbgLine("    X(SE_ONBOARDCHECK)", false);
        check(readAllBytes(debugCPath) == afterSe, "SE entry not routed to SOUND_LIST_SE");
        check(SongRegistry::unregisterSong(projectRoot, seLabel, seConstant, &error),
              "SE unregisterSong failed");
        check(readAllBytes(debugCPath) == afterMain && readAllBytes(tablePath) == tableR &&
                  readAllBytes(songsHPath) == songsHR && readAllBytes(ldPath) == ldR &&
                  readAllBytes(charmapPath) == charmapR,
              "SE song's registration did not round-trip");

        // A mid-list removal needs no continuation rewiring; the ghost label
        // exists nowhere but the fixture, so unregisterSong touches only its
        // line.
        {
            QByteArray withGhost = afterMain;
            const QByteArray anchor = dbgLine("    X(MUS_GSC_ROUTE38)", true);
            withGhost.insert(withGhost.indexOf(anchor) + anchor.size(),
                             dbgLine("    X(MUS_ONBOARDCHECK_GHOST)", true));
            writeDebug(withGhost);
            check(SongRegistry::unregisterSong(projectRoot,
                                               QStringLiteral("mus_onboardcheck_ghost"),
                                               QStringLiteral("MUS_ONBOARDCHECK_GHOST"), &error),
                  "ghost unregisterSong failed");
            check(readAllBytes(debugCPath) == afterMain,
                  "mid-list removal did not excise exactly one line");
        }

        // A stripped mid-list entry backfills at its ID position between its
        // neighbors — byte-identically, like the charmap backfill above.
        {
            QByteArray stripped = afterMain;
            const QByteArray caught = dbgLine("    X(MUS_CAUGHT)", true);
            const qsizetype at = stripped.indexOf(caught);
            check(at >= 0, "debug backfill: entry not in the fixture");
            stripped.remove(at, caught.size());
            writeDebug(stripped);
            check(SongRegistry::registerSong(
                      projectRoot, QStringLiteral("mus_caught"), QStringLiteral("MUS_CAUGHT"),
                      QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
                  "debug backfill: registerSong failed");
            check(readAllBytes(debugCPath) == afterMain,
                  "stripped debug entry not restored at its ID position");
        }

        // An ID preceding every listed entry lands before the first one.
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_littleroot_test"),
                                         QStringLiteral("MUS_LITTLEROOT_TEST"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "debug before-first: registerSong failed");
        QByteArray withLittleroot = afterMain;
        withLittleroot.insert(withLittleroot.indexOf(dbgLine("    X(MUS_GSC_ROUTE38)", true)),
                              dbgLine("    X(MUS_LITTLEROOT_TEST)", true));
        check(readAllBytes(debugCPath) == withLittleroot,
              "smallest-ID entry not inserted before the first entry");

        // List-end edges: removing a list's only entry bares its #define;
        // inserting into an empty list hands the #define the continuation
        // (single-space form — nothing left to align with).
        writeDebug(QByteArrayLiteral("#define SOUND_LIST_BGM\n") +
                   dbgLine("#define SOUND_LIST_SE", true) + "    X(SE_ONBOARDCHECK_GHOST)\n");
        check(SongRegistry::unregisterSong(projectRoot, QStringLiteral("se_onboardcheck_ghost"),
                                           QStringLiteral("SE_ONBOARDCHECK_GHOST"), &error),
              "sole-entry unregisterSong failed");
        check(readAllBytes(debugCPath) == QByteArrayLiteral("#define SOUND_LIST_BGM\n"
                                                            "#define SOUND_LIST_SE\n"),
              "removing a list's only entry left the #define continued");
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_caught"),
                                         QStringLiteral("MUS_CAUGHT"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
              "empty-list registerSong failed");
        check(readAllBytes(debugCPath) == QByteArrayLiteral("#define SOUND_LIST_BGM \\\n"
                                                            "    X(MUS_CAUGHT)\n"
                                                            "#define SOUND_LIST_SE\n"),
              "empty-list insert did not hand the #define its continuation");

        // Older expansion debug menus (and forks of them) use a two-argument
        // entry form with a quoted display name, per-list column alignment
        // (the BGM and SE lists align differently), every line '\'-continued,
        // and a blank line ending the macro. New entries must match that
        // shape — a field report caught the single-arg form being inserted
        // into (and never recognized in) such a file.
        {
            const auto namedLine = [](const char *constant, int commaCol, int parenCol,
                                      int slashCol) {
                QByteArray t("    X(");
                t += constant;
                t += QByteArray(commaCol - t.size(), ' ') + ", \"";
                QByteArray display(constant);
                display.replace('_', '-');
                t += display + "\"";
                t += QByteArray(parenCol - t.size(), ' ') + ")";
                t += QByteArray(slashCol - t.size(), ' ') + "\\";
                return t + "\n";
            };
            const QByteArray named0 = QByteArrayLiteral("#define SOUND_LIST_BGM \\\n") +
                                      namedLine("MUS_GSC_ROUTE38", 34, 60, 62) +
                                      namedLine("MUS_VICTORY_WILD", 34, 60, 62) + "\n" +
                                      QByteArrayLiteral("#define SOUND_LIST_SE \\\n") +
                                      namedLine("SE_USE_ITEM", 28, 50, 52) + "\n";
            writeDebug(named0);
            check(SongRegistry::checkRegistration(projectRoot, QStringLiteral("mus_gsc_route38"),
                                                  QStringLiteral("MUS_GSC_ROUTE38"))
                      .inDebugMenu,
                  "two-argument debug entry not recognized");

            // Appending keeps the named shape and the BGM list's columns.
            check(SongRegistry::registerSong(projectRoot, label, constant,
                                             QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                             &songId),
                  "named-style registerSong failed");
            const QByteArray namedMain = QByteArrayLiteral("#define SOUND_LIST_BGM \\\n") +
                                         namedLine("MUS_GSC_ROUTE38", 34, 60, 62) +
                                         namedLine("MUS_VICTORY_WILD", 34, 60, 62) +
                                         namedLine("MUS_ONBOARDCHECK", 34, 60, 62) + "\n" +
                                         QByteArrayLiteral("#define SOUND_LIST_SE \\\n") +
                                         namedLine("SE_USE_ITEM", 28, 50, 52) + "\n";
            check(readAllBytes(debugCPath) == namedMain,
                  "named entry not appended in the list's shape");
            check(SongRegistry::registerSong(projectRoot, label, constant,
                                             QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                             &songId) &&
                      readAllBytes(debugCPath) == namedMain,
                  "named entry duplicated on re-register");

            // Mid-list ID order holds in the named shape too.
            check(SongRegistry::registerSong(
                      projectRoot, QStringLiteral("mus_caught"), QStringLiteral("MUS_CAUGHT"),
                      QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId),
                  "named mid-list registerSong failed");
            const QByteArray namedCaught = QByteArrayLiteral("#define SOUND_LIST_BGM \\\n") +
                                           namedLine("MUS_GSC_ROUTE38", 34, 60, 62) +
                                           namedLine("MUS_CAUGHT", 34, 60, 62) +
                                           namedLine("MUS_VICTORY_WILD", 34, 60, 62) +
                                           namedLine("MUS_ONBOARDCHECK", 34, 60, 62) + "\n" +
                                           QByteArrayLiteral("#define SOUND_LIST_SE \\\n") +
                                           namedLine("SE_USE_ITEM", 28, 50, 52) + "\n";
            check(readAllBytes(debugCPath) == namedCaught, "named backfill not at its ID position");

            // The SE list's own (different) columns drive SE entries, and
            // unregistering round-trips everything.
            check(SongRegistry::registerSong(projectRoot, seLabel, seConstant,
                                             QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                             &songId),
                  "named SE registerSong failed");
            const QByteArray namedSe = QByteArrayLiteral("#define SOUND_LIST_BGM \\\n") +
                                       namedLine("MUS_GSC_ROUTE38", 34, 60, 62) +
                                       namedLine("MUS_CAUGHT", 34, 60, 62) +
                                       namedLine("MUS_VICTORY_WILD", 34, 60, 62) +
                                       namedLine("MUS_ONBOARDCHECK", 34, 60, 62) + "\n" +
                                       QByteArrayLiteral("#define SOUND_LIST_SE \\\n") +
                                       namedLine("SE_USE_ITEM", 28, 50, 52) +
                                       namedLine("SE_ONBOARDCHECK", 28, 50, 52) + "\n";
            check(readAllBytes(debugCPath) == namedSe,
                  "named SE entry not aligned to the SE list's columns");
            check(SongRegistry::unregisterSong(projectRoot, seLabel, seConstant, &error) &&
                      readAllBytes(debugCPath) == namedCaught && readAllBytes(tablePath) == tableR,
                  "named SE registration did not round-trip");
        }

        // A vanilla scratch loses the fixture outright; an expansion checkout
        // gets its own debug.c back (the song's entry was already in the
        // snapshot — registerSong wrote it before this section).
        if (originalDebug.isEmpty())
            check(QFile::remove(debugCPath), "remove src/debug.c fixture");
        else
            writeDebug(originalDebug);
        check(project.reload(&error), "reload after debug.c fixture cleanup");
    }

    // ---- Regioned songs.h layouts (END_SE / START_MUS / END_MUS) ------------
    // Marker-bounded songs.h layouts size ID-indexed arrays from their
    // markers — pre-#9713 checkouts alias the last constant and size
    // src/debug.c's sound-tester arrays (sBGMNames[END_MUS - START_MUS +
    // 1]), the night-music line re-added value-form markers sizing
    // overworld.c's sNightMusicTable — so a song appended past the phoneme
    // block breaks the build or falls outside the feature. On any layout
    // whose END_MUS resolves, registration must insert music at END_MUS + 1
    // (the phoneme block shifts up by one in songs.h and charmap.txt), fill
    // the placeholder gap after END_SE for sound effects, keep each marker
    // on its region's last song — through deletion and free-slot reuse too
    // — and migrate a stranded registration back into the region.
    {
        const QString debugCPath = projectRoot + QStringLiteral("/src/debug.c");
        const QByteArray table0 = readAllBytes(tablePath);
        const QByteArray songsH0 = readAllBytes(songsHPath);
        const QByteArray ld0 = readAllBytes(ldPath);
        const QByteArray charmap0 = readAllBytes(charmapPath);
        const QByteArray debug0 = readAllBytes(debugCPath);

        const auto writeFixture = [&](const QString &path, const QByteArray &bytes) {
            QFile out(path);
            check(out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                      out.write(bytes) == bytes.size(),
                  "regioned: fixture write failed");
        };
        // "#define NAME<pad to column 28>VALUE" — the fixture's shared value
        // column, so planned lines pad identically.
        const auto defLine = [](const char *name, const char *value) {
            QByteArray t("#define ");
            t += name;
            t += QByteArray(std::max<qsizetype>(1, 28 - t.size()), ' ');
            return t + value + "\n";
        };
        const auto dbgEntry = [](const char *text, bool continued) {
            QByteArray line(text);
            if (continued)
                line += QByteArray(36 - line.size(), ' ') + "\\";
            return line + "\n";
        };
        const auto songLine = [](const char *lab, const char *player, int n) {
            return QByteArray("\tsong ") + lab + ", " + player + ", " + QByteArray::number(n) +
                   "\n";
        };

        const QByteArray tableHead = QByteArrayLiteral(
            "\t.equiv MUSIC_PLAYER_BGM, 0\n\t.equiv MUSIC_PLAYER_SE1, 1\n\ngSongTable::\n");
        const QByteArray tableFix = tableHead +                                            // index:
                                    songLine("mus_dummy", "MUSIC_PLAYER_BGM", 0) +         // 0
                                    songLine("se_use_item", "MUSIC_PLAYER_SE1", 1) +       // 1
                                    songLine("se_last", "MUSIC_PLAYER_SE1", 1) +           // 2
                                    songLine("dummy_song_header", "MUSIC_PLAYER_BGM", 0) + // 3
                                    songLine("dummy_song_header", "MUSIC_PLAYER_BGM", 0) + // 4
                                    songLine("mus_first", "MUSIC_PLAYER_BGM", 0) +         // 5
                                    songLine("mus_last", "MUSIC_PLAYER_BGM", 0) +          // 6
                                    songLine("ph_one", "MUSIC_PLAYER_SE1", 1) +            // 7
                                    songLine("ph_two", "MUSIC_PLAYER_SE1", 1);             // 8
        const QByteArray songsHFix =
            defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") + defLine("SE_LAST", "2") +
            defLine("END_SE", "SE_LAST") + defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") +
            defLine("MUS_LAST", "6") + defLine("END_MUS", "MUS_LAST") + defLine("PH_ONE", "7") +
            defLine("PH_TWO", "8") + defLine("MUS_NONE", "0xFFFF");
        const QByteArray charmapFix = QByteArrayLiteral(
            "MUS_DUMMY = 00 00\nSE_USE_ITEM = 01 00\nSE_LAST = 02 00\nMUS_FIRST = 05 00\n"
            "MUS_LAST = 06 00\nPH_ONE = 07 00\nPH_TWO = 08 00\n");
        const QByteArray debugMarkerLine =
            QByteArrayLiteral("static const u8 *const sBGMNames[END_MUS - START_MUS + 1];\n");
        const QByteArray debugListsFix =
            dbgEntry("#define SOUND_LIST_BGM", true) + dbgEntry("    X(MUS_FIRST)", true) +
            dbgEntry("    X(MUS_LAST)", false) + "\n" + dbgEntry("#define SOUND_LIST_SE", true) +
            dbgEntry("    X(SE_USE_ITEM)", true) + dbgEntry("    X(SE_LAST)", false);

        writeFixture(tablePath, tableFix);
        writeFixture(songsHPath, songsHFix);
        writeFixture(charmapPath, charmapFix);

        // True modern (post-#9713) has no markers at all — placement
        // appends, exactly as before.
        const QByteArray songsHNoMarkers = defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") +
                                           defLine("SE_LAST", "2") + defLine("MUS_FIRST", "5") +
                                           defLine("MUS_LAST", "6") + defLine("PH_ONE", "7") +
                                           defLine("PH_TWO", "8") + defLine("MUS_NONE", "0xFFFF");
        writeFixture(songsHPath, songsHNoMarkers);
        writeFixture(debugCPath, debugListsFix);
        check(SongRegistry::makePlan(projectRoot, QStringLiteral("mus_oldcheck"),
                                     QStringLiteral("MUS_OLDCHECK"),
                                     QStringLiteral("MUSIC_PLAYER_BGM"))
                      .songId == 9,
              "regioned: markerless modern layout no longer appends");

        // Value-form markers (the night-music line: "#define END_MUS 558"
        // sizing overworld.c's sNightMusicTable) get regioned placement
        // even though debug.c never consumes them — the marker itself
        // renumbers, and with the modern single ID-indexed debug array the
        // SE_-prefix list routing stays cosmetic-and-by-name.
        const QByteArray songsHVal = defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") +
                                     defLine("SE_LAST", "2") + defLine("START_MUS", "5") +
                                     defLine("MUS_FIRST", "5") + defLine("MUS_LAST", "6") +
                                     defLine("END_MUS", "6") + defLine("PH_ONE", "7") +
                                     defLine("PH_TWO", "8") + defLine("MUS_NONE", "0xFFFF");
        writeFixture(songsHPath, songsHVal);
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_valcheck"),
                                         QStringLiteral("MUS_VALCHECK"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId) &&
                  songId == 7,
              "regioned: value-form markers did not place at END_MUS + 1");
        const QByteArray songsHValAfter =
            defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") + defLine("SE_LAST", "2") +
            defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") + defLine("MUS_LAST", "6") +
            defLine("MUS_VALCHECK", "7") + defLine("END_MUS", "7") + defLine("PH_ONE", "8") +
            defLine("PH_TWO", "9") + defLine("MUS_NONE", "0xFFFF");
        check(readAllBytes(songsHPath) == songsHValAfter,
              "regioned: value-form END_MUS not renumbered with the insert");
        // No END_SE marker, but the placeholder gap survives: the SE
        // boundary derives from the highest define below START_MUS, so the
        // sound effect fills the gap instead of grouping with the music —
        // and END_MUS never comes to rest on an SE.
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("se_valcheck"),
                                         QStringLiteral("SE_VALCHECK"),
                                         QStringLiteral("MUSIC_PLAYER_SE1"), &regError, &songId) &&
                  songId == 3,
              "regioned: markerless SE region not derived from START_MUS");
        const QByteArray songsHValSe =
            defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") + defLine("SE_LAST", "2") +
            defLine("SE_VALCHECK", "3") + defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") +
            defLine("MUS_LAST", "6") + defLine("MUS_VALCHECK", "7") + defLine("END_MUS", "7") +
            defLine("PH_ONE", "8") + defLine("PH_TWO", "9") + defLine("MUS_NONE", "0xFFFF");
        check(readAllBytes(songsHPath) == songsHValSe,
              "regioned: derived-gap SE define misplaced or END_MUS disturbed");
        check(readAllBytes(tablePath).contains("\tsong se_valcheck, MUSIC_PLAYER_SE1, 1\n"
                                               "\tsong dummy_song_header"),
              "regioned: derived-gap SE did not overwrite the placeholder row");
        check(readAllBytes(debugCPath).indexOf("X(SE_VALCHECK)") >
                  readAllBytes(debugCPath).indexOf("#define SOUND_LIST_SE"),
              "regioned: single-array debug.c lost the by-name SE list routing");
        // Deleting the value-form marker's own song renumbers it down.
        check(SongRegistry::unregisterSong(projectRoot, QStringLiteral("mus_valcheck"),
                                           QStringLiteral("MUS_VALCHECK"), &error) &&
                  readAllBytes(songsHPath).contains(defLine("END_MUS", "6")) &&
                  !readAllBytes(songsHPath).contains("MUS_VALCHECK"),
              "regioned: value-form END_MUS not renumbered down on delete");

        // The pre-#9713 alias layout proper: debug.c sizes an array from
        // END_MUS, so the BGM/SE list split is functional. Fresh fixtures —
        // the value-form scenario above mutated them.
        writeFixture(tablePath, tableFix);
        writeFixture(songsHPath, songsHFix);
        writeFixture(charmapPath, charmapFix);
        writeFixture(debugCPath, debugMarkerLine + debugListsFix);

        // Music inserts at END_MUS + 1: the phoneme rows shift down the
        // table, their defines and charmap values shift up by one, and
        // END_MUS follows the new constant.
        RegistrationPlan rp = SongRegistry::makePlan(projectRoot, QStringLiteral("mus_oldcheck"),
                                                     QStringLiteral("MUS_OLDCHECK"),
                                                     QStringLiteral("MUSIC_PLAYER_BGM"));
        check(rp.songId == 7, "regioned: music not proposed at END_MUS + 1");
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_oldcheck"),
                                         QStringLiteral("MUS_OLDCHECK"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId) &&
                  songId == 7,
              "regioned: music registerSong failed");
        const QByteArray tableMus = tableHead + songLine("mus_dummy", "MUSIC_PLAYER_BGM", 0) +
                                    songLine("se_use_item", "MUSIC_PLAYER_SE1", 1) +
                                    songLine("se_last", "MUSIC_PLAYER_SE1", 1) +
                                    songLine("dummy_song_header", "MUSIC_PLAYER_BGM", 0) +
                                    songLine("dummy_song_header", "MUSIC_PLAYER_BGM", 0) +
                                    songLine("mus_first", "MUSIC_PLAYER_BGM", 0) +
                                    songLine("mus_last", "MUSIC_PLAYER_BGM", 0) +
                                    songLine("mus_oldcheck", "MUSIC_PLAYER_BGM", 0) + // 7
                                    songLine("ph_one", "MUSIC_PLAYER_SE1", 1) +       // 8
                                    songLine("ph_two", "MUSIC_PLAYER_SE1", 1);        // 9
        check(readAllBytes(tablePath) == tableMus,
              "regioned: music row not inserted ahead of the phoneme block");
        const QByteArray songsHMus = defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") +
                                     defLine("SE_LAST", "2") + defLine("END_SE", "SE_LAST") +
                                     defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") +
                                     defLine("MUS_LAST", "6") + defLine("MUS_OLDCHECK", "7") +
                                     defLine("END_MUS", "MUS_OLDCHECK") + defLine("PH_ONE", "8") +
                                     defLine("PH_TWO", "9") + defLine("MUS_NONE", "0xFFFF");
        check(readAllBytes(songsHPath) == songsHMus,
              "regioned: define/END_MUS/phoneme renumbering wrong in songs.h");
        const QByteArray charmapMus = QByteArrayLiteral(
            "MUS_DUMMY = 00 00\nSE_USE_ITEM = 01 00\nSE_LAST = 02 00\nMUS_FIRST = 05 00\n"
            "MUS_LAST = 06 00\nMUS_OLDCHECK = 07 00\nPH_ONE = 08 00\nPH_TWO = 09 00\n");
        check(readAllBytes(charmapPath) == charmapMus,
              "regioned: charmap values did not shift with the phonemes");
        const QByteArray debugMus =
            debugMarkerLine + dbgEntry("#define SOUND_LIST_BGM", true) +
            dbgEntry("    X(MUS_FIRST)", true) + dbgEntry("    X(MUS_LAST)", true) +
            dbgEntry("    X(MUS_OLDCHECK)", false) + "\n" +
            dbgEntry("#define SOUND_LIST_SE", true) + dbgEntry("    X(SE_USE_ITEM)", true) +
            dbgEntry("    X(SE_LAST)", false);
        check(readAllBytes(debugCPath) == debugMus,
              "regioned: music debug entry not appended to SOUND_LIST_BGM");
        if (rp.ldApplicable)
            check(readAllBytes(ldPath).contains("sound/songs/midi/mus_oldcheck.o"),
                  "regioned: ld_script.ld missing the song's object line");
        check(SongRegistry::checkRegistration(projectRoot, QStringLiteral("mus_oldcheck"),
                                              QStringLiteral("MUS_OLDCHECK"))
                  .complete(),
              "regioned: music registration incomplete");

        // Idempotency: registering again is a byte-level no-op.
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_oldcheck"),
                                         QStringLiteral("MUS_OLDCHECK"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId) &&
                  songId == 7 && readAllBytes(tablePath) == tableMus &&
                  readAllBytes(songsHPath) == songsHMus &&
                  readAllBytes(charmapPath) == charmapMus && readAllBytes(debugCPath) == debugMus,
              "regioned: re-register was not byte-identical");

        // A sound effect fills the placeholder gap after END_SE in place —
        // nothing shifts — and END_SE follows it.
        rp = SongRegistry::makePlan(projectRoot, QStringLiteral("se_oldcheck"),
                                    QStringLiteral("SE_OLDCHECK"),
                                    QStringLiteral("MUSIC_PLAYER_SE1"));
        check(rp.songId == 3, "regioned: SE not proposed at the placeholder slot");
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("se_oldcheck"),
                                         QStringLiteral("SE_OLDCHECK"),
                                         QStringLiteral("MUSIC_PLAYER_SE1"), &regError, &songId) &&
                  songId == 3,
              "regioned: SE registerSong failed");
        const QByteArray tableSe = tableHead + songLine("mus_dummy", "MUSIC_PLAYER_BGM", 0) +
                                   songLine("se_use_item", "MUSIC_PLAYER_SE1", 1) +
                                   songLine("se_last", "MUSIC_PLAYER_SE1", 1) +
                                   songLine("se_oldcheck", "MUSIC_PLAYER_SE1", 1) + // 3
                                   songLine("dummy_song_header", "MUSIC_PLAYER_BGM", 0) +
                                   songLine("mus_first", "MUSIC_PLAYER_BGM", 0) +
                                   songLine("mus_last", "MUSIC_PLAYER_BGM", 0) +
                                   songLine("mus_oldcheck", "MUSIC_PLAYER_BGM", 0) +
                                   songLine("ph_one", "MUSIC_PLAYER_SE1", 1) +
                                   songLine("ph_two", "MUSIC_PLAYER_SE1", 1);
        check(readAllBytes(tablePath) == tableSe,
              "regioned: SE did not overwrite the placeholder row");
        const QByteArray songsHSe =
            defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") + defLine("SE_LAST", "2") +
            defLine("SE_OLDCHECK", "3") + defLine("END_SE", "SE_OLDCHECK") +
            defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") + defLine("MUS_LAST", "6") +
            defLine("MUS_OLDCHECK", "7") + defLine("END_MUS", "MUS_OLDCHECK") +
            defLine("PH_ONE", "8") + defLine("PH_TWO", "9") + defLine("MUS_NONE", "0xFFFF");
        check(readAllBytes(songsHPath) == songsHSe,
              "regioned: SE define/END_SE placement wrong in songs.h");
        const QByteArray charmapSe = QByteArrayLiteral(
            "MUS_DUMMY = 00 00\nSE_USE_ITEM = 01 00\nSE_LAST = 02 00\nSE_OLDCHECK = 03 00\n"
            "MUS_FIRST = 05 00\nMUS_LAST = 06 00\nMUS_OLDCHECK = 07 00\nPH_ONE = 08 00\n"
            "PH_TWO = 09 00\n");
        check(readAllBytes(charmapPath) == charmapSe,
              "regioned: SE charmap entry not at its ID position");
        const QByteArray debugSe =
            debugMarkerLine + dbgEntry("#define SOUND_LIST_BGM", true) +
            dbgEntry("    X(MUS_FIRST)", true) + dbgEntry("    X(MUS_LAST)", true) +
            dbgEntry("    X(MUS_OLDCHECK)", false) + "\n" +
            dbgEntry("#define SOUND_LIST_SE", true) + dbgEntry("    X(SE_USE_ITEM)", true) +
            dbgEntry("    X(SE_LAST)", true) + dbgEntry("    X(SE_OLDCHECK)", false);
        check(readAllBytes(debugCPath) == debugSe,
              "regioned: SE debug entry not appended to SOUND_LIST_SE");

        // A registration stranded past the phonemes (an earlier porydaw
        // appended it there) is flagged and migrates into the region on
        // re-register: row, define, and charmap entry move to END_MUS + 1,
        // the phonemes shift again, and the debug entry stays put.
        writeFixture(tablePath, tableSe + songLine("mus_straggler", "MUSIC_PLAYER_BGM", 0));
        QByteArray songsHBroken = songsHSe;
        songsHBroken.replace(defLine("PH_TWO", "9"),
                             defLine("PH_TWO", "9") + defLine("MUS_STRAGGLER", "10"));
        writeFixture(songsHPath, songsHBroken);
        writeFixture(charmapPath, charmapSe + QByteArrayLiteral("MUS_STRAGGLER = 0A 00\n"));
        QByteArray debugBroken = debugSe;
        debugBroken.replace(dbgEntry("    X(MUS_OLDCHECK)", false),
                            dbgEntry("    X(MUS_OLDCHECK)", true) +
                                dbgEntry("    X(MUS_STRAGGLER)", false));
        writeFixture(debugCPath, debugBroken);

        check(!SongRegistry::checkRegistration(projectRoot, QStringLiteral("mus_straggler"),
                                               QStringLiteral("MUS_STRAGGLER"))
                   .inSongsH,
              "regioned: stranded registration not flagged");
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_straggler"),
                                         QStringLiteral("MUS_STRAGGLER"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId) &&
                  songId == 8,
              "regioned: stranded registerSong did not migrate");
        const QByteArray tableMigrated = tableHead + songLine("mus_dummy", "MUSIC_PLAYER_BGM", 0) +
                                         songLine("se_use_item", "MUSIC_PLAYER_SE1", 1) +
                                         songLine("se_last", "MUSIC_PLAYER_SE1", 1) +
                                         songLine("se_oldcheck", "MUSIC_PLAYER_SE1", 1) +
                                         songLine("dummy_song_header", "MUSIC_PLAYER_BGM", 0) +
                                         songLine("mus_first", "MUSIC_PLAYER_BGM", 0) +
                                         songLine("mus_last", "MUSIC_PLAYER_BGM", 0) +
                                         songLine("mus_oldcheck", "MUSIC_PLAYER_BGM", 0) +
                                         songLine("mus_straggler", "MUSIC_PLAYER_BGM", 0) + // 8
                                         songLine("ph_one", "MUSIC_PLAYER_SE1", 1) +        // 9
                                         songLine("ph_two", "MUSIC_PLAYER_SE1", 1);         // 10
        check(readAllBytes(tablePath) == tableMigrated,
              "regioned: stranded row not moved ahead of the phonemes");
        const QByteArray songsHMigrated =
            defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") + defLine("SE_LAST", "2") +
            defLine("SE_OLDCHECK", "3") + defLine("END_SE", "SE_OLDCHECK") +
            defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") + defLine("MUS_LAST", "6") +
            defLine("MUS_OLDCHECK", "7") + defLine("MUS_STRAGGLER", "8") +
            defLine("END_MUS", "MUS_STRAGGLER") + defLine("PH_ONE", "9") + defLine("PH_TWO", "10") +
            defLine("MUS_NONE", "0xFFFF");
        check(readAllBytes(songsHPath) == songsHMigrated,
              "regioned: stranded define not moved to its ID position");
        const QByteArray charmapMigrated = QByteArrayLiteral(
            "MUS_DUMMY = 00 00\nSE_USE_ITEM = 01 00\nSE_LAST = 02 00\nSE_OLDCHECK = 03 00\n"
            "MUS_FIRST = 05 00\nMUS_LAST = 06 00\nMUS_OLDCHECK = 07 00\nMUS_STRAGGLER = 08 00\n"
            "PH_ONE = 09 00\nPH_TWO = 0A 00\n");
        check(readAllBytes(charmapPath) == charmapMigrated,
              "regioned: stranded charmap entry not migrated");
        check(readAllBytes(debugCPath) == debugBroken,
              "regioned: migration should leave the debug entry untouched");
        check(SongRegistry::checkRegistration(projectRoot, QStringLiteral("mus_straggler"),
                                              QStringLiteral("MUS_STRAGGLER"))
                  .complete(),
              "regioned: migrated registration incomplete");

        // Deleting the marker's referent re-points END_MUS to the region's
        // new last song; the freed slot is later reused and the marker
        // follows again.
        check(SongRegistry::unregisterSong(projectRoot, QStringLiteral("mus_straggler"),
                                           QStringLiteral("MUS_STRAGGLER"), &error),
              "regioned: unregisterSong failed");
        const QByteArray songsHDeleted =
            defLine("MUS_DUMMY", "0") + defLine("SE_USE_ITEM", "1") + defLine("SE_LAST", "2") +
            defLine("SE_OLDCHECK", "3") + defLine("END_SE", "SE_OLDCHECK") +
            defLine("START_MUS", "5") + defLine("MUS_FIRST", "5") + defLine("MUS_LAST", "6") +
            defLine("MUS_OLDCHECK", "7") + defLine("END_MUS", "MUS_OLDCHECK") +
            defLine("PH_ONE", "9") + defLine("PH_TWO", "10") + defLine("MUS_NONE", "0xFFFF");
        check(readAllBytes(songsHPath) == songsHDeleted,
              "regioned: END_MUS not re-pointed after deleting its referent");
        const QByteArray tableFreed =
            tableMigrated.left(tableMigrated.indexOf("\tsong mus_straggler")) +
            songLine("mus_dummy", "MUSIC_PLAYER_BGM", 0) +
            songLine("ph_one", "MUSIC_PLAYER_SE1", 1) + songLine("ph_two", "MUSIC_PLAYER_SE1", 1);
        check(readAllBytes(tablePath) == tableFreed,
              "regioned: deleted mid-region row did not become a free slot");
        rp = SongRegistry::makePlan(projectRoot, QStringLiteral("mus_refill"),
                                    QStringLiteral("MUS_REFILL"),
                                    QStringLiteral("MUSIC_PLAYER_BGM"));
        check(rp.songId == 8, "regioned: freed in-region slot not proposed for reuse");
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("mus_refill"),
                                         QStringLiteral("MUS_REFILL"),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &songId) &&
                  songId == 8,
              "regioned: freed slot not reused");
        check(readAllBytes(songsHPath)
                  .contains(defLine("MUS_REFILL", "8") + defLine("END_MUS", "MUS_REFILL")),
              "regioned: END_MUS does not follow the slot-reusing song");

        // With the placeholder gap exhausted, a sound effect overflows into
        // the music region — and its debug entry must land in
        // SOUND_LIST_BGM, whose array is the one its ID indexes.
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("se_extra"),
                                         QStringLiteral("SE_EXTRA"),
                                         QStringLiteral("MUSIC_PLAYER_SE1"), &regError, &songId) &&
                  songId == 4,
              "regioned: second SE did not take the last placeholder");
        check(SongRegistry::registerSong(projectRoot, QStringLiteral("se_over"),
                                         QStringLiteral("SE_OVER"),
                                         QStringLiteral("MUSIC_PLAYER_SE1"), &regError, &songId) &&
                  songId == 9,
              "regioned: overflow SE not placed at END_MUS + 1");
        const QByteArray debugAfter = readAllBytes(debugCPath);
        const qsizetype seListAt = debugAfter.indexOf("#define SOUND_LIST_SE");
        const qsizetype overAt = debugAfter.indexOf("X(SE_OVER)");
        check(overAt >= 0 && seListAt >= 0 && overAt < seListAt,
              "regioned: overflow SE's debug entry not routed to SOUND_LIST_BGM");
        check(readAllBytes(songsHPath).contains(defLine("END_MUS", "SE_OVER")),
              "regioned: END_MUS does not follow the overflow SE");

        // Restore the scratch project.
        writeFixture(tablePath, table0);
        writeFixture(songsHPath, songsH0);
        writeFixture(ldPath, ld0);
        writeFixture(charmapPath, charmap0);
        if (debug0.isEmpty())
            check(QFile::remove(debugCPath), "regioned: remove src/debug.c fixture");
        else
            writeFixture(debugCPath, debug0);
    }

    // ---- Register Song action wiring ----------------------------------------
    // Strip the song's charmap line — the state of any song registered before
    // porydaw wrote charmap entries. The song still reads as registered from
    // the song table, but File → Register Song must stay enabled, and running
    // it must backfill the line byte-identically.
    if (plan.charmapApplicable) {
        const QByteArray full = readAllBytes(charmapPath);
        QByteArray stripped = full;
        const int at = stripped.indexOf(plan.charmapLine.toUtf8());
        check(at >= 0, "action check: charmap entry not found");
        int end = stripped.indexOf('\n', at);
        end = end < 0 ? stripped.size() : end + 1;
        stripped.remove(at, end - at);
        QFile out(charmapPath);
        check(out.open(QIODevice::WriteOnly) && out.write(stripped) == stripped.size(),
              "action check: rewrite charmap.txt");
        out.close();

        // Redirected settings: the user's real session is never touched.
        QTemporaryDir settingsDir;
        check(settingsDir.isValid(), "action check: no temp dir for settings");
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        MainWindow window;
        check(window.runRegisterActionCheck(projectRoot, label),
              "register-action check did not run");
        check(readAllBytes(charmapPath) == full,
              "backfill did not restore charmap.txt byte-identically");
    }

    if (haveMid2agb)
        check(compilesThroughMid2agb(mid2agb, midPath, cfg.rawFlags),
              "blank new song does not compile through mid2agb");

    // ---- Import flow --------------------------------------------------------
    const SmfFile external = makeExternalMidi();
    ImportAnalysis analysis = analyzeForImport(external);
    check(analysis.mappedTracks == 2, "import: mapped track count");
    check(analysis.peakConcurrentNotes == 7, "import: peak polyphony");
    check(analysis.sampleNoteLimit == 5, "import: sample note limit");
    bool sawDivisionWarning = false, sawPolyWarning = false;
    for (const QString &w : analysis.warnings) {
        if (w.contains(QStringLiteral("note timing")))
            sawDivisionWarning = true;
        if (w.contains(QStringLiteral("same time")))
            sawPolyWarning = true;
    }
    check(sawDivisionWarning, "import: no division warning for 480 ppqn");
    check(sawPolyWarning, "import: no polyphony warning for 7-note chord");
    bool sawMod = false, sawInert = false;
    for (const ImportCcUsage &cc : analysis.ccs) {
        if (cc.cc == 1)
            sawMod = cc.audible;
        if (cc.cc == 91)
            sawInert = !cc.audible;
    }
    check(sawMod, "import: CC1 not classified audible");
    check(sawInert, "import: CC91 not classified inert");
    check(analysis.tracks.size() == 2 && analysis.tracks[1].programs.size() == 2,
          "import: per-track program usage");
    check(analysis.silentTracks == 0, "import: no budget warning at default 16");

    // Track-budget warning: with a 1-track player, the second mapped track is
    // silent in-game and the warning names the player and its allocation.
    {
        const ImportAnalysis tight =
            analyzeForImport(external, 1, QStringLiteral("MUSIC_PLAYER_BGM"));
        check(tight.silentTracks == 1, "import: silent track counted");
        bool sawBudgetWarning = false;
        for (const QString &w : tight.warnings) {
            if (w.contains(QStringLiteral("MUSIC_PLAYER_BGM")) &&
                w.contains(QStringLiteral("will not play")))
                sawBudgetWarning = true;
        }
        check(sawBudgetWarning, "import: budget warning names the player");
        check(analyzeForImport(external, -1).silentTracks == 0,
              "import: unknown budget warns about nothing");
    }

    SmfFile imported = external;

    // Division rescale onto the 24-clock grid (the wizard's default for a
    // non-multiple-of-24 file). Floor arithmetic matches mid2agb, so the
    // chord's onset lands where an as-is import would have played it:
    // 480 * 24 / 400 = 28.8 -> 28, offs 960 -> 57, EOT 3840 -> 230.
    rescaleDivision(&imported, 24);
    check(imported.division == 24, "rescale: division not rewritten");
    check(imported.tracks[1].events[4].tick == 28, "rescale: note-on tick");
    check(imported.tracks[1].events[11].tick == 57, "rescale: note-off tick");
    check(imported.tracks[1].endTick == 230, "rescale: end-of-track tick");
    check(imported.tracks[2].events[0].tick == 0, "rescale: tick-0 event moved");
    for (const SmfTrack &track : imported.tracks) {
        uint64_t prev = 0;
        for (const SmfEvent &ev : track.events) {
            check(ev.tick >= prev, "rescale: tick order regressed");
            prev = ev.tick;
        }
    }

    // The wizard end of the same option: the analysis page offers the rescale
    // (default on) and songFile() applies it with the Sound page's clock base.
    {
        NewSongWizard wizard(&project, external, QStringLiteral("ext.mid"), vgArgs);
        auto *rescale = wizard.page(0)->findChild<QCheckBox *>();
        check(rescale && rescale->isChecked(),
              "wizard: rescale checkbox missing or off for division 400");
        check(wizard.songFile().division == 24, "wizard: songFile() not rescaled by default");
        if (rescale) {
            rescale->setChecked(false);
            check(wizard.songFile().division == 400,
                  "wizard: opting out of the rescale still rescaled");
        }

        // The name validator folds typed capitals (Shift, Caps Lock) into the
        // lowercase convention instead of swallowing the keystroke; characters
        // outside the label grammar are still rejected outright.
        QLineEdit *nameEdit = nullptr;
        for (QLineEdit *edit : wizard.findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == QStringLiteral("mus_my_song"))
                nameEdit = edit;
        }
        check(nameEdit, "wizard: name field not found");
        if (nameEdit) {
            nameEdit->clear();
            nameEdit->insert(QStringLiteral("MUS_Loud_3"));
            check(nameEdit->text() == QStringLiteral("mus_loud_3"),
                  "wizard: typed capitals not folded to lowercase");
            nameEdit->clear();
            nameEdit->insert(QStringLiteral("mus 3!"));
            check(nameEdit->text().isEmpty(),
                  "wizard: characters outside the label grammar accepted");
        }

        // Reverb is a plain value, not an optional override: the wizard emits
        // an explicit -R at the vanilla STD_REVERB default.
        check(wizard.cfg().reverb == SongCfg::kDefaultReverb,
              "wizard: reverb does not default to 50");
        check(wizard.cfg().rawFlags.contains(QStringLiteral("-R50")),
              "wizard: default reverb not written as an explicit -R flag");

        // Song Settings likewise: a cfg whose flags lack -R comes back healed
        // to the default rather than staying absent.
        SongCfg bare;
        SongSettingsDialog dialog(bare, QStringLiteral("mus_bare"), vgArgs);
        check(dialog.cfg().reverb == SongCfg::kDefaultReverb,
              "song settings: absent -R does not heal to the default reverb");

        // Role-aware analysis: the analysis page's player choice and the
        // identity page's are one selection, kept in sync from either side,
        // and the analysis text tracks the chosen player's track budget.
        auto *analysisCombo = wizard.page(0)->findChild<QComboBox *>();
        auto *identityCombo = wizard.page(1)->findChild<QComboBox *>();
        check(analysisCombo && identityCombo, "wizard: player combos not found");
        if (analysisCombo && identityCombo) {
            bool synced = analysisCombo->currentData() == identityCombo->currentData();
            for (int i = analysisCombo->count() - 1; i >= 0; i--) {
                analysisCombo->setCurrentIndex(i);
                synced = synced && identityCombo->currentData() == analysisCombo->currentData();
            }
            check(synced, "wizard: identity player does not follow the analysis page");
            identityCombo->setCurrentIndex(identityCombo->count() - 1);
            check(analysisCombo->currentIndex() == identityCombo->currentIndex(),
                  "wizard: analysis player does not follow the identity page");
            identityCombo->setCurrentIndex(0);

            // A 1-track player mutes the external file's second track, and
            // the page says so in the singular. The selection also decides
            // what player() hands the song registration.
            const QVector<MusicPlayer> players = SongRegistry::musicPlayers(projectRoot);
            int tight = -1;
            for (int i = 0; i < players.size(); i++)
                if (players[i].trackCount == 1)
                    tight = i;
            if (tight >= 0) {
                analysisCombo->setCurrentIndex(analysisCombo->findData(players[tight].name));
                bool sawMute = false;
                for (const QLabel *label : wizard.page(0)->findChildren<QLabel *>())
                    if (label->text().contains(QStringLiteral("mute track 2")))
                        sawMute = true;
                check(sawMute, "wizard: 1-track player does not warn about muting track 2");
                check(wizard.player() == players[tight].name,
                      "wizard: player() does not return the engine symbol");
                analysisCombo->setCurrentIndex(0);
            }
        }
    }

    // Same-tick duplicate setters: the import silently keeps only the last of
    // each same-slot run (exporters love repeating the channel-init block),
    // while events whose every occurrence acts — notes, text metas, MEMACC
    // plumbing, the loop Label — survive untouched and in order.
    {
        const auto metaEvent = [](uint64_t tick, uint8_t type, QByteArray blob) {
            SmfEvent ev;
            ev.tick = tick;
            ev.status = 0xFF;
            ev.metaType = type;
            ev.blob = std::move(blob);
            return ev;
        };
        const auto channelEvent = [](uint8_t status, uint64_t tick, uint8_t d0, uint8_t d1) {
            SmfEvent ev;
            ev.status = status;
            ev.tick = tick;
            ev.data0 = d0;
            ev.data1 = d1;
            return ev;
        };
        SmfFile dups;
        dups.format = 1;
        dups.division = 24;
        SmfTrack conductor;
        conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x06\x1A\x80", 3))); // 150 BPM
        conductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3))); // 120 BPM
        conductor.events.push_back(metaEvent(0, 0x01, QByteArrayLiteral("a")));
        conductor.events.push_back(metaEvent(0, 0x01, QByteArrayLiteral("a")));
        conductor.endTick = 192;
        dups.tracks.push_back(conductor);
        SmfTrack ch0;
        ch0.events.push_back(channelEvent(0xC0, 0, 5, 0));
        ch0.events.push_back(channelEvent(0xB0, 0, 7, 100));
        ch0.events.push_back(channelEvent(0xC0, 0, 9, 0));
        ch0.events.push_back(channelEvent(0xE0, 0, 0, 0x20));
        ch0.events.push_back(channelEvent(0xB0, 0, 101, 0));
        ch0.events.push_back(channelEvent(0xB0, 0, 7, 80));
        ch0.events.push_back(channelEvent(0xB0, 0, 0x0D, 1)); // MEMACC op select
        ch0.events.push_back(channelEvent(0xB0, 0, 0x11, 2)); // loop Label
        ch0.events.push_back(channelEvent(0xC0, 0, 12, 0));
        ch0.events.push_back(channelEvent(0xE0, 0, 0, 0x40));
        ch0.events.push_back(channelEvent(0xB0, 0, 101, 0));
        ch0.events.push_back(channelEvent(0xB0, 0, 0x0D, 1));
        ch0.events.push_back(channelEvent(0xB0, 0, 0x11, 3));
        ch0.events.push_back(channelEvent(0xA0, 0, 60, 10));
        ch0.events.push_back(channelEvent(0xA0, 0, 61, 11));
        ch0.events.push_back(channelEvent(0xA0, 0, 60, 12));
        ch0.events.push_back(channelEvent(0x90, 0, 60, 100));
        ch0.events.push_back(channelEvent(0x90, 0, 64, 100));
        ch0.events.push_back(channelEvent(0xB0, 96, 7, 70));
        ch0.events.push_back(channelEvent(0xC0, 96, 3, 0));
        ch0.events.push_back(channelEvent(0xB0, 96, 7, 60));
        ch0.events.push_back(channelEvent(0x80, 192, 60, 0));
        ch0.events.push_back(channelEvent(0x80, 192, 64, 0));
        ch0.events.push_back(channelEvent(0xB0, 192, 7, 50));
        ch0.endTick = 192;
        dups.tracks.push_back(ch0);

        SmfFile direct = dups;
        // A tempo, 2 programs, CC7, bend, CC101, and polyAT(60) at tick 0,
        // plus the CC7 at 96.
        check(removeRedundantSetterEvents(&direct) == 8, "dedup: wrong removal count");
        check(removeRedundantSetterEvents(&direct) == 0, "dedup: not idempotent");
        const auto countEvents = [](const SmfTrack &track, uint8_t nibble, int cc) {
            int n = 0;
            for (const SmfEvent &ev : track.events) {
                if (ev.isChannel() && ev.typeNibble() == nibble && (cc < 0 || ev.data0 == cc))
                    n++;
            }
            return n;
        };
        int tempoCount = 0, textCount = 0;
        for (const SmfEvent &ev : direct.tracks[0].events) {
            if (!ev.isMeta())
                continue;
            if (ev.metaType == 0x51) {
                tempoCount++;
                check(ev.blob == QByteArray("\x07\xA1\x20", 3), "dedup: kept the wrong tempo");
            }
            textCount += ev.metaType == 0x01 ? 1 : 0;
        }
        check(tempoCount == 1, "dedup: same-tick tempo metas not collapsed");
        check(textCount == 2, "dedup: text metas were touched");
        const SmfTrack &lead = direct.tracks[1];
        check(countEvents(lead, 0xC, -1) == 2, "dedup: program-change count");
        check(countEvents(lead, 0xB, 7) == 3, "dedup: CC7 count");
        check(countEvents(lead, 0xE, -1) == 1, "dedup: bend count");
        check(countEvents(lead, 0xB, 101) == 1, "dedup: inert-CC count");
        check(countEvents(lead, 0xB, 0x0D) == 2, "dedup: MEMACC CCs were touched");
        check(countEvents(lead, 0xB, 0x11) == 2, "dedup: Label CCs were touched");
        check(countEvents(lead, 0xA, 60) == 1 && countEvents(lead, 0xA, 61) == 1,
              "dedup: poly-aftertouch not keyed per note");
        check(countEvents(lead, 0x9, -1) == 2 && countEvents(lead, 0x8, -1) == 2,
              "dedup: notes were touched");
        // The kept run preserves values, positions, and relative order: the
        // winners are the LAST of each run, still ahead of the tick's notes,
        // and the Label pair keeps its 2-then-3 file order.
        bool progOk = false, ccOk = false, bendOk = false, orderOk = true;
        int lastLabel = 0, firstNoteIdx = -1, progIdx = -1;
        for (size_t i = 0; i < lead.events.size(); i++) {
            const SmfEvent &ev = lead.events[i];
            if (ev.tick != 0)
                break;
            if (ev.typeNibble() == 0xC) {
                progOk = ev.data0 == 12;
                progIdx = int(i);
            }
            if (ev.typeNibble() == 0xB && ev.data0 == 7)
                ccOk = ev.data1 == 80;
            if (ev.typeNibble() == 0xE)
                bendOk = ev.data1 == 0x40;
            if (ev.typeNibble() == 0xB && ev.data0 == 0x11) {
                orderOk = orderOk && ev.data1 > lastLabel;
                lastLabel = ev.data1;
            }
            if (ev.typeNibble() == 0x9 && firstNoteIdx < 0)
                firstNoteIdx = int(i);
        }
        check(progOk && ccOk && bendOk, "dedup: a run's survivor is not its last value");
        check(orderOk && lastLabel == 3, "dedup: kept events lost their relative order");
        check(progIdx >= 0 && firstNoteIdx > progIdx,
              "dedup: setter drifted past the tick's notes");
        for (const SmfTrack &track : direct.tracks) {
            uint64_t prev = 0;
            for (const SmfEvent &ev : track.events) {
                check(ev.tick >= prev, "dedup: tick order regressed");
                prev = ev.tick;
            }
        }

        // The wizard end: songFile() applies the dedup silently on import.
        NewSongWizard wizard(&project, dups, QStringLiteral("dups.mid"), vgArgs);
        const SmfFile cleaned = wizard.songFile();
        check(cleaned.tracks.size() == 2 && countEvents(cleaned.tracks[1], 0xB, 7) == 3 &&
                  countEvents(cleaned.tracks[1], 0xC, -1) == 2,
              "wizard: songFile() did not dedup same-tick setters");

        // Foreign tempo maps: mid2agb reads tempo from the first chunk only,
        // so import moves later-chunk 0x51 metas there — tick order kept,
        // the file-order winner of a shared tick still last (later chunk
        // after earlier, moved after resident), other events untouched —
        // and the dedup that follows collapses the losers.
        SmfFile stray;
        stray.format = 1;
        stray.division = 24;
        SmfTrack sConductor;
        sConductor.events.push_back(metaEvent(0, 0x51, QByteArray("\x06\x1A\x80", 3)));   // 150
        sConductor.events.push_back(metaEvent(192, 0x51, QByteArray("\x0A\x2C\x2B", 3))); // 90
        sConductor.endTick = 192;
        stray.tracks.push_back(sConductor);
        SmfTrack sLead;
        sLead.events.push_back(metaEvent(0, 0x01, QByteArrayLiteral("keep me")));
        sLead.events.push_back(metaEvent(0, 0x51, QByteArray("\x07\xA1\x20", 3))); // 120
        sLead.events.push_back(channelEvent(0x90, 0, 60, 100));
        sLead.events.push_back(metaEvent(48, 0x51, QByteArray("\x0F\x42\x40", 3))); // 60
        sLead.events.push_back(channelEvent(0x80, 96, 60, 0));
        sLead.endTick = 192;
        stray.tracks.push_back(sLead);
        SmfTrack sExtra;
        sExtra.events.push_back(metaEvent(48, 0x51, QByteArray("\x0C\x35\x00", 3)));  // 75
        sExtra.events.push_back(metaEvent(240, 0x51, QByteArray("\x09\x27\xC0", 3))); // 100
        sExtra.endTick = 240;
        stray.tracks.push_back(sExtra);

        SmfFile movedFile = stray;
        check(moveTempoMetasToFirstChunk(&movedFile) == 4, "tempo move: wrong moved count");
        check(moveTempoMetasToFirstChunk(&movedFile) == 0, "tempo move: not idempotent");
        const auto tempoBlobs = [](const SmfTrack &track) {
            QList<QByteArray> blobs;
            for (const SmfEvent &ev : track.events)
                if (ev.isMeta() && ev.metaType == 0x51)
                    blobs.push_back(ev.blob);
            return blobs;
        };
        check(tempoBlobs(movedFile.tracks[1]).isEmpty() &&
                  tempoBlobs(movedFile.tracks[2]).isEmpty(),
              "tempo move: metas left behind");
        check(movedFile.tracks[1].events.size() == 3 && movedFile.tracks[2].events.size() == 0,
              "tempo move: touched non-tempo events");
        check(tempoBlobs(movedFile.tracks[0]) ==
                  (QList<QByteArray>{QByteArray("\x06\x1A\x80", 3), QByteArray("\x07\xA1\x20", 3),
                                     QByteArray("\x0F\x42\x40", 3), QByteArray("\x0C\x35\x00", 3),
                                     QByteArray("\x0A\x2C\x2B", 3), QByteArray("\x09\x27\xC0", 3)}),
              "tempo move: wrong order in the first chunk");
        {
            uint64_t prev = 0;
            for (const SmfEvent &ev : movedFile.tracks[0].events) {
                check(ev.tick >= prev, "tempo move: tick order regressed");
                prev = ev.tick;
            }
        }
        check(movedFile.tracks[0].endTick == 240, "tempo move: endTick not extended");
        removeRedundantSetterEvents(&movedFile);
        check(tempoBlobs(movedFile.tracks[0]) ==
                  (QList<QByteArray>{QByteArray("\x07\xA1\x20", 3), QByteArray("\x0C\x35\x00", 3),
                                     QByteArray("\x0A\x2C\x2B", 3), QByteArray("\x09\x27\xC0", 3)}),
              "tempo move: dedup kept the wrong same-tick winners");

        // The wizard end applies the move (then the dedup) silently.
        NewSongWizard strayWizard(&project, stray, QStringLiteral("stray.mid"), vgArgs);
        const SmfFile strayCleaned = strayWizard.songFile();
        check(strayCleaned.tracks.size() == 3 && tempoBlobs(strayCleaned.tracks[1]).isEmpty() &&
                  tempoBlobs(strayCleaned.tracks[2]).isEmpty() &&
                  tempoBlobs(strayCleaned.tracks[0]).size() == 4,
              "wizard: songFile() did not move foreign tempo metas");
    }

    const QString importLabel = QStringLiteral("mus_onboardcheck_import");
    const QString importMid = midiDir + QStringLiteral("/%1.mid").arg(importLabel);
    check(imported.writeFile(importMid, &error), "write imported .mid");
    check(SongRegistry::writeMidiCfgLine(midiDir, importLabel, cfg.rawFlags, &error),
          "write imported midi.cfg line");

    SmfFile reread;
    check(SmfFile::readFile(importMid, &reread, &error) &&
              reread.tracks.size() == imported.tracks.size() && reread.division == 24,
          "imported .mid does not re-read cleanly");

    check(project.reload(&error), "project reload after import");
    const SongInfo *importedSong = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == importLabel)
            importedSong = &s;
    }
    check(importedSong && importedSong->isPlayable() && !importedSong->registered,
          "imported song not discovered");
    if (importedSong) {
        SongDocument doc;
        check(doc.load(*importedSong, &error), "imported song fails to open");
        check(doc.engineTrackCount() == 2, "imported song engine track count");
    }

    if (haveMid2agb)
        check(compilesThroughMid2agb(mid2agb, importMid, cfg.rawFlags),
              "imported song does not compile through mid2agb");

    // ---- Delete Song --------------------------------------------------------
    // The inverse of the flows above. A full create→register→delete cycle
    // must leave every file byte-identical; a mid-table delete leaves a free
    // slot — a plain duplicate of entry 0's dummy line — that keeps later
    // IDs stable and is reused by the next registration; entry 0 itself (the
    // fallback song) is untouchable either way.
    const QString cfgPath = midiDir + QStringLiteral("/midi.cfg");
    QString firstLabel; // the song table's entry 0
    {
        const QByteArray table0 = readAllBytes(tablePath);
        const QByteArray songsH0 = readAllBytes(songsHPath);
        const QByteArray ld0 = readAllBytes(ldPath);
        const QByteArray charmap0 = readAllBytes(charmapPath);
        const QByteArray cfg0 = readAllBytes(cfgPath);
        // Empty on vanilla; on an expansion checkout the delete cycle must
        // round-trip the debug menu's sound lists too.
        const QString debugCPath = projectRoot + QStringLiteral("/src/debug.c");
        const QByteArray debug0 = readAllBytes(debugCPath);

        static const QRegularExpression songEntryRe(QStringLiteral(R"(^\s*song\s+(\w+))"));
        int tableEntries = 0;
        for (const QByteArray &line : table0.split('\n')) {
            const QRegularExpressionMatch m = songEntryRe.match(QString::fromUtf8(line));
            if (!m.hasMatch())
                continue;
            if (firstLabel.isEmpty())
                firstLabel = m.captured(1);
            tableEntries++;
        }
        check(!firstLabel.isEmpty(), "delete: no entry 0 in song_table.inc");
        // Entries bearing entry 0's label; one more than at the snapshot
        // means one free slot is open.
        const auto dummyEntries = [&]() {
            int n = 0;
            for (const QByteArray &line : readAllBytes(tablePath).split('\n')) {
                const QRegularExpressionMatch m = songEntryRe.match(QString::fromUtf8(line));
                if (m.hasMatch() && m.captured(1) == firstLabel)
                    n++;
            }
            return n;
        };
        const int dummies0 = dummyEntries();

        const QString labelA = QStringLiteral("mus_onboardcheck_del_a");
        const QString labelB = QStringLiteral("mus_onboardcheck_del_b");
        const QString labelC = QStringLiteral("mus_onboardcheck_del_c");
        const auto createAndRegister = [&](const QString &lab, int *id) {
            const SmfFile smf = SongRegistry::blankSong();
            check(smf.writeFile(midiDir + QStringLiteral("/%1.mid").arg(lab), &error),
                  "delete: write .mid");
            check(SongRegistry::writeSongFlags(midiDir, lab, cfg.rawFlags, &error),
                  "delete: write flags");
            check(SongRegistry::registerSong(projectRoot, lab, SongRegistry::constantForLabel(lab),
                                             QStringLiteral("MUSIC_PLAYER_BGM"), &regError, id),
                  "delete: registerSong failed");
        };
        const auto deleteSong = [&](const QString &lab) {
            QString err;
            check(SongRegistry::unregisterSong(projectRoot, lab,
                                               SongRegistry::constantForLabel(lab), &err),
                  "delete: unregisterSong failed");
            check(SongRegistry::removeSongFlags(midiDir, lab, &err),
                  "delete: removeSongFlags failed");
            check(QFile::remove(midiDir + QStringLiteral("/%1.mid").arg(lab)),
                  "delete: remove .mid");
        };

        int idA = -1, idB = -1, idC = -1;
        createAndRegister(labelA, &idA);
        createAndRegister(labelB, &idB);
        check(idB == idA + 1, "delete: fresh registrations not sequential");

        // Mid-table delete: A leaves a free slot; B keeps its ID.
        deleteSong(labelA);
        check(dummyEntries() == dummies0 + 1, "mid-table delete left no free slot");
        check(!readAllBytes(songsHPath).contains("MUS_ONBOARDCHECK_DEL_A"),
              "deleted song's define still in songs.h");
        check(!readAllBytes(ldPath).contains("mus_onboardcheck_del_a.o"),
              "deleted song's object line still in ld_script.ld");
        check(!readAllBytes(charmapPath).contains("MUS_ONBOARDCHECK_DEL_A"),
              "deleted song's charmap entry still present");
        check(!readAllBytes(debugCPath).contains("MUS_ONBOARDCHECK_DEL_A"),
              "deleted song's debug menu entry still present");
        check(!readAllBytes(cfgPath).contains("mus_onboardcheck_del_a.mid"),
              "deleted song's midi.cfg line still present");
        RegistrationStatus after = SongRegistry::checkRegistration(
            projectRoot, labelB, SongRegistry::constantForLabel(labelB));
        check(after.complete(), "surviving song's registration broke on delete");
        // The free slot borrows entry 0's label without impersonating it:
        // the fallback song must still read as correctly registered.
        after = SongRegistry::checkRegistration(projectRoot, firstLabel,
                                                SongRegistry::constantForLabel(firstLabel));
        check(after.inSongTable && after.inSongsH,
              "free slot misattributed the fallback song's table entry");

        // Reuse: the next song is offered the freed ID, and its lines land
        // in ID order (songs.h sorted like the charmap insertion).
        const RegistrationPlan planC =
            SongRegistry::makePlan(projectRoot, labelC, SongRegistry::constantForLabel(labelC),
                                   QStringLiteral("MUSIC_PLAYER_BGM"));
        check(planC.songId == idA, "free slot not proposed for the next song");
        createAndRegister(labelC, &idC);
        check(idC == idA, "free slot not reused on registration");
        check(dummyEntries() == dummies0, "reused slot kept its dummy entry");
        {
            const QByteArray songsH = readAllBytes(songsHPath);
            const auto defineAt = [&songsH](const char *constant) {
                return songsH.indexOf(QByteArray("#define ") + constant);
            };
            check(defineAt("MUS_ONBOARDCHECK_DEL_C") >= 0 &&
                      defineAt("MUS_ONBOARDCHECK_DEL_C") < defineAt("MUS_ONBOARDCHECK_DEL_B"),
                  "reused ID's define not inserted in songs.h ID order");
        }
        if (plan.charmapApplicable) {
            const QByteArray charmap = readAllBytes(charmapPath);
            check(charmap.indexOf("MUS_ONBOARDCHECK_DEL_C") >= 0 &&
                      charmap.indexOf("MUS_ONBOARDCHECK_DEL_C") <
                          charmap.indexOf("MUS_ONBOARDCHECK_DEL_B"),
                  "reused ID's charmap entry not in ID order");
        }

        // Deleting an already-deleted song is a byte-level no-op success.
        {
            const QByteArray t = readAllBytes(tablePath);
            const QByteArray h = readAllBytes(songsHPath);
            QString err;
            check(SongRegistry::unregisterSong(projectRoot, labelA,
                                               SongRegistry::constantForLabel(labelA), &err),
                  "second unregister failed");
            check(readAllBytes(tablePath) == t && readAllBytes(songsHPath) == h,
                  "second unregister was not byte-identical");
        }

        // Wind back down: C leaves the slot again; B's last-entry delete then
        // collapses the trailing free slot. Everything must round-trip to the
        // pre-cycle bytes.
        deleteSong(labelC);
        check(dummyEntries() == dummies0 + 1, "re-deleted slot is not free again");
        deleteSong(labelB);
        check(readAllBytes(tablePath) == table0, "song_table.inc did not round-trip");
        check(readAllBytes(songsHPath) == songsH0, "songs.h did not round-trip");
        check(readAllBytes(ldPath) == ld0, "ld_script.ld did not round-trip");
        check(readAllBytes(charmapPath) == charmap0, "charmap.txt did not round-trip");
        check(readAllBytes(cfgPath) == cfg0, "midi.cfg did not round-trip");
        check(readAllBytes(debugCPath) == debug0, "src/debug.c did not round-trip");

        // Entry 0 is never deletable...
        {
            QString err;
            check(!SongRegistry::unregisterSong(projectRoot, firstLabel,
                                                SongRegistry::constantForLabel(firstLabel), &err) &&
                      !err.isEmpty(),
                  "unregisterSong deleted the fallback song");
            check(readAllBytes(tablePath) == table0, "refused delete still wrote");
        }
        // ...and never a free slot: entry 0 bears the dummy label like any
        // tombstone would, but the planner must not offer ID 0 — on a table
        // whose only dummy entry IS entry 0, it appends.
        {
            const RegistrationPlan probed = SongRegistry::makePlan(
                projectRoot, QStringLiteral("mus_onboardcheck_probe"),
                QStringLiteral("MUS_ONBOARDCHECK_PROBE"), QStringLiteral("MUSIC_PLAYER_BGM"));
            check(probed.songId != 0, "entry 0 was offered as a free slot");
            if (dummies0 == 1)
                check(probed.songId == tableEntries,
                      "planner did not append with no free slots open");
        }

        // An unregistered stray (the imported song): no table entry at all,
        // so deletion is just the .mid and the cfg line.
        {
            QString err;
            check(SongRegistry::unregisterSong(projectRoot, importLabel,
                                               SongRegistry::constantForLabel(importLabel), &err),
                  "stray unregister failed");
            check(readAllBytes(tablePath) == table0, "stray unregister touched song_table.inc");
            check(SongRegistry::removeSongFlags(midiDir, importLabel, &err),
                  "stray removeSongFlags failed");
            check(!readAllBytes(cfgPath).contains(importLabel.toUtf8() + ".mid"),
                  "stray's midi.cfg line still present");
        }
    }

    // ---- Delete voicegroup --------------------------------------------------
    // deletableVoicegroup gates the offer: sole song user, per-file layout,
    // no keysplit/drumkit reference, no C reference. deleteVoicegroup then
    // inverts createVoicegroup + appendIncludeLine byte-identically.
    {
        const QString hubPath = projectRoot + QStringLiteral("/sound/voice_groups.inc");
        const QByteArray hub0 = readAllBytes(hubPath);
        const QString vgName = QStringLiteral("onboardcheckvg");
        const bool created =
            VoicegroupSource::createVoicegroup(projectRoot, vgName, QString(), QString(), &error) &&
            VoicegroupSource::appendIncludeLine(projectRoot, vgName, &error);
        check(created, "vg delete: createVoicegroup/appendIncludeLine failed");
        if (created) {
            QVector<SongInfo> songs = project.songs();
            SongInfo user;
            user.label = QStringLiteral("mus_vg_user");
            user.cfg.voicegroupArg = QStringLiteral("_onboardcheckvg");
            songs.append(user);
            check(SongRegistry::deletableVoicegroup(projectRoot, songs, user.label) == vgName,
                  "sole-user voicegroup not deletable");

            SongInfo second = user;
            second.label = QStringLiteral("mus_vg_user2");
            songs.append(second);
            check(SongRegistry::deletableVoicegroup(projectRoot, songs, user.label).isEmpty(),
                  "shared voicegroup offered for deletion");
            songs.removeLast();

            // A keysplit reference from another voicegroup is load-bearing.
            const QString subName = QStringLiteral("onboardchecksub");
            check(VoicegroupSource::createVoicegroup(projectRoot, subName, QString(), QString(),
                                                     &error),
                  "vg delete: create sub voicegroup");
            {
                QFile host(projectRoot + QStringLiteral("/sound/voicegroups/onboardcheckvg.inc"));
                check(host.open(QIODevice::Append), "vg delete: append keysplit line");
                host.write("\tvoice_keysplit voicegroup_onboardchecksub, "
                           "KeySplitTable1\n");
            }
            SongInfo subUser = user;
            subUser.label = QStringLiteral("mus_vg_sub_user");
            subUser.cfg.voicegroupArg = QStringLiteral("_onboardchecksub");
            QVector<SongInfo> subSongs = songs;
            subSongs.append(subUser);
            check(SongRegistry::deletableVoicegroup(projectRoot, subSongs, subUser.label).isEmpty(),
                  "keysplit sub-voicegroup offered for deletion");
            check(VoicegroupSource::deleteVoicegroup(projectRoot, subName, &error),
                  "vg delete: remove sub voicegroup");

            // A C reference would break the link, not merely dangle.
            const QString refPath = projectRoot + QStringLiteral("/src/onboardcheck_ref.c");
            {
                QFile ref(refPath);
                check(ref.open(QIODevice::WriteOnly), "vg delete: write C ref");
                ref.write("extern int voicegroup_onboardcheckvg[];\n");
            }
            check(SongRegistry::deletableVoicegroup(projectRoot, songs, user.label).isEmpty(),
                  "C-referenced voicegroup offered for deletion");
            QFile::remove(refPath);
            check(SongRegistry::deletableVoicegroup(projectRoot, songs, user.label) == vgName,
                  "vg delete: dropped C reference not re-detected");

            check(VoicegroupSource::deleteVoicegroup(projectRoot, vgName, &error),
                  "deleteVoicegroup failed");
            check(!QFile::exists(projectRoot +
                                 QStringLiteral("/sound/voicegroups/onboardcheckvg.inc")),
                  "voicegroup file survived deletion");
            check(readAllBytes(hubPath) == hub0, "voice_groups.inc did not round-trip");
            check(VoicegroupSource::deleteVoicegroup(projectRoot, vgName, &error),
                  "second deleteVoicegroup failed");
        }
    }

    // ---- Delete action wiring ----------------------------------------------
    // The MainWindow path: deleting an open song closes its tab, drops it
    // from the model and browser, and moves the .mid to .porydaw/trash/.
    {
        const QString delLabel = QStringLiteral("mus_onboardcheck_del_ui");
        const SmfFile smf = SongRegistry::blankSong();
        check(smf.writeFile(midiDir + QStringLiteral("/%1.mid").arg(delLabel), &error),
              "action delete: write .mid");
        check(SongRegistry::writeSongFlags(midiDir, delLabel, cfg.rawFlags, &error),
              "action delete: write flags");
        int id = -1;
        check(SongRegistry::registerSong(projectRoot, delLabel,
                                         SongRegistry::constantForLabel(delLabel),
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError, &id),
              "action delete: registerSong failed");

        QTemporaryDir settingsDir;
        check(settingsDir.isValid(), "action delete: no temp dir for settings");
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        MainWindow window;
        check(window.runDeleteActionCheck(projectRoot, delLabel),
              "delete-action check did not run");
    }

    std::printf("onboardcheck: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}

bool MainWindow::runRegisterActionCheck(const QString &projectRoot, const QString &label)
{
    m_persistSession = false;
    // Native-format QSettings use the registry on Windows, so setPath() in
    // the harness cannot isolate a persisted song filter. Clear it explicitly
    // before asserting on the fixture's list item.
    m_songList->restoreFilters(QString(), 0, QString());
    if (!openProjectDir(projectRoot, /*interactive=*/false)) {
        std::fprintf(stderr, "onboardcheck: project failed to open in MainWindow\n");
        return false;
    }
    loadSongByLabel(label);
    if (!m_active || m_active->doc.label() != label) {
        std::fprintf(stderr, "onboardcheck: '%s' did not load in MainWindow\n",
                     qUtf8Printable(label));
        return false;
    }
    check(m_registerAction->isEnabled(),
          "Register Song disabled for a song missing its charmap entry");

    // The model carries the gap and the song browser badges it.
    const auto findSong = [this](const QString &wanted) -> const SongInfo * {
        for (const SongInfo &s : m_project.songs()) {
            if (s.label == wanted)
                return &s;
        }
        return nullptr;
    };
    const SongInfo *info = findSong(label);
    check(info && info->registered,
          "partially registered song no longer counts as table-registered");
    check(info && info->registrationGaps == QStringList{QStringLiteral("charmap.txt")},
          "registrationGaps does not name the stripped charmap entry");
    auto *list = m_songList->findChild<QListWidget *>();
    const auto itemFor = [list](int id) -> QListWidgetItem * {
        for (int i = 0; list && i < list->count(); i++) {
            if (list->item(i)->data(Qt::UserRole).toInt() == id)
                return list->item(i);
        }
        return nullptr;
    };
    QListWidgetItem *item = info ? itemFor(info->id) : nullptr;
    check(item && item->text().contains(QStringLiteral("not fully registered")),
          "song list shows no badge for a partial registration");

    // The context menu's Register Song path heals the registration.
    if (info)
        registerSongById(info->id);
    check(!m_registerAction->isEnabled(), "Register Song still enabled after backfill");
    info = findSong(label);
    check(info && info->registrationGaps.isEmpty(),
          "registration gaps not cleared by the backfill");
    item = info ? itemFor(info->id) : nullptr;
    check(item && item->text() == label, "badge not cleared after the backfill");
    // A fresh activation recomputes the enable state from the reloaded songs.
    activateSession(m_active, /*force=*/true);
    check(!m_registerAction->isEnabled(),
          "re-activation re-enabled Register Song for a complete registration");
    return true;
}

bool MainWindow::runDeleteActionCheck(const QString &projectRoot, const QString &label)
{
    m_persistSession = false;
    // Same registry-persisted-filter hole as runRegisterActionCheck above —
    // worse here, because a filter that hides the fixture makes the negative
    // !listed probe pass vacuously instead of flaking.
    m_songList->restoreFilters(QString(), 0, QString());
    if (!openProjectDir(projectRoot, /*interactive=*/false)) {
        std::fprintf(stderr, "onboardcheck: project failed to open in MainWindow\n");
        return false;
    }
    loadSongByLabel(label);
    if (!m_active || m_active->doc.label() != label) {
        std::fprintf(stderr, "onboardcheck: '%s' did not load in MainWindow\n",
                     qUtf8Printable(label));
        return false;
    }
    const SongInfo *info = nullptr;
    for (const SongInfo &s : m_project.songs()) {
        if (s.label == label)
            info = &s;
    }
    if (!info) {
        std::fprintf(stderr, "onboardcheck: '%s' not in the project model\n",
                     qUtf8Printable(label));
        return false;
    }
    const SongInfo song = *info; // survives the reload inside the deletion

    QString error;
    check(performSongDeletion(song, QString(), &error), "performSongDeletion failed");
    if (!error.isEmpty())
        std::fprintf(stderr, "onboardcheck: delete: %s\n", qUtf8Printable(error));
    check(!sessionForLabel(label), "deleted song's tab still open");
    bool inModel = false;
    for (const SongInfo &s : m_project.songs())
        inModel = inModel || s.label == label;
    check(!inModel, "deleted song still in the project model");
    auto *list = m_songList->findChild<QListWidget *>();
    bool listed = false;
    for (int i = 0; list && i < list->count(); i++)
        listed = listed || list->item(i)->text().startsWith(label);
    check(!listed, "deleted song still listed in the browser");
    check(!QFile::exists(projectRoot + QStringLiteral("/sound/songs/midi/%1.mid").arg(label)),
          "deleted song's .mid still in sound/songs/midi");
    check(QFile::exists(projectRoot + QStringLiteral("/.porydaw/trash/%1.mid").arg(label)),
          "deleted song's .mid not moved to .porydaw/trash");

    // The fallback song refuses deletion end to end, before any file edit.
    const SongInfo *fallback = nullptr;
    for (const SongInfo &s : m_project.songs()) {
        if (s.registered && s.id == 0)
            fallback = &s;
    }
    if (fallback) {
        const QByteArray tableBefore =
            readAllBytes(projectRoot + QStringLiteral("/sound/song_table.inc"));
        const QString fallbackMid =
            projectRoot + QStringLiteral("/sound/songs/midi/%1.mid").arg(fallback->label);
        const bool hadMid = QFile::exists(fallbackMid);
        QString refuse;
        check(!performSongDeletion(*fallback, QString(), &refuse) && !refuse.isEmpty(),
              "performSongDeletion deleted the fallback song");
        check(readAllBytes(projectRoot + QStringLiteral("/sound/song_table.inc")) == tableBefore,
              "refused fallback delete still edited song_table.inc");
        check(QFile::exists(fallbackMid) == hadMid, "refused fallback delete still moved the .mid");
    }
    return true;
}
