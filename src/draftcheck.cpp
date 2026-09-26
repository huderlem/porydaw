#include <QAbstractButton>
#include <QApplication>
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
#include <functional>

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

// Answers the close/save prompt ("Unsaved Changes") with button from inside
// its own exec() loop, for as long as the guard is in scope. A 10 ms poll
// on the active modal widget: any OTHER modal that shows meanwhile (an
// error box, a second prompt) is recorded as a failure and dismissed, so a
// surprise dialog fails the section instead of hanging the harness. The
// guard's destructor stops the poll, so nothing fires into a later
// section; answered() says whether the expected prompt ever appeared.
class PromptAnswerer
{
  public:
    using Fail = std::function<void(const QString &)>;

    PromptAnswerer(QMessageBox::StandardButton button, Fail fail)
        : m_button(button)
        , m_fail(std::move(fail))
    {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { poll(); });
        m_timer.start();
    }
    ~PromptAnswerer() { m_timer.stop(); }
    PromptAnswerer(const PromptAnswerer &) = delete;
    PromptAnswerer &operator=(const PromptAnswerer &) = delete;

    bool answered() const { return m_answered; }

  private:
    void poll()
    {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        auto *box = qobject_cast<QMessageBox *>(modal);
        if (!m_answered && box && box->windowTitle() == QStringLiteral("Unsaved Changes")) {
            if (QAbstractButton *b = box->button(m_button)) {
                m_answered = true;
                b->click();
                return;
            }
        }
        m_fail(
            QStringLiteral("unexpected modal \"%1\"%2")
                .arg(modal->windowTitle(), box ? QStringLiteral(": ") + box->text() : QString()));
        if (auto *dialog = qobject_cast<QDialog *>(modal))
            dialog->reject();
        else
            modal->close();
    }

    QMessageBox::StandardButton m_button;
    Fail m_fail;
    QTimer m_timer;
    bool m_answered = false;
};

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
    const PromptAnswerer::Fail modalFail = [&failures](const QString &what) {
        std::fprintf(stderr, "draftcheck: FAIL: %s\n", qUtf8Printable(what));
        failures++;
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

    // 2b. A second draft under the same label is refused (a stopgap until
    // the wizard reserves draft names, PLAN step 2): the two would commit
    // over one .mid. The first draft is left exactly as it was.
    {
        const int tabsBefore = m_tabs->count();
        const size_t notesBefore = draftA->doc.notesForTrack(0).size();
        QString dupError;
        {
            PromptAnswerer noPrompt(QMessageBox::Cancel, modalFail);
            check(!openDraftSong(draftSmf(), labelA, QStringLiteral("MUS_DRAFTCHECK_A"), player,
                                 cfg, QString(), &dupError),
                  "a second draft under an open draft's label opened");
            check(!noPrompt.answered(), "refusing a duplicate draft prompted");
        }
        check(!dupError.isEmpty(), "refusing a duplicate draft gave no error");
        check(m_tabs->count() == tabsBefore && m_active == draftA &&
                  sessionForLabel(labelA) == draftA,
              "refusing a duplicate draft changed the tabs");
        check(draftA->isDraft() && draftA->isDirty() &&
                  draftA->doc.notesForTrack(0).size() == notesBefore,
              "refusing a duplicate draft touched the first one");
        check(treeFingerprint(root) == pristine,
              "refusing a duplicate draft wrote into the project");
    }

    // 3. Discard through the close prompt: the tab goes and nothing is left
    // behind, view sidecar included.
    {
        PromptAnswerer prompt(QMessageBox::Discard, modalFail);
        closeTab(m_tabs->indexOf(draftA->view));
        check(prompt.answered(), "closing a draft did not prompt");
    }
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

    {
        // No prompt expected: the guard only fails and dismisses a surprise
        // box (e.g. a registration-failure warning).
        PromptAnswerer noPrompt(QMessageBox::Cancel, modalFail);
        check(saveSession(*draftB), "saving the draft failed");
    }
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
    {
        QString dupError;
        check(!openDraftSong(draftSmf(), labelB, constantB, player, cfg, QString(), &dupError) &&
                  m_active == draftB,
              "a draft opened under a committed song's open label");
    }

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
    {
        PromptAnswerer prompt(QMessageBox::Save, modalFail);
        loadSongByLabel(existing);
        check(prompt.answered(), "loading a song over a draft did not prompt");
    }
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

    // 6. A draft replaced in place by a browser load, answering Discard: the
    // tab becomes the requested song as an ordinary project song — not a
    // draft, its ID resolved, persisted, saved through the normal path —
    // and nothing of the discarded draft reaches the disk.
    {
        const QString labelD = QStringLiteral("mus_draftcheck_d");
        if (!check(openDraftSong(draftSmf(), labelD, QStringLiteral("MUS_DRAFTCHECK_D"), player,
                                 cfg, QString(), &error),
                   "the fourth draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *draftD = m_active;
        draftD->doc.addNote(0, 48, 72, 24, 100);
        const int tabsBefore = m_tabs->count();
        const int songsBefore = int(m_project.songs().size());
        const QByteArray beforeD = treeFingerprint(root);
        QString target;
        for (const SongInfo &song : m_project.songs()) {
            if (song.isPlayable() && song.registered && !sessionForLabel(song.label)) {
                target = song.label;
                break;
            }
        }
        {
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            loadSongByLabel(target);
            check(prompt.answered(), "loading a song over a draft did not prompt (Discard)");
        }
        check(m_active == draftD && m_tabs->count() == tabsBefore && draftD->doc.label() == target,
              "loading a song over a discarded draft did not replace it in place");
        check(!draftD->isDraft(), "the song loaded over a discarded draft is still a draft");
        check(!draftD->isDirty(), "the song loaded over a discarded draft is dirty");
        int targetId = -1;
        for (const SongInfo &song : m_project.songs()) {
            if (song.label == target)
                targetId = song.id;
        }
        check(targetId >= 0 && draftD->songId == targetId,
              "the song loaded over a discarded draft has no (or a stale) song ID");
        check(persistedLabels().contains(target),
              "the song loaded over a discarded draft was not persisted as open");
        check(!persistedLabels().contains(labelD), "the discarded draft was persisted");
        check(treeFingerprint(root) == beforeD,
              "discarding a replaced draft wrote into the project");
        check(!QFile::exists(midiDir + labelD + QStringLiteral(".mid")),
              "discarding a replaced draft wrote its .mid");
        check(
            !QFile::exists(root + QStringLiteral("/.porydaw/") + labelD + QStringLiteral(".json")),
            "discarding a replaced draft wrote its view sidecar");
        bool knowsD = false;
        for (const SongInfo &song : m_project.songs())
            knowsD = knowsD || song.label == labelD;
        check(!knowsD && int(m_project.songs().size()) == songsBefore,
              "the discarded draft reached the project's song list");

        // Its next save is an ordinary save: the .mid is rewritten from the
        // document, and the registration files are left alone.
        const QString songTable = root + QStringLiteral("/sound/song_table.inc");
        const QString songsH = root + QStringLiteral("/include/constants/songs.h");
        const auto stamp = [](const QString &path) {
            const QFileInfo info(path);
            return QStringLiteral("%1|%2")
                .arg(info.size())
                .arg(info.lastModified().toMSecsSinceEpoch());
        };
        const QString tableBefore = stamp(songTable);
        const QString headerBefore = stamp(songsH);
        const QString midPath = draftD->doc.midPath();
        const QString midBefore = stamp(midPath);
        draftD->doc.addNote(0, 48, 71, 24, 100);
        {
            PromptAnswerer noPrompt(QMessageBox::Cancel, modalFail);
            check(saveSession(*draftD), "saving the song loaded over a discarded draft failed");
        }
        check(!draftD->isDirty() && m_active == draftD && draftD->songId == targetId,
              "the ordinary save left the song unsaved or moved its ID");
        check(stamp(midPath) != midBefore, "the ordinary save did not write the .mid");
        SmfFile saved;
        check(SmfFile::readFile(midPath, &saved, &error) &&
                  std::equal(saved.tracks.begin(), saved.tracks.end(),
                             draftD->doc.smf().tracks.begin(), draftD->doc.smf().tracks.end(),
                             [](const SmfTrack &a, const SmfTrack &b) {
                                 return a.events == b.events && a.endTick == b.endTick;
                             }),
              "the ordinary save's .mid is not the document");
        check(stamp(songTable) == tableBefore && stamp(songsH) == headerBefore,
              "the ordinary save re-registered the song");
        check(int(m_project.songs().size()) == songsBefore,
              "the ordinary save changed the project's song list");
        check(!QFile::exists(midiDir + labelD + QStringLiteral(".mid")),
              "the ordinary save wrote the discarded draft's .mid");
    }

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
