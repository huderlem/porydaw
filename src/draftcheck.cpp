#include <QAbstractButton>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <QWizardPage>
#include <algorithm>
#include <cstdio>
#include <functional>

#include "mainwindow.h"
#include "project/songregistry.h"
#include "ui/newsongwizard.h"
#ifdef PORYDAW_SCRIPTING
#include "scripting/scripthost.h"
#endif

// --draftcheck <projectRoot>: draft songs (docs/draft-songs/PLAN.md). Import
// MIDI / New Song open a draft tab that writes nothing into the project:
// opening, editing and discarding one leaves every file outside .porydaw/
// untouched, and no view sidecar appears. Saving a draft commits it in
// place — .mid, flags, registration — keeping its session, view and undo
// history. Names are reserved: the wizard rejects an open draft's label and
// constant, and a name taken behind a draft's back (its .mid created) is
// caught at Save, which writes nothing until the Rename dialog picks a free
// name (or only a free constant). A commit that failed after writing its
// .mid retries under the same name, even after a project reload lists that
// .mid as an unregistered song. QSettings is redirected into a temp dir; the commit writes into the
// project — run against a scratch copy.

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
// surprise dialog fails the section instead of hanging the harness —
// unless onDialog registered a handler for its title, which then runs once
// and must close it. The guard's destructor stops the poll, so nothing
// fires into a later section; answered() says whether the expected prompt
// ever appeared, handled(title) whether a registered dialog did.
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
    void onDialog(const QString &title, std::function<void(QDialog *)> action)
    {
        m_handlers.insert(title, std::move(action));
    }
    bool handled(const QString &title) const { return m_handled.contains(title); }

  private:
    void poll()
    {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        auto *dialog = qobject_cast<QDialog *>(modal);
        const QString title = modal->windowTitle();
        if (dialog && m_handlers.contains(title) && !m_handled.contains(title)) {
            m_handled.insert(title);
            m_handlers.value(title)(dialog);
            return;
        }
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
        if (dialog)
            dialog->reject();
        else
            modal->close();
    }

    QMessageBox::StandardButton m_button;
    Fail m_fail;
    QTimer m_timer;
    bool m_answered = false;
    QHash<QString, std::function<void(QDialog *)>> m_handlers;
    QSet<QString> m_handled;
};

// The New Song wizard's and the Rename dialog's label field.
QLineEdit *nameField(QWidget *parent)
{
    for (QLineEdit *edit : parent->findChildren<QLineEdit *>()) {
        if (edit->placeholderText() == QStringLiteral("mus_my_song"))
            return edit;
    }
    return nullptr;
}

// The field after the label: the constant.
QLineEdit *constantField(QWidget *parent)
{
    const QList<QLineEdit *> edits = parent->findChildren<QLineEdit *>();
    QLineEdit *name = nameField(parent);
    for (QLineEdit *edit : edits) {
        if (edit != name)
            return edit;
    }
    return nullptr;
}

// The text of the red conflict hint under a name field, if any label shows one.
QString nameHint(QWidget *parent)
{
    for (const QLabel *label : parent->findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("already")))
            return label->text();
    }
    return QString();
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

    const QString songsHPath = root + QStringLiteral("/include/constants/songs.h");
    // Appends a #define to songs.h, as a git pull or another tool might.
    const auto plantDefine = [&songsHPath](const QString &name, const QString &value) {
        QFile file(songsHPath);
        if (!file.open(QIODevice::Append))
            return false;
        const QByteArray line = QStringLiteral("#define %1 %2\n").arg(name, value).toUtf8();
        return file.write(line) == line.size();
    };
    // The first songs.h define no listed song uses as its constant: an alias
    // (or a hex value) the project's song list never picks up. Plants one
    // when the project has none.
    const auto unlistedSongsHDefine = [&]() -> QString {
        static const QRegularExpression defineRe(
            QStringLiteral(R"(^\s*#define\s+((?:MUS|SE|PH)_\w+)\b)"));
        QSet<QString> listed;
        for (const SongInfo &song : m_project.songs())
            listed.insert(song.constant);
        QFile file(songsHPath);
        if (file.open(QIODevice::ReadOnly)) {
            while (!file.atEnd()) {
                const QRegularExpressionMatch m =
                    defineRe.match(QString::fromUtf8(file.readLine()));
                if (m.hasMatch() && !listed.contains(m.captured(1)))
                    return m.captured(1);
            }
        }
        const QString planted = QStringLiteral("MUS_DRAFTCHECK_ALIAS");
        return plantDefine(planted, QStringLiteral("MUS_DUMMY")) ? planted : QString();
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

    // 7. Name reservation: the wizard rejects an open draft's label and
    // constant with the messages it gives project collisions, and the
    // shared check reads the disk, not only the (possibly stale) song list.
    const QString labelE = QStringLiteral("mus_draftcheck_e");
    const QString constantE = QStringLiteral("MUS_DRAFTCHECK_E");
    if (!check(openDraftSong(draftSmf(), labelE, constantE, player, cfg, QString(), &error),
               "the fifth draft did not open")) {
        std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
        return failures;
    }
    SongSession *draftE = m_active;
    {
        const QStringList vgArgs = vgCatalog(root).groupArgs;
        const ReservedSongNames reserved = reservedSongNames();
        check(reserved.labels.contains(labelE) && reserved.constants.contains(constantE),
              "an open draft's label and constant are not reserved");
        NewSongWizard wizard(&m_project, vgArgs, reserved);
        QWizardPage *identity = wizard.page(0);
        QLineEdit *name = nameField(identity);
        QLineEdit *constant = constantField(identity);
        if (check(name && constant, "wizard: name or constant field not found")) {
            name->setText(labelE);
            check(!identity->isComplete(), "wizard: accepted an open draft's label");
            check(nameHint(identity) ==
                      QStringLiteral("A song named %1 already exists.").arg(labelE),
                  "wizard: a reserved label did not get the project-collision message");
            name->setText(QStringLiteral("mus_draftcheck_free"));
            check(identity->isComplete(), "wizard: a free label was rejected");
            constant->setText(constantE);
            // setText does not count as the user's edit; textEdited does.
            emit constant->textEdited(constantE);
            check(!identity->isComplete(), "wizard: accepted an open draft's constant");
            constant->setText(QStringLiteral("MUS_LITTLEROOT"));
            emit constant->textEdited(constant->text());
            check(!identity->isComplete(), "wizard: accepted a constant songs.h defines");
            // A define songs.h has but the song list doesn't (vanilla's
            // MUS_ROUTE118/MUS_NONE are hex, so the project never reads them
            // as songs): only a read of the file itself can reject it.
            const QString alias = unlistedSongsHDefine();
            if (check(!alias.isEmpty(), "no songs.h define outside the song list to test with")) {
                constant->setText(alias);
                emit constant->textEdited(alias);
                check(!identity->isComplete(),
                      "wizard: accepted a songs.h define the song list doesn't know");
            }
        }
        // Without the reservation the same label is free: the rejection
        // above came from the draft, not from the disk.
        NewSongWizard unreserved(&m_project, vgArgs);
        if (QLineEdit *freeName = nameField(unreserved.page(0))) {
            freeName->setText(labelE);
            check(unreserved.page(0)->isComplete(),
                  "wizard: an unreserved draft label was rejected");
        }

        // The disk beats a stale song list: song_table.inc and songs.h
        // still name a vanilla song absent from the list passed in.
        const SongNameConflicts stale =
            SongRegistry::checkNewSongNames(root, {}, {}, QStringLiteral("mus_littleroot"),
                                            QStringLiteral("MUS_LITTLEROOT"), QString());
        check(!stale.label.isEmpty() && !stale.mid.isEmpty() && !stale.constant.isEmpty(),
              "checkNewSongNames trusted the song list over the disk");
        QString existingVg;
        for (const QString &arg : vgArgs) {
            const QString base = arg.startsWith(QLatin1Char('_')) ? arg.mid(1) : arg;
            if (QFile::exists(root + QStringLiteral("/sound/voicegroups/%1.inc").arg(base))) {
                existingVg = base;
                break;
            }
        }
        check(!existingVg.isEmpty() &&
                  !SongRegistry::checkNewSongNames(root, {}, {}, QString(), QString(), existingVg)
                       .voicegroup.isEmpty(),
              "checkNewSongNames missed an existing voicegroup file");
        ReservedSongNames vgReserved;
        vgReserved.voicegroups.append(QStringLiteral("mus_draftcheck_vg"));
        check(!SongRegistry::checkNewSongNames(root, {}, vgReserved, QString(), QString(),
                                               QStringLiteral("mus_draftcheck_vg"))
                   .voicegroup.isEmpty(),
              "checkNewSongNames missed a reserved voicegroup");
        check(SongRegistry::checkNewSongNames(
                  root, m_project.songs(), reserved, QStringLiteral("mus_draftcheck_free"),
                  QStringLiteral("MUS_DRAFTCHECK_FREE"), QStringLiteral("mus_draftcheck_free"))
                  .isEmpty(),
              "checkNewSongNames rejected free names");
    }

    // 8. A name taken behind the draft's back: its .mid appears on disk. Save
    // writes nothing, opens the Rename dialog (OK disabled on the taken
    // name), and commits under the new name once it is accepted; the
    // planted file is never touched.
    const QString plantedE = midiDir + labelE + QStringLiteral(".mid");
    const QByteArray plantedBytes("not a song, planted by draftcheck\n");
    {
        QFile planted(plantedE);
        if (!check(planted.open(QIODevice::WriteOnly) &&
                       planted.write(plantedBytes) == plantedBytes.size(),
                   "could not plant a file under the draft's .mid"))
            return failures;
    }
    const QDateTime plantedTime = QFileInfo(plantedE).lastModified();
    const QString labelE2 = QStringLiteral("mus_draftcheck_e2");
#ifdef PORYDAW_SCRIPTING
    // A plugin watching the song: after the rename it must hear the new
    // name (song.activated) and keep hearing edits (song.changed).
    m_scriptHost->evalConsole(
        QStringLiteral("var dcChanged = 0; var dcActivated = '';"
                       "porydaw.song.on('changed', function () { dcChanged++; });"
                       "porydaw.song.on('activated', function (e) {"
                       "  dcActivated = e ? e.label : ''; });"));
#endif
    {
        bool okDisabledFirst = false;
        bool okEnabledAfter = false;
        PromptAnswerer rename(QMessageBox::Cancel, modalFail);
        rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QPushButton *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            QLineEdit *name = nameField(dialog);
            if (!ok || !name) {
                dialog->reject();
                return;
            }
            okDisabledFirst = !ok->isEnabled() && name->text() == labelE;
            name->setText(labelE2);
            okEnabledAfter = ok->isEnabled();
            ok->click();
        });
        check(saveSession(*draftE), "saving a draft through the Rename dialog failed");
        check(rename.handled(QStringLiteral("Rename Song")),
              "a draft whose .mid appeared did not open the Rename dialog");
        check(okDisabledFirst, "the Rename dialog accepted the taken name");
        check(okEnabledAfter, "the Rename dialog rejected a free name");
    }
    check(m_active == draftE && !draftE->isDraft() && !draftE->isDirty(),
          "the renamed draft was not committed in place");
    check(draftE->doc.label() == labelE2 &&
              draftE->doc.midPath() == midiDir + labelE2 + QStringLiteral(".mid"),
          "the rename did not move the document's label and .mid path");
    check(m_tabs->tabText(m_tabs->indexOf(draftE->view)) == labelE2 &&
              m_tabs->tabToolTip(m_tabs->indexOf(draftE->view)) == draftE->doc.midPath(),
          "the tab does not show the new name");
    {
        QFile planted(plantedE);
        check(planted.open(QIODevice::ReadOnly) && planted.readAll() == plantedBytes &&
                  QFileInfo(plantedE).lastModified() == plantedTime,
              "the commit touched the planted file");
    }
    const SongInfo *renamed = nullptr;
    bool oldNameKnown = false;
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == labelE2)
            renamed = &song;
        // The planted .mid itself shows up as an unregistered song.
        oldNameKnown = oldNameKnown || (song.label == labelE && song.registered);
    }
    if (check(renamed != nullptr, "the renamed song is not in the project")) {
        check(renamed->registered && renamed->constant == QStringLiteral("MUS_DRAFTCHECK_E2") &&
                  renamed->hasCfg && draftE->songId == renamed->id,
              "the renamed song is not registered under its new names");
    }
    check(!oldNameKnown, "the old name was registered");
    const RegistrationStatus oldNames = SongRegistry::checkRegistration(root, labelE, constantE);
    check(!oldNames.inSongTable && !oldNames.inSongsH && !oldNames.inCharmap,
          "the old names reached the registration files");
    check(persistedLabels().contains(labelE2) && !persistedLabels().contains(labelE),
          "the renamed song was not persisted under its new name");
    // What activateSession set by label follows the rename.
    check(m_songLabel->text().trimmed() == labelE2,
          "the transport's song label kept the old name after the rename");
    check(windowTitle().contains(labelE2), "the window title kept the old name after the rename");
#ifdef PORYDAW_SCRIPTING
    check(m_scriptHost->evalConsole(QStringLiteral("dcActivated")) == labelE2,
          "the rename did not fire song.activated with the new label");
    m_scriptHost->evalConsole(QStringLiteral("dcChanged = 0;"));
    draftE->doc.addNote(0, 48, 62, 24, 100);
    QApplication::processEvents(); // song.changed is delivered after the turn
    check(m_scriptHost->evalConsole(QStringLiteral("dcChanged")).toInt() >= 1,
          "song.changed stopped firing for the renamed song");
    draftE->doc.undoStack()->undo();
    QApplication::processEvents();
#endif

    // 9. Cancel on the Rename dialog: nothing is written, the tab stays a
    // dirty draft, and the Save counts as failed — without a second box.
    const QString labelF = QStringLiteral("mus_draftcheck_f");
    if (!check(openDraftSong(draftSmf(), labelF, QStringLiteral("MUS_DRAFTCHECK_F"), player, cfg,
                             QString(), &error),
               "the sixth draft did not open")) {
        std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
        return failures;
    }
    SongSession *draftF = m_active;
    {
        QFile planted(midiDir + labelF + QStringLiteral(".mid"));
        check(planted.open(QIODevice::WriteOnly) && planted.write(plantedBytes) > 0,
              "could not plant a file under the sixth draft's .mid");
    }
    const QByteArray beforeF = treeFingerprint(root);
    {
        PromptAnswerer rename(QMessageBox::Cancel, modalFail);
        rename.onDialog(QStringLiteral("Rename Song"), [](QDialog *dialog) { dialog->reject(); });
        check(!saveSession(*draftF), "cancelling the Rename dialog still saved");
        check(rename.handled(QStringLiteral("Rename Song")),
              "the sixth draft did not open the Rename dialog");
    }
    check(treeFingerprint(root) == beforeF, "cancelling the Rename dialog wrote into the project");
    check(m_active == draftF && draftF->isDraft() && draftF->isDirty() &&
              draftF->doc.label() == labelF && draftF->songId == -1,
          "cancelling the Rename dialog changed the draft");
    check(m_tabs->tabText(m_tabs->indexOf(draftF->view)) == labelF + QLatin1Char('*'),
          "the cancelled draft's tab title changed");
    check(!persistedLabels().contains(labelF), "the cancelled draft was persisted");

    // Looks up a song in the (reloaded) project list.
    const auto songNamed = [this](const QString &label) -> const SongInfo * {
        for (const SongInfo &song : m_project.songs()) {
            if (song.label == label)
                return &song;
        }
        return nullptr;
    };
    // The Rename dialog, kept at its label: OK must start disabled (the
    // constant is taken), and changing only the constant must enable it.
    const auto keepLabelNewConstant = [&](PromptAnswerer &answerer, const QString &label,
                                          const QString &newConstant, bool *okDisabledFirst,
                                          bool *okEnabledAfter) {
        answerer.onDialog(QStringLiteral("Rename Song"), [=](QDialog *dialog) {
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QPushButton *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            QLineEdit *name = nameField(dialog);
            QLineEdit *constant = constantField(dialog);
            if (!ok || !name || !constant) {
                dialog->reject();
                return;
            }
            *okDisabledFirst = !ok->isEnabled() && name->text() == label;
            constant->setText(newConstant);
            emit constant->textEdited(newConstant);
            *okEnabledAfter = ok->isEnabled() && name->text() == label;
            ok->click();
        });
    };

    // 10. Only the constant was taken behind the draft's back (songs.h gained
    // an alias define): the Rename dialog lets the label stay and changes
    // just the constant, and the commit lands under the original label.
    {
        const QString labelG = QStringLiteral("mus_draftcheck_g");
        const QString constantG = QStringLiteral("MUS_DRAFTCHECK_G");
        const QString constantG2 = QStringLiteral("MUS_DRAFTCHECK_G2");
        if (!check(openDraftSong(draftSmf(), labelG, constantG, player, cfg, QString(), &error),
                   "the seventh draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *draftG = m_active;
        check(plantDefine(constantG, QStringLiteral("MUS_DUMMY")),
              "could not plant a define in songs.h");
        bool okDisabledFirst = false;
        bool okEnabledAfter = false;
        {
            PromptAnswerer rename(QMessageBox::Cancel, modalFail);
            keepLabelNewConstant(rename, labelG, constantG2, &okDisabledFirst, &okEnabledAfter);
            check(saveSession(*draftG), "saving a draft whose constant was taken failed");
            check(rename.handled(QStringLiteral("Rename Song")),
                  "a draft whose constant was taken did not open the Rename dialog");
        }
        check(okDisabledFirst, "the Rename dialog accepted the taken constant");
        check(okEnabledAfter, "the Rename dialog would not keep the label with a new constant");
        const SongInfo *song = songNamed(labelG);
        check(!draftG->isDraft() && draftG->doc.label() == labelG && song && song->registered &&
                  song->constant == constantG2 && draftG->songId == song->id,
              "the constant-only rename did not commit under the original label");
    }

    // 11-12. A commit that failed after writing the .mid (its flags write
    // failed), then a project reload, which lists that .mid as an
    // unregistered song under the draft's label. The .mid is the draft's
    // own: a retry commits under the same name (11), and a Rename dialog
    // opened for another reason still lets the label stay (12).
    const QString cfgPath = midiDir + QStringLiteral("midi.cfg");
    const QFileDevice::Permissions cfgPerms = QFile::permissions(cfgPath);
    // Makes midi.cfg unwritable; false (restored) when the file system or
    // the user (root) ignores that — the caller then skips with a note.
    const auto lockCfg = [&]() {
        if (!QFile::setPermissions(cfgPath, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                                QFileDevice::ReadGroup | QFileDevice::ReadOther))
            return false;
        QFile probe(cfgPath);
        if (probe.open(QIODevice::WriteOnly | QIODevice::Append)) {
            probe.close();
            QFile::setPermissions(cfgPath, cfgPerms);
            return false;
        }
        return true;
    };
    // Opens a draft, fails its first Save after the .mid lands, reloads the
    // project. False (with the failure recorded) when that didn't happen.
    const auto partialCommit = [&](const QString &label, const QString &constant,
                                   SongSession **out) {
        if (!check(openDraftSong(draftSmf(), label, constant, player, cfg, QString(), &error),
                   "a partial-commit draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return false;
        }
        SongSession *draft = m_active;
        *out = draft;
        const QString midPath = midiDir + label + QStringLiteral(".mid");
        const QByteArray cfgBefore = [&] {
            QFile f(cfgPath);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        {
            PromptAnswerer failBox(QMessageBox::Cancel, modalFail);
            failBox.onDialog(QStringLiteral("Save Song"),
                             [](QDialog *dialog) { dialog->reject(); });
            check(!saveSession(*draft), "saving with an unwritable midi.cfg succeeded");
            check(failBox.handled(QStringLiteral("Save Song")),
                  "the failed flags write did not report its error");
        }
        QFile::setPermissions(cfgPath, cfgPerms);
        QFile cfgFile(cfgPath);
        check(cfgFile.open(QIODevice::ReadOnly) && cfgFile.readAll() == cfgBefore,
              "the failed commit changed midi.cfg");
        if (!check(QFile::exists(midPath) && draft->isDraft() && draft->draft &&
                       draft->draft->wroteMidPath == midPath,
                   "the failed commit did not leave the draft's own .mid behind"))
            return false;
        QString reloadError;
        check(reloadProject(&reloadError), "reloading the project failed");
        const SongInfo *stray = songNamed(label);
        // The reviewed bug's precondition: the reload lists the draft's own
        // .mid as an unregistered song under its label and constant.
        check(stray && !stray->registered && stray->constant == constant,
              "the reload did not list the draft's own .mid as an unregistered song");
        const SongNameConflicts unwaived = SongRegistry::checkNewSongNames(
            root, m_project.songs(), reservedSongNames(draft), label, constant, QString());
        check(!unwaived.label.isEmpty() && !unwaived.mid.isEmpty() && !unwaived.constant.isEmpty(),
              "without the waiver the draft's own .mid did not read as taken");
        check(SongRegistry::checkNewSongNames(root, m_project.songs(), reservedSongNames(draft),
                                              label, constant, QString(), midPath)
                  .isEmpty(),
              "the waiver did not clear the draft's own .mid and its unregistered entry");
        check(draft->isDraft() && draft->songId == -1, "the reload resolved the draft's song ID");
        return true;
    };

    if (!lockCfg()) {
        std::printf("draftcheck: note: midi.cfg stays writable after chmod (root or a file "
                    "system that ignores permissions); partial-commit sections 11-12 skipped\n");
    } else {
        // 11. Retry with no changes: no Rename dialog, the original names.
        const QString labelH = QStringLiteral("mus_draftcheck_h");
        const QString constantH = QStringLiteral("MUS_DRAFTCHECK_H");
        SongSession *draftH = nullptr;
        if (partialCommit(labelH, constantH, &draftH)) {
            {
                PromptAnswerer noPrompt(QMessageBox::Cancel, modalFail);
                check(saveSession(*draftH), "retrying a partially committed draft failed");
            }
            const SongInfo *song = songNamed(labelH);
            check(!draftH->isDraft() && !draftH->isDirty() && draftH->doc.label() == labelH &&
                      song && song->registered && song->constant == constantH && song->hasCfg &&
                      draftH->songId == song->id,
                  "the retry did not commit under the original names");
            check(persistedLabels().contains(labelH), "the retried commit was not persisted");
        }

        // 12. The same, but songs.h has meanwhile taken the constant: the
        // Rename dialog opens for the constant alone and keeps the label
        // allowed — the draft's own .mid and its unregistered entry don't
        // count against it.
        const QString labelI = QStringLiteral("mus_draftcheck_i");
        const QString constantI = QStringLiteral("MUS_DRAFTCHECK_I");
        const QString constantI2 = QStringLiteral("MUS_DRAFTCHECK_I2");
        SongSession *draftI = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(labelI, constantI, &draftI)) {
            check(plantDefine(constantI, QStringLiteral("MUS_DUMMY")),
                  "could not plant a define in songs.h");
            bool okDisabledFirst = false;
            bool okEnabledAfter = false;
            {
                PromptAnswerer rename(QMessageBox::Cancel, modalFail);
                keepLabelNewConstant(rename, labelI, constantI2, &okDisabledFirst, &okEnabledAfter);
                check(saveSession(*draftI), "saving the partially committed draft failed");
                check(rename.handled(QStringLiteral("Rename Song")),
                      "a taken constant did not open the Rename dialog");
            }
            check(okDisabledFirst, "the Rename dialog accepted the taken constant (retry)");
            check(okEnabledAfter,
                  "the Rename dialog counted the draft's own .mid against keeping its label");
            const SongInfo *song = songNamed(labelI);
            check(!draftI->isDraft() && draftI->doc.label() == labelI && song && song->registered &&
                      song->constant == constantI2 && draftI->songId == song->id,
                  "the retry with a new constant did not commit under the original label");
            check(QFile::exists(midiDir + labelI + QStringLiteral(".mid")),
                  "keeping the label deleted the draft's own .mid");
        }
        QFile::setPermissions(cfgPath, cfgPerms);
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
