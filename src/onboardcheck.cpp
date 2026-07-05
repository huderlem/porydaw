#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <cstdio>

#include "core/midiimport.h"
#include "core/smf.h"
#include "core/songdocument.h"
#include "project/decompproject.h"
#include "project/songregistry.h"

// --onboardcheck <projectRoot> [mid2agbPath]: M3 onboarding check. Exercises
// the New Song and Import backends headlessly against a scratch copy of a
// project — it writes into it. Creates a song, verifies its files and sidecar,
// registers it (porydaw writes song_table.inc / songs.h / ld_script.ld
// directly), verifies idempotency and stale-ID correction, and runs an
// external-MIDI import (analysis + program remap), compiling both songs
// through the project's real mid2agb.

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
        check(created->constant == constant && created->player == QStringLiteral("MUSIC_PLAYER_BGM"),
              "sidecar registration meta not recalled");

        SongDocument doc;
        check(doc.load(*created, &error), "created song fails to open as a document");
    }

    RegistrationStatus status = SongRegistry::checkRegistration(projectRoot, label, constant);
    check(!status.inSongTable && !status.inSongsH && !status.complete(),
          "fresh song already looks registered");

    RegistrationPlan plan = SongRegistry::makePlan(projectRoot, label, constant,
                                                   QStringLiteral("MUSIC_PLAYER_BGM"));
    check(plan.songId == registeredCount, "proposed song ID != registered song count");
    check(plan.songTableLine.contains(QStringLiteral("song mus_onboardcheck, MUSIC_PLAYER_BGM, 0")),
          "song_table line malformed");
    check(plan.songsHLine.startsWith(QStringLiteral("#define MUS_ONBOARDCHECK"))
              && plan.songsHLine.endsWith(QString::number(plan.songId)),
          "songs.h line malformed");

    // porydaw writes the registration files itself.
    QString regError;
    int songId = -1;
    check(SongRegistry::registerSong(projectRoot, label, constant,
                                     QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                     &songId),
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

    // Registering again must be a byte-level no-op.
    const QByteArray tableBefore = readAllBytes(tablePath);
    const QByteArray songsHBefore = readAllBytes(songsHPath);
    const QByteArray ldBefore = readAllBytes(ldPath);
    check(SongRegistry::registerSong(projectRoot, label, constant,
                                     QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                     &songId),
          "second registerSong failed");
    check(songId == registeredCount, "song ID drifted on re-register");
    check(readAllBytes(tablePath) == tableBefore
              && readAllBytes(songsHPath) == songsHBefore
              && readAllBytes(ldPath) == ldBefore,
          "re-register was not byte-identical");

    // A songs.h define whose ID drifted from the table index gets corrected.
    {
        QByteArray tampered = songsHBefore;
        const QByteArray goodDefine =
            QStringLiteral("#define %1").arg(constant).toUtf8();
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
                                         QStringLiteral("MUSIC_PLAYER_BGM"), &regError,
                                         &songId),
              "registerSong after tamper failed");
        check(readAllBytes(songsHPath) == songsHBefore, "stale define not corrected");
    }

    check(project.reload(&error), "project reload after registration");
    const SongInfo *registered = nullptr;
    for (const SongInfo &s : project.songs()) {
        if (s.label == label)
            registered = &s;
    }
    check(registered && registered->registered, "song not registered after registerSong");
    check(registered && registered->id == registeredCount, "registered song ID wrong");
    check(registered && registered->constant == constant,
          "constant not matched from songs.h");

    if (haveMid2agb)
        check(compilesThroughMid2agb(mid2agb, midPath, cfg.rawFlags),
              "blank new song does not compile through mid2agb");

    // ---- Import flow --------------------------------------------------------
    const SmfFile external = makeExternalMidi();
    ImportAnalysis analysis = analyzeForImport(external);
    check(analysis.mappedTracks == 2, "import: mapped track count");
    check(analysis.peakConcurrentNotes == 7, "import: peak polyphony");
    bool sawDivisionWarning = false, sawPolyWarning = false;
    for (const QString &w : analysis.warnings) {
        if (w.contains(QStringLiteral("Division")))
            sawDivisionWarning = true;
        if (w.contains(QStringLiteral("at once")))
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

    // Remap program 5 -> 10 on the lead track, then land the song in the
    // project the same way the wizard does.
    SmfFile imported = external;
    applyProgramRemaps(&imported, {{analysis.tracks[0].smfTrack,
                                    analysis.tracks[0].channel, 5, 10}});
    int remapped = 0;
    for (const SmfEvent &ev : imported.tracks[1].events) {
        if (ev.isChannel() && ev.typeNibble() == 0xC)
            remapped += (ev.data0 == 10) ? 1 : 0;
    }
    check(remapped == 1, "import: program remap not applied");
    // The untouched track keeps its programs.
    check(imported.tracks[2].events[0].data0 == 20, "import: remap bled across tracks");

    const QString importLabel = QStringLiteral("mus_onboardcheck_import");
    const QString importMid = midiDir + QStringLiteral("/%1.mid").arg(importLabel);
    check(imported.writeFile(importMid, &error), "write imported .mid");
    check(SongRegistry::writeMidiCfgLine(midiDir, importLabel, cfg.rawFlags, &error),
          "write imported midi.cfg line");

    SmfFile reread;
    check(SmfFile::readFile(importMid, &reread, &error)
              && reread.tracks.size() == imported.tracks.size(),
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

    std::printf("onboardcheck: %s (%d failures)\n", g_failures ? "FAIL" : "PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
