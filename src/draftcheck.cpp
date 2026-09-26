#include <QAbstractButton>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <algorithm>
#include <cstdio>

#include "mainwindow.h"
#include "project/songregistry.h"

// --draftcheck <projectRoot>: draft songs (docs/draft-songs/PLAN.md). Import
// MIDI / New Song open a draft tab that writes nothing into the project:
// opening, editing and discarding one leaves every file outside .porydaw/
// untouched, and no view sidecar appears. Saving a draft commits it in
// place — .mid, flags, registration — keeping its session, view and undo
// history. QSettings is redirected into a temp dir; the commit writes into
// the project — run against a scratch copy.

namespace {

// One digest over every file outside .porydaw/: its relative path, size and
// modification time. Metadata, not contents: any write (even of identical
// bytes) moves the mtime, and the tree is too big to read four times over.
QByteArray treeFingerprint(const QString &root)
{
    QStringList entries;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString rel = info.filePath().mid(root.size() + 1);
        if (rel.startsWith(QLatin1String(".porydaw/")))
            continue;
        entries << QStringLiteral("%1|%2|%3")
                       .arg(rel)
                       .arg(info.size())
                       .arg(info.lastModified().toMSecsSinceEpoch());
    }
    entries.sort();
    return QCryptographicHash::hash(entries.join(QLatin1Char('\n')).toUtf8(),
                                    QCryptographicHash::Sha1);
}

// A one-bar song with one note, on top of New Song's blank template.
SmfFile draftSmf()
{
    SmfFile smf = SongRegistry::blankSong();
    SmfEvent on;
    on.status = 0x90;
    on.data0 = 60;
    on.data1 = 100;
    SmfEvent off = on;
    off.tick = 24;
    off.data1 = 0;
    smf.tracks[1].events.push_back(on);
    smf.tracks[1].events.push_back(off);
    return smf;
}

// Answers the next QMessageBox that opens (the close/save prompt) with
// button, from inside its own exec() loop. A box that never appears is
// never answered; one that shows a different button set is dismissed.
void answerNextPrompt(QWidget *window, QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, window, [window, button] {
        for (QMessageBox *box : window->findChildren<QMessageBox *>()) {
            if (!box->isVisible())
                continue;
            if (QAbstractButton *b = box->button(button))
                b->click();
            else
                box->reject(); // never hang the harness in exec()
            return;
        }
    });
}

} // namespace

int MainWindow::runDraftCheck(const QString &projectRoot)
{
    // m_persistSession stays true (the caller redirected QSettings): drafts
    // must stay out of the persisted tab list, and a committed one join it.
    if (!m_audioOk) {
        std::fprintf(stderr, "draftcheck: no audio device available\n");
        return 1;
    }
    if (!openProjectDir(projectRoot, /*interactive=*/false)) {
        std::fprintf(stderr, "draftcheck: project failed to open\n");
        return 1;
    }
    const QString root = m_project.root();

    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "draftcheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    // An existing voicegroup: the one a vanilla song already uses.
    SongCfg cfg;
    for (const SongInfo &song : m_project.songs()) {
        if (song.isPlayable() && !song.cfg.voicegroupArg.isEmpty()) {
            cfg.voicegroupArg = song.cfg.voicegroupArg;
            break;
        }
    }
    if (cfg.voicegroupArg.isEmpty()) {
        std::fprintf(stderr, "draftcheck: no song with a voicegroup in the project\n");
        return 1;
    }
    const QString player = QStringLiteral("MUSIC_PLAYER_BGM");
    const QString midiDir = root + QStringLiteral("/sound/songs/midi/");
    const auto persistedLabels = [] {
        return QSettings().value(QStringLiteral("lastOpenSongs")).toStringList();
    };

    const QByteArray pristine = treeFingerprint(root);
    QString error;

    // 1. Opening a draft writes nothing, yet the tab is unsaved: no song ID,
    // an asterisk, and the undo stack itself clean.
    const QString labelA = QStringLiteral("mus_draftcheck_a");
    if (!check(openDraftSong(draftSmf(), labelA, QStringLiteral("MUS_DRAFTCHECK_A"), player, cfg,
                             QString(), &error),
               "the first draft did not open")) {
        std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
        return failures;
    }
    SongSession *draftA = m_active;
    if (!check(draftA && draftA->isDraft() && draftA->doc.label() == labelA,
               "the draft is not the active tab"))
        return failures;
    check(treeFingerprint(root) == pristine, "opening a draft wrote into the project");
    check(draftA->isDirty() && !draftA->doc.isDirty(),
          "a fresh draft is not dirty through its draft state alone");
    check(m_tabs->tabText(m_tabs->indexOf(draftA->view)).endsWith(QLatin1Char('*')),
          "the draft's tab title has no asterisk");
    check(draftA->songId == -1, "the draft has a song ID");
    check(draftA->doc.midPath() == midiDir + labelA + QStringLiteral(".mid"),
          "the draft does not target the project's .mid path");
    check(draftA->doc.engineTrackCount() == 1 && draftA->doc.notesForTrack(0).size() == 1,
          "the draft's document does not hold the imported song");
    check(draftA->voicegroup && m_audio.timeline() == draftA->timeline.get(),
          "the engine is not playing the draft");
    check(!persistedLabels().contains(labelA), "the draft was persisted as an open tab");

    // 2. Edits stay in memory.
    draftA->doc.addNote(0, 48, 64, 24, 100);
    check(draftA->doc.isDirty(), "an edit did not reach the draft's undo stack");
    check(treeFingerprint(root) == pristine, "editing a draft wrote into the project");

    // 3. Discard through the close prompt: the tab goes and nothing is left
    // behind, view sidecar included.
    answerNextPrompt(this, QMessageBox::Discard);
    closeTab(m_tabs->indexOf(draftA->view));
    draftA = nullptr;
    check(!sessionForLabel(labelA), "discarding did not close the draft");
    check(treeFingerprint(root) == pristine, "discarding a draft wrote into the project");
    check(!QFile::exists(root + QStringLiteral("/.porydaw/") + labelA + QStringLiteral(".json")),
          "discarding a draft wrote its view sidecar");
    check(!QFile::exists(midiDir + labelA + QStringLiteral(".mid")),
          "discarding a draft wrote its .mid");
    check(!persistedLabels().contains(labelA), "the discarded draft was persisted");

    // 4. A second draft, edited and saved: committed in place.
    const QString labelB = QStringLiteral("mus_draftcheck_b");
    const QString constantB = QStringLiteral("MUS_DRAFTCHECK_B");
    if (!check(openDraftSong(draftSmf(), labelB, constantB, player, cfg, QString(), &error),
               "the second draft did not open")) {
        std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
        return failures;
    }
    SongSession *draftB = m_active;
    if (!check(draftB && draftB->isDraft() && draftB->doc.label() == labelB,
               "the second draft is not the active tab"))
        return failures;
    SongView *viewB = draftB->view;
    draftB->doc.addNote(0, 48, 67, 24, 100);
    check(treeFingerprint(root) == pristine, "the second draft wrote before saving");

    check(saveSession(*draftB), "saving the draft failed");
    check(sessionForLabel(labelB) == draftB && m_active == draftB && draftB->view == viewB,
          "the commit did not keep the session and its view");
    check(!draftB->isDraft() && !draftB->isDirty(), "the committed song is still unsaved");
    check(draftB->doc.undoStack()->isClean() && draftB->doc.undoStack()->canUndo(),
          "the commit dropped the undo history");
    check(!m_tabs->tabText(m_tabs->indexOf(viewB)).endsWith(QLatin1Char('*')),
          "the committed song's tab title kept its asterisk");
    check(treeFingerprint(root) != pristine, "the commit wrote nothing");

    SmfFile onDisk;
    check(SmfFile::readFile(midiDir + labelB + QStringLiteral(".mid"), &onDisk, &error) &&
              std::equal(onDisk.tracks.begin(), onDisk.tracks.end(),
                         draftB->doc.smf().tracks.begin(), draftB->doc.smf().tracks.end(),
                         [](const SmfTrack &a, const SmfTrack &b) {
                             return a.events == b.events && a.endTick == b.endTick;
                         }),
          "the committed .mid is not the document");
    const SongInfo *committed = nullptr;
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == labelB)
            committed = &song;
    }
    if (check(committed != nullptr, "the committed song is not in the project")) {
        check(committed->registered && committed->constant == constantB,
              "the committed song is not registered");
        check(committed->hasCfg && committed->cfg.voicegroupArg == cfg.voicegroupArg,
              "the committed song's flags were not written");
        check(draftB->songId >= 0 && draftB->songId == committed->id,
              "the committed session did not pick up its song ID");
    }
    check(persistedLabels().contains(labelB), "the committed song was not persisted as open");

    // The pre-save edit is still undoable, and undo dirties the song again.
    const size_t notesSaved = draftB->doc.notesForTrack(0).size();
    draftB->doc.undoStack()->undo();
    check(draftB->doc.notesForTrack(0).size() + 1 == notesSaved && draftB->isDirty(),
          "undoing the pre-save edit after the commit did not work");
    draftB->doc.undoStack()->redo();
    check(!draftB->isDirty(), "redo did not return to the saved state");

    // 5. A draft replaced in place by a browser load: answering Save
    // commits it — its project reload renews the song list under the
    // load — and the requested song lands in the same tab with its ID.
    const QString labelC = QStringLiteral("mus_draftcheck_c");
    if (!check(openDraftSong(draftSmf(), labelC, QStringLiteral("MUS_DRAFTCHECK_C"), player, cfg,
                             QString(), &error),
               "the third draft did not open")) {
        std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
        return failures;
    }
    SongSession *draftC = m_active;
    const int tabsBefore = m_tabs->count();
    QString existing;
    for (const SongInfo &song : m_project.songs()) {
        if (song.isPlayable() && song.registered && !sessionForLabel(song.label)) {
            existing = song.label;
            break;
        }
    }
    answerNextPrompt(this, QMessageBox::Save);
    loadSongByLabel(existing);
    check(m_active == draftC && m_tabs->count() == tabsBefore && !draftC->isDraft() &&
              draftC->doc.label() == existing,
          "loading a song over a draft did not replace it in place");
    bool committedC = false;
    for (const SongInfo &song : m_project.songs()) {
        committedC = committedC || (song.label == labelC && song.registered);
        if (song.label == existing)
            check(draftC->songId == song.id, "the song loaded over a draft has a stale ID");
    }
    check(committedC, "answering Save over a draft did not commit it");

    if (failures == 0)
        std::printf("draftcheck: PASS\n");
    return failures;
}

int runDraftCheck(const QString &projectRoot)
{
    // Redirected settings: the user's real session is never touched.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "draftcheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    MainWindow window;
    return window.runDraftCheck(projectRoot) == 0 ? 0 : 1;
}
