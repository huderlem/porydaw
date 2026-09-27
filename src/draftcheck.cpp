#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMap>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <QWizardPage>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

#include "mainwindow.h"
#include "project/bundlearchive.h"
#include "project/bundleimport.h"
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
// .mid as an unregistered song. A draft's new voicegroup lives in its own
// .porydaw/drafts/ folder until the commit writes it, edits included — only
// if the cfg still names it — and a Rename renames it too; a symbol declared
// in another file counts as taken; project open sweeps unlocked draft
// folders. A draft tab shows it (PLAN step 4): a [draft] tab title, tooltip
// and window title, a banner whose Save to project button commits, and an
// "Add … to the project?" close prompt; all of it goes with the commit or a
// replace in place. Names are settled before an edited existing voicegroup
// is written, so a cancelled Rename writes nothing at all; the Rename
// dialog lists what a partial commit already wrote, the close prompt names
// an edited shared voicegroup, and a save started during another is a
// no-op. The rest of the app (PLAN step 5): Register Song stays disabled,
// Song Settings edits reach the commit's flags, Export WAV / Export Song
// Bundle (with the draft's hints) / Import Sample for a slot work on a
// draft, bundle import and New Voicegroup keep off its names; Discard of a
// partly committed draft (close, replace, project switch, quit) removes
// what that commit wrote — but keeps a .mid registered or flagged since, a
// voicegroup another tab took over, and an include line the hub already
// had — and a project switch's Add commits into the old project first. A
// draft's leftover browser entry focuses the draft; Register Song and
// Delete Song refuse its label. QSettings is
// redirected into a temp dir; the commit writes into the project — run
// against a scratch copy. PORYDAW_DRAFTCHECK_SHOTS=<dir> saves screenshots.

namespace {

// One digest over every file outside .porydaw/: its relative path, size and
// modification time. Metadata, not contents: any write (even of identical
// bytes) moves the mtime, and the tree is too big to read four times over.
// byContent lists files (relative paths) digested by their bytes instead:
// ones a rollback rewrites back to what they held (an include line added,
// then removed again).
QMap<QString, QString> treeListing(const QString &root, const QStringList &byContent = {})
{
    QMap<QString, QString> entries;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        const QString rel = info.filePath().mid(root.size() + 1);
        if (rel.startsWith(QLatin1String(".porydaw/")))
            continue;
        if (byContent.contains(rel)) {
            QFile file(info.filePath());
            entries.insert(
                rel, QString::fromLatin1(QCryptographicHash::hash(file.open(QIODevice::ReadOnly)
                                                                      ? file.readAll()
                                                                      : QByteArray(),
                                                                  QCryptographicHash::Sha1)
                                             .toHex()));
            continue;
        }
        entries.insert(
            rel,
            QStringLiteral("%1|%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch()));
    }
    return entries;
}

QByteArray treeFingerprint(const QString &root, const QStringList &byContent = {})
{
    const QMap<QString, QString> listing = treeListing(root, byContent);
    QStringList entries;
    for (auto it = listing.constBegin(); it != listing.constEnd(); ++it)
        entries << it.key() + QLatin1Char('|') + it.value();
    return QCryptographicHash::hash(entries.join(QLatin1Char('\n')).toUtf8(),
                                    QCryptographicHash::Sha1);
}

// The relative paths added, removed or changed between two listings.
QStringList treeChanges(const QMap<QString, QString> &before, const QMap<QString, QString> &after)
{
    QStringList changed;
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        if (after.value(it.key()) != it.value())
            changed << it.key();
    }
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (!before.contains(it.key()))
            changed << it.key();
    }
    changed.sort();
    return changed;
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

// Answers the close/save prompt ("Unsaved Changes", or "Unsaved Draft" for a
// draft, whose Save button reads "Add to Project") with button from inside
// its own exec() loop, for as long as the guard is in scope. A 10 ms poll
// on the active modal widget: any OTHER modal that shows meanwhile (an
// error box, a second prompt) is recorded as a failure and dismissed, so a
// surprise dialog fails the section instead of hanging the harness —
// unless onDialog registered a handler for its title, which then runs (once,
// or `times` times for a title two dialogs share) and must close it. The guard's destructor stops the poll, so nothing
// fires into a later section; answered() says whether the expected prompt
// ever appeared, handled(title) whether a registered dialog did, and
// inspect(fn) sees the prompt (still open) before it is answered.
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
    void inspect(std::function<void(QMessageBox *)> fn) { m_inspect = std::move(fn); }
    void onDialog(const QString &title, std::function<void(QDialog *)> action, int times = 1)
    {
        m_handlers.insert(title, std::move(action));
        m_times.insert(title, times);
    }
    bool handled(const QString &title) const { return m_handled.value(title) > 0; }

  private:
    void poll()
    {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        auto *dialog = qobject_cast<QDialog *>(modal);
        const QString title = modal->windowTitle();
        if (dialog && m_handlers.contains(title) && m_handled.value(title) < m_times.value(title)) {
            m_handled[title]++;
            m_handlers.value(title)(dialog);
            return;
        }
        // A progress dialog (Export WAV's render) closes on its own.
        if (qobject_cast<QProgressDialog *>(modal))
            return;
        auto *box = qobject_cast<QMessageBox *>(modal);
        if (!m_answered && box &&
            (box->windowTitle() == QStringLiteral("Unsaved Changes") ||
             box->windowTitle() == QStringLiteral("Unsaved Draft"))) {
            if (QAbstractButton *b = box->button(m_button)) {
                m_answered = true;
                if (m_inspect)
                    m_inspect(box);
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
    std::function<void(QMessageBox *)> m_inspect;
    QHash<QString, std::function<void(QDialog *)>> m_handlers;
    QHash<QString, int> m_times;
    QHash<QString, int> m_handled;
};

// A PromptAnswerer for code that must not prompt at all: a close prompt
// that shows is answered Cancel and, when the guard goes out of scope,
// recorded as a failure naming the guard's line (any other modal already
// fails through PromptAnswerer).
class NoPromptGuard
{
  public:
    NoPromptGuard(PromptAnswerer::Fail fail, int line)
        : m_answerer(QMessageBox::Cancel, fail)
        , m_fail(std::move(fail))
        , m_line(line)
    {}
    ~NoPromptGuard()
    {
        if (m_answerer.answered())
            m_fail(QStringLiteral("unexpected close prompt (draftcheck.cpp:%1)").arg(m_line));
    }
    NoPromptGuard(const NoPromptGuard &) = delete;
    NoPromptGuard &operator=(const NoPromptGuard &) = delete;

  private:
    PromptAnswerer m_answerer;
    PromptAnswerer::Fail m_fail;
    int m_line;
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

    // The draft close prompt's informative text: plain, or naming an edited
    // existing (shared) voicegroup.
    const QString plainDraftInfo =
        QStringLiteral("It isn't in the project yet. Discarding it closes the song for good.");
    const auto vgDirtyDraftInfo = [](const QString &voicegroup) {
        return QStringLiteral("It isn't in the project yet, but it has edits to voicegroup %1, "
                              "which other songs may use. Add to Project saves those too; "
                              "Discard drops them.")
            .arg(voicegroup);
    };
    // The Rename dialog's explanation (the label naming the conflicts).
    const auto renameExplanation = [](QDialog *dialog) {
        for (QLabel *label : dialog->findChildren<QLabel *>()) {
            if (label->text().contains(QStringLiteral("can't be added to the project")))
                return label->text();
        }
        return QString();
    };
    const QString notWrittenYet = QStringLiteral("The song hasn't been written yet.");

    // The draft tab's banner and its Save to project button (PLAN step 4),
    // by object name; null when the view has none.
    const auto bannerOf = [](const SongSession *session) {
        return session->view->findChild<QWidget *>(QStringLiteral("draftBanner"));
    };
    const auto saveButtonOf = [](const SongSession *session) {
        return session->view->findChild<QPushButton *>(QStringLiteral("draftSaveButton"));
    };
    const auto tabTextOf = [this](const SongSession *session) {
        return m_tabs->tabText(m_tabs->indexOf(session->view));
    };
    const auto tabToolTipOf = [this](const SongSession *session) {
        return m_tabs->tabToolTip(m_tabs->indexOf(session->view));
    };
    // Visual review: PORYDAW_DRAFTCHECK_SHOTS=<dir> saves the window with a
    // draft tab up (draft-tab.png) and the draft close prompt
    // (draft-close-prompt.png).
    const QString shotsDir = qEnvironmentVariable("PORYDAW_DRAFTCHECK_SHOTS");

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
    check(tabTextOf(draftA) == QStringLiteral("[draft] %1*").arg(labelA),
          "the draft's tab title is not marked as a draft (with an asterisk)");
    check(tabToolTipOf(draftA).startsWith(QStringLiteral("Draft: not in the project yet.")) &&
              tabToolTipOf(draftA).contains(labelA + QStringLiteral(".mid")),
          "the draft's tab tooltip does not explain the draft");
    check(windowTitle().startsWith(QStringLiteral("[draft] %1[*]").arg(labelA)) &&
              isWindowModified(),
          "the window title does not show the draft");
    {
        // The banner: the plan's text under the label, and the button.
        QWidget *banner = bannerOf(draftA);
        auto *text =
            banner ? banner->findChild<QLabel *>(QStringLiteral("draftBannerText")) : nullptr;
        QPushButton *save = saveButtonOf(draftA);
        check(banner && banner == draftA->draftBanner, "the draft tab has no banner");
        check(text && text->text() == QStringLiteral("<i>%1</i> isn't in your project yet. Save "
                                                     "adds it; closing the tab discards it.")
                                          .arg(labelA),
              "the draft banner's text is not the plan's");
        check(save && save->text() == QStringLiteral("Save to project") && save->isEnabled() &&
                  save->focusPolicy() == Qt::NoFocus,
              "the draft banner has no (enabled, focusless) Save to project button");
    }
    if (!shotsDir.isEmpty()) {
        resize(1280, 800);
        show();
        QApplication::processEvents();
        check(grab().toImage().save(shotsDir + QStringLiteral("/draft-tab.png")),
              "could not save the draft tab screenshot");
        // Hidden again: the later sections run as they do without shots.
        hide();
        QApplication::processEvents();
    }
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
            NoPromptGuard noPrompt(modalFail, __LINE__);
            check(!openDraftSong(draftSmf(), labelA, QStringLiteral("MUS_DRAFTCHECK_A"), player,
                                 cfg, QString(), &dupError),
                  "a second draft under an open draft's label opened");
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
        bool promptOk = false;
        prompt.inspect([&](QMessageBox *box) {
            QAbstractButton *add = box->button(QMessageBox::Save);
            QAbstractButton *discard = box->button(QMessageBox::Discard);
            promptOk = box->windowTitle() == QStringLiteral("Unsaved Draft") &&
                       box->text() == QStringLiteral("Add %1 to the project?").arg(labelA) && add &&
                       add->text() == QStringLiteral("Add to Project") &&
                       box->defaultButton() == add && discard &&
                       box->buttonRole(discard) == QMessageBox::DestructiveRole &&
                       box->button(QMessageBox::Cancel) && box->buttons().size() == 3 &&
                       box->informativeText() == plainDraftInfo;
            if (!shotsDir.isEmpty())
                box->grab().toImage().save(shotsDir + QStringLiteral("/draft-close-prompt.png"));
        });
        closeTab(m_tabs->indexOf(draftA->view));
        check(prompt.answered(), "closing a draft did not prompt");
        check(promptOk, "the draft close prompt is not \"Add … to the project?\" with Add to "
                        "Project (default) / Discard (destructive) / Cancel and the plain "
                        "informative text");
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
        // Through the banner's Save to project button (queued: the commit
        // removes the button). No prompt expected: the guard only fails and
        // dismisses a surprise box (e.g. a registration-failure warning).
        NoPromptGuard noPrompt(modalFail, __LINE__);
        QPushButton *save = saveButtonOf(draftB);
        if (check(save != nullptr, "the second draft has no Save to project button")) {
            save->click();
            QElapsedTimer waited;
            waited.start();
            while (draftB->isDraft() && waited.elapsed() < 10000)
                QApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        check(!draftB->isDraft(), "Save to project did not commit the draft");
    }
    check(!bannerOf(draftB) && !draftB->draftBanner, "the commit left the draft banner behind");
    check(tabTextOf(draftB) == labelB && tabToolTipOf(draftB) == draftB->doc.midPath(),
          "the committed song's tab title or tooltip is still a draft's");
    check(windowTitle().startsWith(labelB + QStringLiteral("[*] — ")) && !isWindowModified(),
          "the committed song's window title is still a draft's");
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
        check(statusBar()->currentMessage() ==
                  QStringLiteral("Created and registered %1 as %2 (song ID %3)")
                      .arg(labelB, constantB)
                      .arg(committed->id),
              "the commit's status message is not the old write-through one");
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

    // Review B needs real geometry: a hidden window never lays its views out
    // (every splitter size reads zero). Shown only around those checks, in
    // either mode, at the screenshot size.
    const auto showWindow = [this](bool shown) {
        if (shown) {
            resize(1280, 800);
            show();
        } else {
            hide();
        }
        QApplication::processEvents();
    };
    // Review B: what a fresh load of label (in a tab of its own) shows,
    // after planting a view sidecar with a moved splitter and a vertical
    // scroll for it; the draft tab (active before) is active again after.
    // A song loaded over a draft must come up the same.
    const auto freshViewOf = [&](const QString &label) {
        SongSession *before = m_active;
        SongView::ViewState state;
        loadSongByLabel(label, /*newTab=*/true);
        if (!check(m_active && m_active != before && m_active->doc.label() == label,
                   "view reference: the song did not load in a new tab"))
            return state;
        state = m_active->view->viewState();
        // Edge cases for a restore into a view whose height changes after
        // it: the roll pane squeezed to its minimum (a shorter view's
        // shortfall must come from the lanes pane).
        if (state.splitterSizes.size() >= 2) {
            state.splitterSizes.last() += state.splitterSizes.first() - 1;
            state.splitterSizes.first() = 1;
        }
        // Scrolled to the bottom: the restore clamps to the roll's height.
        state.scrollY = 1e9;
        m_active->view->applyViewState(state);
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            closeTab(m_tabs->indexOf(m_active->view));
        }
        loadSongByLabel(label, /*newTab=*/true);
        if (!check(m_active && m_active != before && m_active->doc.label() == label,
                   "view reference: the song did not load in a new tab again"))
            return state;
        QApplication::processEvents();
        state = m_active->view->viewState();
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            closeTab(m_tabs->indexOf(m_active->view));
        }
        m_tabs->setCurrentWidget(before->view);
        // The draft tab laid out, banner included, as the user sees it.
        QApplication::processEvents();
        return state;
    };
    const auto sameView = [](const SongView::ViewState &a, const SongView::ViewState &b) {
        const bool same = a.splitterSizes == b.splitterSizes && a.scrollY == b.scrollY;
        if (!same) {
            QStringList sa, sb;
            for (int v : a.splitterSizes)
                sa << QString::number(v);
            for (int v : b.splitterSizes)
                sb << QString::number(v);
            std::fprintf(stderr, "draftcheck: view: splitter [%s] scrollY %g vs fresh [%s] %g\n",
                         qUtf8Printable(sa.join(',')), a.scrollY, qUtf8Printable(sb.join(',')),
                         b.scrollY);
        }
        return same;
    };

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
    check(bannerOf(draftC) != nullptr, "the third draft has no banner");
    const int tabsBefore = m_tabs->count();
    QString existing;
    for (const SongInfo &song : m_project.songs()) {
        if (song.isPlayable() && song.registered && !sessionForLabel(song.label)) {
            existing = song.label;
            break;
        }
    }
    showWindow(true);
    const SongView::ViewState freshC = freshViewOf(existing);
    check(m_active == draftC, "the view reference did not return to the third draft");
    {
        PromptAnswerer prompt(QMessageBox::Save, modalFail);
        loadSongByLabel(existing);
        check(prompt.answered(), "loading a song over a draft did not prompt");
    }
    QApplication::processEvents();
    check(sameView(draftC->view->viewState(), freshC),
          "a song loaded over a committed draft does not show its view state as a fresh load does");
    showWindow(false);
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
    check(!bannerOf(draftC) && !draftC->draftBanner && tabTextOf(draftC) == existing &&
              tabToolTipOf(draftC) == draftC->doc.midPath(),
          "a draft committed and replaced in place kept its banner or draft tab title");

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
        check(bannerOf(draftD) != nullptr, "the fourth draft has no banner");
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
        showWindow(true);
        const SongView::ViewState freshD = freshViewOf(target);
        check(m_active == draftD, "the view reference did not return to the fourth draft");
        {
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            loadSongByLabel(target);
            check(prompt.answered(), "loading a song over a draft did not prompt (Discard)");
        }
        QApplication::processEvents();
        check(sameView(draftD->view->viewState(), freshD),
              "a song loaded over a discarded draft does not show its view state as a fresh "
              "load does");
        showWindow(false);
        check(m_active == draftD && m_tabs->count() == tabsBefore && draftD->doc.label() == target,
              "loading a song over a discarded draft did not replace it in place");
        check(!draftD->isDraft(), "the song loaded over a discarded draft is still a draft");
        check(!draftD->isDirty(), "the song loaded over a discarded draft is dirty");
        check(!bannerOf(draftD) && !draftD->draftBanner && tabTextOf(draftD) == target &&
                  tabToolTipOf(draftD) == draftD->doc.midPath(),
              "a draft discarded and replaced in place kept its banner or draft tab title");
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
            NoPromptGuard noPrompt(modalFail, __LINE__);
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
    check(!bannerOf(draftE), "the renamed and committed draft kept its banner");
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
        QString explanation;
        rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
            explanation = renameExplanation(dialog);
            dialog->reject();
        });
        check(!saveSession(*draftF), "cancelling the Rename dialog still saved");
        check(rename.handled(QStringLiteral("Rename Song")),
              "the sixth draft did not open the Rename dialog");
        check(explanation.endsWith(notWrittenYet),
              "the Rename dialog of an unwritten draft does not say it hasn't been written");
    }
    // 9b. A second save while the Rename dialog is up (review E): two queued
    // banner clicks, the second dispatched inside the dialog's loop, and
    // Save Song (Ctrl+S) triggered from it, are no-ops — one dialog only (a
    // second would fail as an unexpected modal).
    {
        PromptAnswerer rename(QMessageBox::Cancel, modalFail);
        bool nestedRefused = false;
        rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
            // Not from the answerer's own timer slot: Qt doesn't re-enter a
            // timer, so a second dialog opened from here would never be
            // answered (the harness would hang instead of failing).
            QTimer::singleShot(0, dialog, [&, dialog] {
                m_saveAction->trigger();
                nestedRefused = !saveSession(*draftF);
                dialog->reject();
            });
        });
        QPushButton *save = saveButtonOf(draftF);
        if (check(save != nullptr, "the sixth draft has no Save to project button")) {
            save->click();
            save->click();
            QElapsedTimer waited;
            waited.start();
            while (!rename.handled(QStringLiteral("Rename Song")) && waited.elapsed() < 10000)
                QApplication::processEvents(QEventLoop::AllEvents, 10);
            // Both queued clicks delivered, and any second dialog polled.
            for (int i = 0; i < 5; i++)
                QApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(rename.handled(QStringLiteral("Rename Song")),
              "the banner's Save to project did not open the Rename dialog");
        check(nestedRefused, "a save started during the Rename dialog was not refused");
    }
    check(treeFingerprint(root) == beforeF, "cancelling the Rename dialog wrote into the project");
    check(m_active == draftF && draftF->isDraft() && draftF->isDirty() &&
              draftF->doc.label() == labelF && draftF->songId == -1,
          "cancelling the Rename dialog changed the draft");
    check(tabTextOf(draftF) == QStringLiteral("[draft] %1*").arg(labelF) && bannerOf(draftF),
          "the cancelled draft's tab title or banner changed");
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
                                          bool *okEnabledAfter, QString *explanation = nullptr) {
        answerer.onDialog(QStringLiteral("Rename Song"), [=](QDialog *dialog) {
            if (explanation)
                *explanation = renameExplanation(dialog);
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
                NoPromptGuard noPrompt(modalFail, __LINE__);
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
                QString explanation;
                keepLabelNewConstant(rename, labelI, constantI2, &okDisabledFirst, &okEnabledAfter,
                                     &explanation);
                check(saveSession(*draftI), "saving the partially committed draft failed");
                check(rename.handled(QStringLiteral("Rename Song")),
                      "a taken constant did not open the Rename dialog");
                // Review C: the earlier attempt's .mid is on disk.
                check(!explanation.contains(notWrittenYet) &&
                          explanation.contains(QStringLiteral("already wrote:")) &&
                          explanation.contains(QDir::toNativeSeparators(
                              QStringLiteral("sound/songs/midi/%1.mid").arg(labelI))),
                      "the Rename dialog after a partial commit does not list the written .mid");
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

    // 13-17. A draft with a new voicegroup (PLAN step 3): the voicegroup
    // lives in memory and in the draft's own .porydaw/drafts/<uuid>/ folder
    // until the commit writes it — with its edits — only if the cfg still
    // names it.
    const auto readBytes = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QString hubPath = root + QStringLiteral("/sound/voice_groups.inc");
    const auto includeCount = [&](const QString &name) {
        return int(readBytes(hubPath).count(
            QStringLiteral("\"sound/voicegroups/%1.inc\"").arg(name).toUtf8()));
    };
    const auto newVgCfg = [&cfg](const QString &name) {
        SongCfg c = cfg;
        c.voicegroupArg = QStringLiteral("_") + name;
        return c;
    };
    // Slot 0 becomes a square_2 (structural: reloaded), slot 1's sustain
    // drops (scalar: poked into the loaded ToneData).
    const auto editVoices = [&](SongSession *session) {
        VgVoice v0 = *session->vgSource->voiceAt(0);
        v0.macro = VgMacro::Square2;
        onVoiceEditRequested(0, v0, /*structural=*/true);
        VgVoice v1 = *session->vgSource->voiceAt(1);
        v1.sustain = 9;
        onVoiceEditRequested(1, v1, /*structural=*/false);
    };

    // 13. Opening, editing and discarding: nothing outside .porydaw/.
    {
        const QString name = QStringLiteral("mus_draftcheck_vg1");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        const QByteArray before = treeFingerprint(root);
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VG1"), player,
                                 newVgCfg(name), name, &error),
                   "a draft with a new voicegroup did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        check(treeFingerprint(root) == before, "a draft with a new voicegroup wrote on open");
        check(s->editsDraftVoicegroup() && s->vgSource->filePath() == target &&
                  !s->vgFileTime.isValid() && !QFile::exists(target),
              "the draft's voicegroup source is not the unwritten new voicegroup");
        const QString folder = s->draft->folder;
        check(!folder.isEmpty() && folder.startsWith(root + QStringLiteral("/.porydaw/drafts/")) &&
                  QFileInfo(folder).isDir() && QFile::exists(folder + QStringLiteral("/.lock")),
              "the draft has no locked folder under .porydaw/drafts/");
        const QByteArray rendered =
            VoicegroupSource::renderNewVoicegroup(root, name, QString(), QString(), &error);
        check(!rendered.isEmpty() && s->draft->voicegroupBytes == rendered &&
                  readBytes(folder + QLatin1Char('/') + name + QStringLiteral(".inc")) == rendered,
              "the draft folder does not hold the voicegroup createVoicegroup would write");
        check(s->voicegroup != nullptr && !s->vgSource->dirty(),
              "the draft's new voicegroup did not load clean");
        check(voicegroupChoices(*s).contains(QStringLiteral("_") + name) &&
                  !vgCatalog(root).groupArgs.contains(QStringLiteral("_") + name),
              "the dock's choices do not offer the draft's voicegroup (or the catalog has it)");

        const uint8_t type0 = s->voicegroup->voices[0].type;
        const uint8_t sustain1 = s->voicegroup->voices[1].sustain;
        editVoices(s);
        check(s->vgSource->dirty() && s->voicegroup->voices[0].type != type0 &&
                  s->voicegroup->voices[1].sustain != sustain1,
              "the draft voicegroup's edits are not heard");
        const QByteArray synced = readBytes(folder + QLatin1Char('/') + name + ".inc");
        check(synced == s->vgSource->renderPreview() && synced.contains("voice_square_2"),
              "the draft folder's copy did not follow the edits");
        check(treeFingerprint(root) == before && !QFile::exists(target),
              "editing the draft's voicegroup wrote into the project");
        // Without the preview shadow, a reload reads the draft folder.
        cleanupVgPreview();
        check(QFileInfo(folder).isDir(), "cleanupVgPreview removed the draft folder");
        QString tried;
        if (LoadedVoiceGroup *reloaded = loadVoicegroupForSession(*s, s->doc.cfg(), &tried)) {
            check(reloaded->voices[0].type == s->voicegroup->voices[0].type &&
                      reloaded->voices[1].sustain == s->voicegroup->voices[1].sustain,
                  "reloading the draft's voicegroup lost the edits");
            voicegroup_free(reloaded);
        } else {
            check(false, "the draft's voicegroup does not reload from its folder");
        }

        {
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            QString info;
            prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
            closeTab(m_tabs->indexOf(s->view));
            check(prompt.answered(), "closing a draft with a new voicegroup did not prompt");
            // Its own voicegroup is part of the song, not a shared one.
            check(info == plainDraftInfo,
                  "the close prompt of a draft editing its own new voicegroup named a shared one");
        }
        check(!sessionForLabel(name), "discarding did not close the voicegroup draft");
        check(!QFileInfo::exists(folder), "discarding the draft left its folder behind");
        check(treeFingerprint(root) == before && !QFile::exists(target) && includeCount(name) == 0,
              "discarding a voicegroup draft wrote into the project");
    }

    // 14. Save commits the voicegroup with its edits, its include line, and
    // a .mid whose -G names it.
    const QString vgHint = QStringLiteral(" — configure its new voicegroup in the Voicegroup dock");
    // Whether an open draft has a folder of its own under drafts/.
    const auto anyDraftFolder = [this] {
        return std::any_of(m_sessions.begin(), m_sessions.end(), [](const auto &session) {
            return session->isDraft() && !session->draft->folder.isEmpty();
        });
    };
    const QString draftsDir = root + QStringLiteral("/.porydaw/drafts");
    {
        const QString name = QStringLiteral("mus_draftcheck_vg2");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VG2"), player,
                                 newVgCfg(name), name, &error),
                   "the second voicegroup draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QString folder = s->draft->folder;
        editVoices(s);
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            check(saveSession(*s), "saving a voicegroup draft failed");
        }
        check(!s->isDraft() && !s->isDirty() && !s->vgSource->dirty(),
              "the committed voicegroup draft is still unsaved");
        // Edited as a draft: no hint to go configure it (step 6, H2; 14b
        // has the unedited case).
        check(statusBar()->currentMessage().startsWith(QStringLiteral("Created and registered")) &&
                  !statusBar()->currentMessage().contains(vgHint),
              "the commit's status message sends the user to configure an edited voicegroup");
        const QByteArray written = readBytes(target);
        check(!written.isEmpty() && written.contains("voice_square_2") &&
                  written.contains(QStringLiteral("voice_group %1").arg(name).toUtf8()) &&
                  written == s->vgSource->renderPreview(),
              "the committed voicegroup file is missing or lacks the edits");
        check(includeCount(name) == 1, "the committed voicegroup's include line is not there once");
        check(!QFileInfo::exists(folder), "the commit left the draft folder behind");
        check(s->vgSource->filePath() == target && s->vgFileTime.isValid() &&
                  s->vgFileTime == QFileInfo(target).lastModified(),
              "the committed session does not track the project's voicegroup file");
        check(vgCatalog(root).groupArgs.contains(QStringLiteral("_") + name),
              "the catalog does not list the committed voicegroup");
        const SongInfo *song = songNamed(name);
        check(song && song->registered && song->hasCfg &&
                  song->cfg.voicegroupArg == QStringLiteral("_") + name && s->songId == song->id,
              "the committed song's -G does not name its new voicegroup");
        VoicegroupSource reopened;
        check(reopened.open(root, QStringLiteral("_") + name, &error) && reopened.voiceAt(0) &&
                  reopened.voiceAt(0)->macro == VgMacro::Square2,
              "the committed voicegroup does not reopen from the project");
        // The voice edit is still undoable against the project file.
        s->doc.undoStack()->undo();
        check(s->vgSource->dirty() && s->isDirty(), "undoing a committed voice edit did nothing");
        s->doc.undoStack()->redo();
        check(!s->vgSource->dirty() && !s->isDirty(),
              "redo did not return to the saved voicegroup");
    }

    // 14b. An unedited new voicegroup: the status message sends the user to
    // configure it, and with no draft left open the commit leaves no empty
    // .porydaw/drafts/ behind (step 6, H1/H2).
    {
        const QString name = QStringLiteral("mus_draftcheck_vg2b");
        if (check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VG2B"), player,
                                newVgCfg(name), name, &error),
                  "the unedited voicegroup draft did not open")) {
            SongSession *s = m_active;
            {
                NoPromptGuard noPrompt(modalFail, __LINE__);
                check(saveSession(*s), "saving an unedited voicegroup draft failed");
            }
            check(!s->isDraft() && statusBar()->currentMessage().endsWith(vgHint),
                  "the commit's status message lacks the new-voicegroup hint");
            if (anyDraftFolder())
                std::printf("draftcheck: note: another draft has a folder; section 14b's "
                            "empty-drafts check skipped\n");
            else
                check(!QFileInfo::exists(draftsDir),
                      "the commit left an empty .porydaw/drafts/ behind");
        } else {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
        }
    }

    // 15. The cfg switched to an existing voicegroup (and back, and away
    // again): the edits follow the switches, and Save writes no voicegroup.
    {
        const QString name = QStringLiteral("mus_draftcheck_vg3");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VG3"), player,
                                 newVgCfg(name), name, &error),
                   "the third voicegroup draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QString folder = s->draft->folder;
        editVoices(s);
        const uint8_t editedType = s->voicegroup->voices[0].type;
        SongCfg away = s->doc.cfg();
        away.voicegroupArg = cfg.voicegroupArg;
        s->doc.setCfg(away);
        check(!s->editsDraftVoicegroup() && s->appliedVoicegroupArg == cfg.voicegroupArg &&
                  voicegroupChoices(*s).contains(QStringLiteral("_") + name),
              "switching away did not open the existing voicegroup (or dropped the choice)");
        s->doc.setCfg(newVgCfg(name));
        check(s->editsDraftVoicegroup() && s->vgSource->dirty() &&
                  s->vgSource->voiceAt(0)->macro == VgMacro::Square2 &&
                  s->voicegroup->voices[0].type == editedType,
              "switching back to the draft's voicegroup lost its edits");
        s->doc.setCfg(away);
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            check(saveSession(*s), "saving a draft switched to another voicegroup failed");
        }
        check(!s->isDraft() && !QFile::exists(target) && includeCount(name) == 0,
              "a draft switched away from its voicegroup still wrote it");
        check(statusBar()->currentMessage().startsWith(QStringLiteral("Created and registered")) &&
                  !statusBar()->currentMessage().contains(QStringLiteral("new voicegroup")),
              "a commit that wrote no voicegroup still hints at one");
        check(!QFileInfo::exists(folder), "the abandoned voicegroup's draft folder remained");
        const SongInfo *song = songNamed(name);
        check(song && song->registered && song->cfg.voicegroupArg == cfg.voicegroupArg,
              "the switched draft's -G does not name the existing voicegroup");
        // Undoing the switches (review A) must not bring back the -G of a
        // voicegroup that was never written: the history now names the
        // committed one, so every undo keeps it, and the next save writes
        // no draft -G into midi.cfg.
        const QString abandonedArg = QStringLiteral("_") + name;
        check(!voicegroupChoices(*s).contains(abandonedArg),
              "the abandoned voicegroup is still offered as a choice");
        bool kept = true;
        while (s->doc.undoStack()->canUndo()) {
            s->doc.undoStack()->undo();
            kept = kept && s->doc.cfg().voicegroupArg == cfg.voicegroupArg &&
                   s->appliedVoicegroupArg == cfg.voicegroupArg;
        }
        check(kept, "undoing a committed draft's settings edits restored its abandoned -G");
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            check(saveSession(*s), "saving the undone committed draft failed");
        }
        const QByteArray cfgText = readBytes(midiDir + QStringLiteral("midi.cfg"));
        check(!cfgText.isEmpty() &&
                  !cfgText.toLower().contains(QStringLiteral("-g%1").arg(abandonedArg).toUtf8()),
              "saving after the undo wrote the abandoned voicegroup's -G into midi.cfg");
        check(reloadProject(&error) && songNamed(name) &&
                  songNamed(name)->cfg.voicegroupArg == cfg.voicegroupArg,
              "after the undo and save the song's -G is not the existing voicegroup");
    }

    // 16. voicegroup_<name> declared in another file (review E), so the
    // file name alone is free: the shared check sees the symbol, and Save's
    // Rename renames the draft's voicegroup — file, symbol, -G — with it.
    {
        const QString name = QStringLiteral("mus_draftcheck_vgr");
        const QString name2 = QStringLiteral("mus_draftcheck_vgr2");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        const QString target2 = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name2);
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VGR"), player,
                                 newVgCfg(name), name, &error),
                   "the renamed voicegroup draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QString folder = s->draft->folder;
        editVoices(s);
        const QString multiPath = root + QStringLiteral("/sound/voicegroups/draftcheck_multi.inc");
        {
            QFile multi(multiPath);
            const QByteArray bytes = QStringLiteral("voice_group draftcheck_other\n"
                                                    "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0\n"
                                                    "voice_group %1\n"
                                                    "\tvoice_square_1 60, 0, 0, 2, 0, 0, 15, 0\n")
                                         .arg(name)
                                         .toUtf8();
            check(multi.open(QIODevice::WriteOnly) && multi.write(bytes) == bytes.size(),
                  "could not plant a multi-voicegroup file");
        }
        check(!QFile::exists(target) &&
                  !SongRegistry::checkNewSongNames(root, {}, {}, QString(), QString(), name)
                       .voicegroup.isEmpty(),
              "checkNewSongNames missed a voicegroup symbol declared in another file");
        check(SongRegistry::checkNewSongNames(root, {}, {}, QString(), QString(), name2)
                  .voicegroup.isEmpty(),
              "checkNewSongNames rejected a free voicegroup name");
        bool okDisabledFirst = false;
        bool okEnabledAfter = false;
        {
            PromptAnswerer rename(QMessageBox::Cancel, modalFail);
            rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                QPushButton *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
                QLineEdit *field = nameField(dialog);
                if (!ok || !field) {
                    dialog->reject();
                    return;
                }
                okDisabledFirst = !ok->isEnabled() && field->text() == name;
                field->setText(name2);
                okEnabledAfter = ok->isEnabled();
                ok->click();
            });
            check(saveSession(*s), "saving a draft whose voicegroup symbol was taken failed");
            check(rename.handled(QStringLiteral("Rename Song")),
                  "a taken voicegroup symbol did not open the Rename dialog");
        }
        check(okDisabledFirst, "the Rename dialog accepted a taken voicegroup symbol");
        check(okEnabledAfter, "the Rename dialog rejected a free voicegroup name");
        check(!s->isDraft() && s->doc.label() == name2 &&
                  s->doc.cfg().voicegroupArg == QStringLiteral("_") + name2 &&
                  s->appliedVoicegroupArg == s->doc.cfg().voicegroupArg,
              "the rename did not carry the draft's -G arg to the new voicegroup");
        const QByteArray written = readBytes(target2);
        check(written.contains(QStringLiteral("voice_group %1\n").arg(name2).toUtf8()) &&
                  written.contains("voice_square_2") && s->vgSource->filePath() == target2,
              "the renamed voicegroup was not written under its new name with its edits");
        check(!QFile::exists(target) && includeCount(name) == 0 && includeCount(name2) == 1,
              "the old voicegroup name reached the project");
        check(!QFileInfo::exists(folder), "the renamed draft left its folder behind");
        const SongInfo *song = songNamed(name2);
        check(song && song->registered && song->cfg.voicegroupArg == QStringLiteral("_") + name2,
              "the renamed song's -G does not name the renamed voicegroup");
        // The pre-rename voice edits follow the voicegroup's new name.
        s->doc.undoStack()->undo();
        check(s->vgSource->dirty(), "a voice edit made before the rename no longer undoes");
        s->doc.undoStack()->redo();
        check(!s->vgSource->dirty(),
              "redo after the rename did not return to the saved voicegroup");
        QFile::remove(multiPath);
    }

    // 17. Stale-folder sweep at project open: an unlocked folder (a crash's
    // leftover) goes; one whose lock a live process holds stays.
    if (SongSession *leftover = sessionForLabel(labelF); leftover && leftover->isDraft()) {
        PromptAnswerer prompt(QMessageBox::Discard, modalFail);
        closeTab(m_tabs->indexOf(leftover->view));
    }
    {
        const QString drafts = root + QStringLiteral("/.porydaw/drafts/");
        const QString stale = drafts + QStringLiteral("draftcheck-stale");
        const QString held = drafts + QStringLiteral("draftcheck-held");
        QDir().mkpath(stale);
        QDir().mkpath(held);
        {
            QFile planted(stale + QStringLiteral("/mus_stale.inc"));
            check(planted.open(QIODevice::WriteOnly) &&
                      planted.write("voice_group mus_stale\n") > 0,
                  "could not plant a stale draft folder");
        }
        auto heldLock = std::make_unique<QLockFile>(held + QStringLiteral("/.lock"));
        heldLock->setStaleLockTime(0);
        check(heldLock->tryLock(0), "could not lock the planted held draft folder");
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            check(openProjectDir(root, /*interactive=*/false), "reopening the project failed");
        }
        check(!QFileInfo::exists(stale), "the sweep left an unlocked draft folder");
        check(QFileInfo::exists(held + QStringLiteral("/.lock")),
              "the sweep removed a draft folder another process holds");
        heldLock.reset();
        QDir(held).removeRecursively();
        // An emptied drafts/ goes with its last folder (step 6, H1).
        sweepStaleDraftFolders(root);
        check(anyDraftFolder() || !QFileInfo::exists(draftsDir),
              "the sweep left an empty .porydaw/drafts/ behind");
    }

    // 18. A voicegroup commit that failed after the .inc landed (review E):
    // the .mid can't be written (the midi directory made read-only; skipped
    // with a note where the chmod doesn't bite). The retry, with no changes,
    // commits under the same names with no Rename dialog and leaves the
    // .inc and its include line as the first attempt wrote them. 18b: the
    // same failure on a draft switched away from its voicegroup — the
    // voicegroup is abandoned for good (review A) even though the draft
    // stays a draft.
    const QString midiDirPath = QDir::cleanPath(midiDir);
    const QFileDevice::Permissions midiPerms = QFile::permissions(midiDirPath);
    const auto lockMidiDir = [&]() {
        if (!QFile::setPermissions(midiDirPath, QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                                    QFileDevice::ReadUser | QFileDevice::ExeUser |
                                                    QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                                    QFileDevice::ReadOther | QFileDevice::ExeOther))
            return false;
        QFile probe(midiDir + QStringLiteral(".draftcheck_probe"));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            probe.remove();
            QFile::setPermissions(midiDirPath, midiPerms);
            return false;
        }
        return true;
    };
    // Save with the midi directory locked: fails, reporting its error.
    const auto failedSave = [&](SongSession *session) {
        if (!lockMidiDir())
            return false;
        {
            PromptAnswerer failBox(QMessageBox::Cancel, modalFail);
            failBox.onDialog(QStringLiteral("Save Song"),
                             [](QDialog *dialog) { dialog->reject(); });
            check(!saveSession(*session), "saving with an unwritable midi directory succeeded");
            check(failBox.handled(QStringLiteral("Save Song")),
                  "the failed .mid write did not report its error");
        }
        QFile::setPermissions(midiDirPath, midiPerms);
        return true;
    };
    {
        const QString name = QStringLiteral("mus_draftcheck_vg4");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        const QString midPath = midiDir + name + QStringLiteral(".mid");
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VG4"), player,
                                 newVgCfg(name), name, &error),
                   "the fourth voicegroup draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QString folder = s->draft->folder;
        editVoices(s);
        if (!failedSave(s)) {
            std::printf("draftcheck: note: the midi directory stays writable after chmod (root "
                        "or a file system that ignores permissions); section 18 skipped\n");
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            closeTab(m_tabs->indexOf(s->view));
        } else {
            const QByteArray first = readBytes(target);
            const QDateTime firstTime = QFileInfo(target).lastModified();
            check(s->isDraft() && s->draft && s->draft->voicegroupWritten &&
                      first.contains("voice_square_2") && includeCount(name) == 1,
                  "the failed commit did not land the voicegroup file and its include line");
            check(!QFile::exists(midPath) && s->draft->wroteMidPath.isEmpty(),
                  "the failed commit wrote the .mid");
            check(!QFileInfo::exists(folder) && s->vgSource->filePath() == target &&
                      !s->editsDraftVoicegroup(),
                  "after the voicegroup landed the session still edits the draft copy");
            {
                NoPromptGuard noPrompt(modalFail, __LINE__);
                check(saveSession(*s), "retrying the failed voicegroup commit failed");
            }
            const SongInfo *song = songNamed(name);
            check(!s->isDraft() && s->doc.label() == name && song && song->registered &&
                      song->cfg.voicegroupArg == QStringLiteral("_") + name &&
                      QFile::exists(midPath),
                  "the retry did not commit under the original names");
            check(readBytes(target) == first && QFileInfo(target).lastModified() == firstTime &&
                      includeCount(name) == 1,
                  "the retry rewrote the voicegroup file or its include line");
        }
    }
    {
        const QString name = QStringLiteral("mus_draftcheck_vg5");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_VG5"), player,
                                 newVgCfg(name), name, &error),
                   "the fifth voicegroup draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QString folder = s->draft->folder;
        editVoices(s);
        SongCfg away = s->doc.cfg();
        away.voicegroupArg = cfg.voicegroupArg;
        s->doc.setCfg(away);
        if (!failedSave(s)) {
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            closeTab(m_tabs->indexOf(s->view));
        } else {
            const QString arg = QStringLiteral("_") + name;
            check(s->isDraft() && s->draft && s->draft->newVoicegroup.isEmpty() &&
                      !QFileInfo::exists(folder) && !QFile::exists(target) &&
                      includeCount(name) == 0,
                  "a failed commit did not abandon the switched-away voicegroup");
            check(!voicegroupChoices(*s).contains(arg) &&
                      !reservedSongNames(nullptr).voicegroups.contains(name),
                  "the abandoned voicegroup is still offered or reserved");
            bool kept = true;
            while (s->doc.undoStack()->canUndo()) {
                s->doc.undoStack()->undo();
                kept = kept && s->doc.cfg().voicegroupArg == cfg.voicegroupArg;
            }
            check(kept, "undo on a draft restored its abandoned voicegroup's -G");
            {
                NoPromptGuard noPrompt(modalFail, __LINE__);
                check(saveSession(*s), "retrying the switched-away draft failed");
            }
            const SongInfo *song = songNamed(name);
            check(!s->isDraft() && song && song->registered &&
                      song->cfg.voicegroupArg == cfg.voicegroupArg && !QFile::exists(target),
                  "the retried switched-away draft did not commit with the existing -G");
        }
    }

    // 19. An existing voicegroup edited in a draft whose name is taken (PLAN
    // step 4 item 5): Save settles the names before any write, so Cancel on
    // the Rename dialog leaves that voicegroup unwritten (still dirty) and
    // the project untouched; accepting saves it along with the commit.
    {
        const QString name = QStringLiteral("mus_draftcheck_x");
        const QString name2 = QStringLiteral("mus_draftcheck_x2");
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_X"), player, cfg,
                                 QString(), &error),
                   "the existing-voicegroup draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        if (check(s->vgSource && !s->editsDraftVoicegroup() && s->vgSource->isEditable(0) &&
                      s->vgSource->isEditable(1),
                  "the draft's existing voicegroup is not editable")) {
            const QString vgPath = s->vgSource->filePath();
            const QByteArray vgBefore = readBytes(vgPath);
            editVoices(s);
            check(s->vgSource->dirty(), "editing the existing voicegroup did not dirty it");
            {
                QFile planted(midiDir + name + QStringLiteral(".mid"));
                check(planted.open(QIODevice::WriteOnly) && planted.write(plantedBytes) > 0,
                      "could not plant a file under the existing-voicegroup draft's .mid");
            }
            const QByteArray before = treeFingerprint(root);
            {
                PromptAnswerer rename(QMessageBox::Cancel, modalFail);
                rename.onDialog(QStringLiteral("Rename Song"),
                                [](QDialog *dialog) { dialog->reject(); });
                check(!saveSession(*s), "cancelling the Rename dialog still saved");
                check(rename.handled(QStringLiteral("Rename Song")),
                      "the existing-voicegroup draft did not open the Rename dialog");
            }
            check(treeFingerprint(root) == before && readBytes(vgPath) == vgBefore,
                  "cancelling the Rename dialog wrote the edited existing voicegroup");
            check(s->isDraft() && s->vgSource->dirty() && s->doc.label() == name,
                  "cancelling the Rename dialog changed the draft or its voicegroup edits");
            {
                // Review A: the close prompt says a shared voicegroup was
                // edited. Cancel keeps the tab.
                QString vgName = s->vgSource->loadName();
                if (vgName.startsWith(QStringLiteral("voicegroup_")))
                    vgName = vgName.mid(11);
                PromptAnswerer prompt(QMessageBox::Cancel, modalFail);
                QString info;
                prompt.inspect([&](QMessageBox *box) {
                    info = box->informativeText();
                    if (!shotsDir.isEmpty())
                        box->grab().toImage().save(shotsDir +
                                                   QStringLiteral("/draft-close-prompt-vg.png"));
                });
                closeTab(m_tabs->indexOf(s->view));
                check(prompt.answered(), "closing the existing-voicegroup draft did not prompt");
                check(info == vgDirtyDraftInfo(vgName),
                      "the draft close prompt does not name the edited existing voicegroup");
                check(sessionForLabel(name) == s && s->isDraft() && s->vgSource->dirty() &&
                          treeFingerprint(root) == before,
                      "cancelling the close prompt closed the draft or wrote");
            }
            {
                PromptAnswerer rename(QMessageBox::Cancel, modalFail);
                rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
                    auto *buttons = dialog->findChild<QDialogButtonBox *>();
                    QPushButton *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
                    QLineEdit *field = nameField(dialog);
                    if (!ok || !field) {
                        dialog->reject();
                        return;
                    }
                    field->setText(name2);
                    ok->click();
                });
                check(saveSession(*s), "renaming the existing-voicegroup draft failed");
            }
            const QByteArray written = readBytes(vgPath);
            check(!s->isDraft() && s->doc.label() == name2 && !s->vgSource->dirty() &&
                      written != vgBefore && written.contains("voice_square_2"),
                  "the commit did not save the edited existing voicegroup");
        }
    }

    // 20. The wizard's finish (createSongFromWizard): New Song and Import
    // MIDI open a draft and say so in the status bar.
    for (const bool imported : {false, true}) {
        const QString label =
            imported ? QStringLiteral("mus_draftcheck_imp") : QStringLiteral("mus_draftcheck_new");
        const QStringList vgArgs = vgCatalog(root).groupArgs;
        const ReservedSongNames reserved = reservedSongNames();
        std::unique_ptr<NewSongWizard> wizard =
            imported ? std::make_unique<NewSongWizard>(&m_project, draftSmf(),
                                                       QStringLiteral("draftcheck_import.mid"),
                                                       vgArgs, reserved)
                     : std::make_unique<NewSongWizard>(&m_project, vgArgs, reserved);
        QWizardPage *identity = nullptr;
        for (const int id : wizard->pageIds()) {
            if (nameField(wizard->page(id)))
                identity = wizard->page(id);
        }
        QLineEdit *name = identity ? nameField(identity) : nullptr;
        QLineEdit *constant = identity ? constantField(identity) : nullptr;
        if (!check(name && constant, "wizard finish: name or constant field not found"))
            continue;
        name->setText(label);
        constant->setText(label.toUpper());
        emit constant->textEdited(constant->text());
        const QByteArray before = treeFingerprint(root);
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            createSongFromWizard(*wizard, imported);
        }
        SongSession *s = m_active;
        if (!check(s && s->isDraft() && s->doc.label() == label && bannerOf(s),
                   "the wizard's finish did not open a draft tab"))
            continue;
        check(
            statusBar()->currentMessage() ==
                QStringLiteral("%1 %2 as a draft — Save to add it to the project.")
                    .arg(imported ? QStringLiteral("Imported") : QStringLiteral("Created"), label),
            "the wizard's finish did not say it opened a draft");
        check(treeFingerprint(root) == before, "the wizard's finish wrote into the project");
        PromptAnswerer prompt(QMessageBox::Discard, modalFail);
        closeTab(m_tabs->indexOf(s->view));
        check(prompt.answered() && !sessionForLabel(label),
              "the wizard's draft did not close through the prompt");
    }

    // 21. The rest of the app on a draft (PLAN step 5): Register Song is
    // disabled; Song Settings edits the cfg the commit writes; Export WAV
    // renders it; Export Song Bundle exports it from memory with the draft's
    // constant and player as hints; Import Sample for a slot writes the
    // sample through but puts the voice into the draft's own voicegroup,
    // which reaches the project only with the commit.
    QTemporaryDir outDir;
    if (!check(outDir.isValid(), "no temp dir for exports"))
        return failures;
    const auto fileDialogPick = [&modalFail](const QString &path) {
        return [path, &modalFail](QDialog *dialog) {
            auto *files = qobject_cast<QFileDialog *>(dialog);
            // A message box under the same title is the action's error
            // report (Export WAV's "could not render"), not a step to click
            // through.
            if (auto *box = qobject_cast<QMessageBox *>(dialog)) {
                modalFail(QStringLiteral("unexpected message box \"%1\": %2")
                              .arg(box->windowTitle(), box->text()));
                dialog->reject();
                return;
            }
            if (!files) {
                dialog->accept(); // an options dialog sharing the title
                return;
            }
            // Typed, as a user would: selectFile only selects what the
            // dialog's (asynchronously filled) model already lists.
            if (auto *edit = files->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
                edit->setText(path);
            else
                files->selectFile(path);
            dialog->accept(); // QFileDialog::accept, through QDialog's public one
        };
    };
    {
        const QString name = QStringLiteral("mus_draftcheck_app");
        const QString constant = QStringLiteral("MUS_DRAFTCHECK_APP");
        const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
        // A player other than the default, so the bundle's hint is the
        // draft's own and not a fallback.
        QString appPlayer = player;
        for (const MusicPlayer &p : m_project.musicPlayers()) {
            if (p.name != player)
                appPlayer = p.name;
        }
        if (!check(
                openDraftSong(draftSmf(), name, constant, appPlayer, newVgCfg(name), name, &error),
                "the app draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QString folder = s->draft->folder;
        const QMap<QString, QString> before = treeListing(root);
        check(!m_registerAction->isEnabled(), "Register Song is enabled on a draft");

        // Song Settings: the dialog's accept is doc.setCfg(dialog.cfg()).
        SongCfg edited = s->doc.cfg();
        edited.masterVolume = 90;
        edited.reverb = 37;
        s->doc.setCfg(edited);
        check(!songNamed(name) && s->songId == -1,
              "Song Settings on a draft reached the project's song list");

        // Export WAV, through the menu action's options and file dialogs.
        const QString wavPath = outDir.filePath(QStringLiteral("draft.wav"));
        {
            PromptAnswerer dialogs(QMessageBox::Cancel, modalFail);
            dialogs.onDialog(QStringLiteral("Export WAV"), fileDialogPick(wavPath), 2);
            exportWav();
            check(dialogs.handled(QStringLiteral("Export WAV")),
                  "Export WAV on a draft showed no dialog");
        }
        check(QFileInfo(wavPath).size() > 44 &&
                  statusBar()->currentMessage().startsWith(QStringLiteral("Exported ")),
              "Export WAV did not render the draft");

        // Export Song Bundle, from memory, with the draft's hints.
        const QString bundlePath = outDir.filePath(QStringLiteral("draft.porysong"));
        {
            PromptAnswerer dialogs(QMessageBox::Cancel, modalFail);
            dialogs.onDialog(QStringLiteral("Export Song Bundle"), fileDialogPick(bundlePath));
            exportBundle();
            check(dialogs.handled(QStringLiteral("Export Song Bundle")),
                  "Export Song Bundle on a draft showed no file dialog");
        }
        const QString extracted = outDir.filePath(QStringLiteral("bundle"));
        BundleManifest manifest;
        SongInfo bundled;
        if (check(BundleArchive::extractBundle(bundlePath, extracted, &error) &&
                      SongBundle::readSong(extracted, &manifest, &bundled, &error),
                  "the draft's bundle does not read back")) {
            check(manifest.label == name && manifest.constant == constant &&
                      manifest.player == appPlayer,
                  "the draft's bundle lacks its constant and player hints");
            check(manifest.voicegroup == QStringLiteral("voicegroup_") + name,
                  "the draft's bundle does not carry its new voicegroup");
            // An import never claims the draft's names: the plan moves past
            // the reserved label, constant and voicegroup (and, without the
            // reservation, would take all three).
            SongBundle::ImportOptions options;
            const SongBundle::ImportPlan free =
                SongBundle::makeImportPlan(extracted, root, options);
            options.reserved = reservedSongNames();
            const SongBundle::ImportPlan reserved =
                SongBundle::makeImportPlan(extracted, root, options);
            check(free.label == name && free.constant == constant && free.voicegroup.name == name,
                  "bundle import: without the reservation the plan did not take the draft's names");
            check(reserved.label != name && reserved.constant != constant &&
                      reserved.voicegroup.name != name,
                  "bundle import: the plan claimed a name an open draft holds");
            options.label = name;
            check(!SongBundle::makeImportPlan(extracted, root, options).ok(),
                  "bundle import: a label an open draft holds was accepted");
        }

        // Import Sample into slot 2: file dialog, then the Sample Editor.
        const QString sampleName = QStringLiteral("draftcheck_tone");
        const QString sourceWav = outDir.filePath(QStringLiteral("tone.wav"));
        {
            // 16-bit mono PCM, a quarter second of a 440 Hz sine.
            const int rate = 13379;
            const int frames = rate / 4;
            QByteArray pcm;
            for (int i = 0; i < frames; i++) {
                const qint16 v = qint16(12000.0 * std::sin(2.0 * M_PI * 440.0 * i / rate));
                pcm.append(char(v & 0xFF));
                pcm.append(char((v >> 8) & 0xFF));
            }
            const auto u32 = [](quint32 v) {
                return QByteArray(1, char(v & 0xFF)) + char((v >> 8) & 0xFF) +
                       char((v >> 16) & 0xFF) + char((v >> 24) & 0xFF);
            };
            const auto u16 = [](quint16 v) {
                return QByteArray(1, char(v & 0xFF)) + char((v >> 8) & 0xFF);
            };
            QByteArray wav = "RIFF" + u32(36 + pcm.size()) + "WAVEfmt " + u32(16) + u16(1) +
                             u16(1) + u32(rate) + u32(rate * 2) + u16(2) + u16(16) + "data" +
                             u32(pcm.size()) + pcm;
            QFile out(sourceWav);
            check(out.open(QIODevice::WriteOnly) && out.write(wav) == wav.size(),
                  "could not write the sample fixture");
        }
        const QString symbol = QStringLiteral("DirectSoundWaveData_") + sampleName;
        {
            PromptAnswerer dialogs(QMessageBox::Cancel, modalFail);
            dialogs.onDialog(QStringLiteral("Import Sample"), fileDialogPick(sourceWav));
            dialogs.onDialog(QStringLiteral("Sample Editor"), [&](QDialog *dialog) {
                if (auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("sampleNameEdit")))
                    edit->setText(sampleName);
                dialog->accept();
            });
            importSampleForSlot(2);
            check(dialogs.handled(QStringLiteral("Sample Editor")),
                  "Import Sample for a draft's slot did not reach the Sample Editor");
        }
        check(QFile::exists(root +
                            QStringLiteral("/sound/direct_sound_samples/%1.wav").arg(sampleName)),
              "Import Sample did not write the sample (write-through by design)");
        check(s->editsDraftVoicegroup() && s->vgSource->voiceAt(2) &&
                  s->vgSource->voiceAt(2)->symbol == symbol && s->vgSource->dirty(),
              "the imported sample was not assigned to the draft voicegroup's slot");
        check(readBytes(folder + QLatin1Char('/') + name + QStringLiteral(".inc"))
                  .contains(symbol.toUtf8()),
              "the draft folder's voicegroup copy lacks the imported sample");
        check(!QFile::exists(target) && includeCount(name) == 0,
              "Import Sample into a draft wrote the draft's voicegroup into the project");
        check(s->isDraft() && !songNamed(name), "the imports before the commit changed the draft");
        // Before the commit the project holds only what Import Sample writes
        // through: the sample and the sample table (its sidecar is
        // .porydaw/samples/<name>.json, outside the listing — asserted on
        // its own).
        {
            const QStringList changed = treeChanges(before, treeListing(root));
            const QStringList expected = {
                QStringLiteral("sound/direct_sound_data.inc"),
                QStringLiteral("sound/direct_sound_samples/%1.wav").arg(sampleName),
            };
            if (!check(changed == expected,
                       "before the commit the project changed by more (or less) than Import "
                       "Sample's sample and direct_sound_data.inc"))
                std::fprintf(stderr, "draftcheck: changed: %s\n",
                             qUtf8Printable(changed.join(QStringLiteral(", "))));
            check(QFile::exists(root + QStringLiteral("/.porydaw/samples/%1.json").arg(sampleName)),
                  "Import Sample did not write the sample's sidecar");
        }

        // The commit: the voicegroup with the sample's voice, the flags as
        // Song Settings left them, the registration with the draft's names.
        {
            NoPromptGuard noPrompt(modalFail, __LINE__);
            check(saveSession(*s), "saving the app draft failed");
        }
        const SongInfo *song = songNamed(name);
        check(!s->isDraft() && song && song->registered && song->constant == constant &&
                  song->player == appPlayer,
              "the app draft did not commit with its names");
        check(song && song->hasCfg && song->cfg.masterVolume == 90 && song->cfg.reverb == 37,
              "the commit did not write the flags Song Settings set on the draft");
        check(readBytes(target).contains(symbol.toUtf8()),
              "the committed voicegroup lacks the imported sample's voice");
        check(m_active == s && !m_registerAction->isEnabled(),
              "Register Song is enabled on a fully registered committed draft");
    }

    // 22. A partial commit then Discard (PLAN step 5, E/F): everything that
    // commit wrote — the .mid, the new voicegroup's file and include line —
    // goes again, and the close prompt says so. Needs the chmod tricks of
    // sections 11 and 18.
    const QString hubRel = QStringLiteral("sound/voice_groups.inc");
    const auto wroteText = QStringLiteral("An earlier save that failed partway already wrote into "
                                          "the project: ");
    if (!lockCfg()) {
        std::printf("draftcheck: note: midi.cfg stays writable after chmod; rollback sections "
                    "22a-22d skipped\n");
    } else {
        QFile::setPermissions(cfgPath, cfgPerms);
        // 22a. .inc + include line + .mid (the flags write failed), then the
        // tab closed answering Discard.
        {
            const QString name = QStringLiteral("mus_draftcheck_rb1");
            const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            const QString midPath = midiDir + name + QStringLiteral(".mid");
            const QByteArray before = treeFingerprint(root, {hubRel});
            if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_RB1"), player,
                                     newVgCfg(name), name, &error),
                       "the rollback draft did not open")) {
                std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
                return failures;
            }
            SongSession *s = m_active;
            editVoices(s);
            check(lockCfg(), "midi.cfg could no longer be made unwritable");
            {
                PromptAnswerer failBox(QMessageBox::Cancel, modalFail);
                failBox.onDialog(QStringLiteral("Save Song"),
                                 [](QDialog *dialog) { dialog->reject(); });
                check(!saveSession(*s), "saving with an unwritable midi.cfg succeeded");
            }
            QFile::setPermissions(cfgPath, cfgPerms);
            check(s->isDraft() && s->draft->voicegroupWritten &&
                      s->draft->wroteMidPath == midPath && QFile::exists(target) &&
                      QFile::exists(midPath) && includeCount(name) == 1,
                  "the partial commit did not land the .inc, its include line and the .mid");
            {
                PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                QString info;
                prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
                closeTab(m_tabs->indexOf(s->view));
                check(prompt.answered(), "closing the partly committed draft did not prompt");
                check(info.startsWith(plainDraftInfo) && info.contains(wroteText) &&
                          info.contains(QDir::toNativeSeparators(
                              QStringLiteral("sound/songs/midi/%1.mid").arg(name))) &&
                          info.contains(QDir::toNativeSeparators(
                              QStringLiteral("sound/voicegroups/%1.inc (and its include line)")
                                  .arg(name))) &&
                          info.endsWith(QStringLiteral("Discard removes what it wrote.")),
                      "the close prompt does not list what the failed save wrote");
            }
            check(!sessionForLabel(name), "Discard did not close the partly committed draft");
            check(!QFile::exists(midPath) && !QFile::exists(target) && includeCount(name) == 0,
                  "Discard left the partial commit's .mid, .inc or include line behind");
            check(treeFingerprint(root, {hubRel}) == before,
                  "the project after Discard is not what it was before the draft");
        }
        // 22b. A leftover that can't be removed (the midi directory made
        // read-only): the tab still closes, and a warning names the file.
        SongSession *stuck = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_rb2"),
                                 QStringLiteral("MUS_DRAFTCHECK_RB2"), &stuck)) {
            const QString midPath = midiDir + QStringLiteral("mus_draftcheck_rb2.mid");
            if (!lockMidiDir()) {
                std::printf("draftcheck: note: the midi directory stays writable after chmod; "
                            "section 22b's warning skipped\n");
                PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                closeTab(m_tabs->indexOf(stuck->view));
            } else {
                QString warning;
                {
                    PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                    prompt.onDialog(QStringLiteral("Discard Draft"), [&](QDialog *dialog) {
                        if (auto *box = qobject_cast<QMessageBox *>(dialog))
                            warning = box->text();
                        dialog->accept();
                    });
                    closeTab(m_tabs->indexOf(stuck->view));
                    check(prompt.answered() && prompt.handled(QStringLiteral("Discard Draft")),
                          "an unremovable leftover did not warn on Discard");
                }
                QFile::setPermissions(midiDirPath, midiPerms);
                check(warning.contains(QDir::toNativeSeparators(midPath)),
                      "the Discard warning does not name the file it could not remove");
                check(!sessionForLabel(QStringLiteral("mus_draftcheck_rb2")),
                      "a failed removal kept the discarded draft open");
            }
            QFile::remove(midPath);
        }
        // 22c. A project switch answering Discard.
        SongSession *switched = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_rb3"),
                                 QStringLiteral("MUS_DRAFTCHECK_RB3"), &switched)) {
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            check(openProjectDir(root, /*interactive=*/false), "reopening the project failed");
            check(prompt.answered(), "the project switch did not prompt for the draft");
            check(!QFile::exists(midiDir + QStringLiteral("mus_draftcheck_rb3.mid")) &&
                      !songNamed(QStringLiteral("mus_draftcheck_rb3")),
                  "a project switch answered Discard left the partial commit's .mid");
        }
        // 22d. A replace in place answering Discard.
        SongSession *replaced = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_rb4"),
                                 QStringLiteral("MUS_DRAFTCHECK_RB4"), &replaced)) {
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
                check(prompt.answered(), "loading a song over a partly committed draft did not "
                                         "prompt");
            }
            check(m_active == replaced && !replaced->isDraft() && replaced->doc.label() == target &&
                      !QFile::exists(midiDir + QStringLiteral("mus_draftcheck_rb4.mid")),
                  "a replace in place answered Discard left the partial commit's .mid");
        }
        // A new-voicegroup draft whose first Save failed after the .inc,
        // its include line and the .mid landed (midi.cfg unwritable), as in
        // 22a. planted: the hub already had the include line (a dangling one
        // the user left), so the commit added none.
        const auto partialVgCommit = [&](const QString &name, const QString &constant, bool planted,
                                         SongSession **out) {
            if (!check(
                    openDraftSong(draftSmf(), name, constant, player, newVgCfg(name), name, &error),
                    "a partial new-voicegroup draft did not open")) {
                std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
                return false;
            }
            SongSession *s = m_active;
            *out = s;
            editVoices(s);
            if (!check(lockCfg(), "midi.cfg could no longer be made unwritable"))
                return false;
            {
                PromptAnswerer failBox(QMessageBox::Cancel, modalFail);
                failBox.onDialog(QStringLiteral("Save Song"),
                                 [](QDialog *dialog) { dialog->reject(); });
                check(!saveSession(*s), "saving with an unwritable midi.cfg succeeded");
            }
            QFile::setPermissions(cfgPath, cfgPerms);
            return check(
                s->isDraft() && s->draft->voicegroupWritten &&
                    s->draft->includeLineAdded == !planted &&
                    s->draft->wroteMidPath == midiDir + name + QStringLiteral(".mid") &&
                    QFile::exists(root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name)) &&
                    QFile::exists(s->draft->wroteMidPath) && includeCount(name) == 1,
                "the partial new-voicegroup commit did not land the .inc, one include "
                "line (recorded as the commit's own or not) and the .mid");
        };
        const auto shownPath = [](const QString &rel) { return QDir::toNativeSeparators(rel); };

        // 22c2. A partly written voicegroup another tab has taken over since
        // (switched to it and saved) survives a project switch's Discard
        // (review C): the rollback runs before the switch, while that tab
        // and the old project can still tell. The same root is reopened, so
        // the guard's "keep" for a project it can't check (none open, or
        // another root) is asserted by direct calls.
        {
            const QString name = QStringLiteral("mus_draftcheck_rb7");
            const QString vgPath = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            SongSession *s = nullptr;
            if (partialVgCommit(name, QStringLiteral("MUS_DRAFTCHECK_RB7"), false, &s)) {
                const std::vector<std::unique_ptr<SongSession>> noSessions;
                check(!draftVoicegroupUsedElsewhere(*s, *s->draft, noSessions, m_project),
                      "the guard called the draft's voicegroup used before anything used it");
                check(draftVoicegroupUsedElsewhere(*s, *s->draft, noSessions, DecompProject()),
                      "the guard did not keep the voicegroup with no project open");
                QTemporaryDir elsewhere;
                DecompProject other;
                bool planted =
                    elsewhere.isValid() && QDir(elsewhere.path()).mkpath(QStringLiteral("sound"));
                {
                    QFile table(elsewhere.filePath(QStringLiteral("sound/song_table.inc")));
                    planted = planted && table.open(QIODevice::WriteOnly) &&
                              table.write("\tsong mus_dummy, MUSIC_PLAYER_BGM, 0\n") > 0;
                }
                if (check(planted && other.open(elsewhere.path(), &error),
                          "the mismatched-root project did not open")) {
                    check(draftVoicegroupUsedElsewhere(*s, *s->draft, noSessions, other),
                          "the guard did not keep the voicegroup against another project's root");
                } else {
                    std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
                }
                // Another tab takes the voicegroup over and saves.
                QString target;
                for (const SongInfo &song : m_project.songs()) {
                    if (song.isPlayable() && song.registered && song.hasCfg &&
                        !sessionForLabel(song.label)) {
                        target = song.label;
                        break;
                    }
                }
                const QString targetMid = midiDir + target + QStringLiteral(".mid");
                const QByteArray cfgBytes = readBytes(cfgPath);
                const QByteArray midBytes = readBytes(targetMid);
                loadSongByLabel(target, /*newTab=*/true);
                SongSession *o = m_active;
                if (check(o && o != s && o->doc.label() == target,
                          "the taking-over song did not open in its own tab")) {
                    SongCfg taken = o->doc.cfg();
                    taken.voicegroupArg = QStringLiteral("_") + name;
                    o->doc.setCfg(taken);
                    NoPromptGuard noPrompt(modalFail, __LINE__);
                    check(saveSession(*o), "saving the taking-over song failed");
                }
                check(draftVoicegroupUsedElsewhere(*s, *s->draft, m_sessions, m_project),
                      "the guard missed the tab that took the voicegroup over");
                {
                    PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                    QString info;
                    prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
                    check(openProjectDir(root, /*interactive=*/false),
                          "reopening the project failed");
                    check(prompt.answered(), "the project switch did not prompt for the draft");
                    check(info.contains(wroteText) &&
                              info.contains(
                                  QStringLiteral("The project uses %1 by now, so Discard keeps "
                                                 "that and removes the rest.")
                                      .arg(shownPath(
                                          QStringLiteral("sound/voicegroups/%1.inc").arg(name)))),
                          "the close prompt does not say the taken-over voicegroup stays");
                }
                check(QFile::exists(vgPath) && includeCount(name) == 1,
                      "a project switch's Discard removed a voicegroup another tab took over");
                check(!QFile::exists(midiDir + name + QStringLiteral(".mid")),
                      "a project switch's Discard kept the draft's own .mid");
                // Back to how it was, for the sections after.
                QFile::setPermissions(cfgPath, cfgPerms);
                QFile cfgOut(cfgPath);
                QFile midOut(targetMid);
                check(cfgOut.open(QIODevice::WriteOnly) &&
                          cfgOut.write(cfgBytes) == cfgBytes.size() &&
                          midOut.open(QIODevice::WriteOnly) &&
                          midOut.write(midBytes) == midBytes.size(),
                      "could not restore the taking-over song");
                cfgOut.close();
                midOut.close();
                check(VoicegroupSource::deleteVoicegroup(root, name, &error),
                      "could not delete the taken-over voicegroup");
                check(reloadProject(&error), "reloading the project failed");
            }
        }

        // 22e. The reload after a partial commit lists the draft's .mid as
        // an unregistered song (review A/B/D). Loading it from the browser
        // focuses the draft, in place or in a new tab; Delete Song and
        // Register Song refuse it. Registered behind the draft's back (the
        // files written directly — the model still says unregistered), a
        // Discard keeps it and says so.
        SongSession *held = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_rb5"),
                                 QStringLiteral("MUS_DRAFTCHECK_RB5"), &held)) {
            const QString label = QStringLiteral("mus_draftcheck_rb5");
            const QString constant = QStringLiteral("MUS_DRAFTCHECK_RB5");
            const QString midPath = midiDir + label + QStringLiteral(".mid");
            const SongInfo *listed = songNamed(label);
            if (check(listed && m_active == held, "the draft's leftover song is not listed")) {
                const SongInfo stray = *listed;
                const int tabs = m_tabs->count();
                {
                    NoPromptGuard noPrompt(modalFail, __LINE__);
                    loadSong(stray, /*newTab=*/false);
                    check(m_active == held && held->isDraft() && m_tabs->count() == tabs &&
                              held->draft->wroteMidPath == midPath && QFile::exists(midPath),
                          "loading the draft's leftover entry replaced the draft");
                    loadSong(stray, /*newTab=*/true);
                    check(m_active == held && held->isDraft() && m_tabs->count() == tabs,
                          "loading the draft's leftover entry in a new tab did not focus the "
                          "draft");
                }
                QString refusal;
                check(!performSongDeletion(stray, QString(), &refusal) &&
                          refusal == draftDeletionRefusal(label) && QFile::exists(midPath) &&
                          !QFile::exists(root +
                                         QStringLiteral("/.porydaw/trash/%1.mid").arg(label)) &&
                          sessionForLabel(label) == held && held->isDraft(),
                      "Delete Song went ahead on a label an open draft holds");
                {
                    PromptAnswerer dialogs(QMessageBox::Cancel, modalFail);
                    QString shown;
                    dialogs.onDialog(QStringLiteral("Delete Song"), [&shown](QDialog *dialog) {
                        if (auto *box = qobject_cast<QMessageBox *>(dialog))
                            shown = box->text();
                        dialog->reject();
                    });
                    deleteSongById(stray.id);
                    check(shown == draftDeletionRefusal(label) && QFile::exists(midPath),
                          "the Delete Song dialog did not refuse a draft's label up front");
                }
                QString regError;
                int regId = -1;
                check(!registerSongByLabel(label, QString(), QString(), &regId, &regError) &&
                          regError.contains(QStringLiteral("unsaved draft")) &&
                          !SongRegistry::checkRegistration(root, label, constant).inSongTable,
                      "Register Song registered a label an open draft holds");
                // Registered since, behind the model's back.
                check(SongRegistry::registerSong(root, label, constant, player, &error, &regId),
                      "could not register the leftover .mid directly");
                {
                    PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                    QString info;
                    prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
                    closeTab(m_tabs->indexOf(held->view));
                    check(prompt.answered() && !sessionForLabel(label),
                          "the draft over a registered .mid did not close through the prompt");
                    check(info.contains(wroteText + shownPath(QStringLiteral("sound/songs/midi/") +
                                                              label + QStringLiteral(".mid"))) &&
                              info.endsWith(QStringLiteral(
                                  "The project uses it by now, so Discard keeps it.")),
                          "the close prompt does not say the registered .mid stays");
                }
                check(QFile::exists(midPath), "Discard deleted the .mid of a registered song");
                SongRegistry::unregisterSong(root, label, constant, &error);
            }
            QFile::remove(midPath);
            check(reloadProject(&error), "reloading the project failed");
        }
        // 22f. The same with a flags line written since (midi.cfg): kept.
        SongSession *flagged = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_rb6"),
                                 QStringLiteral("MUS_DRAFTCHECK_RB6"), &flagged)) {
            const QString label = QStringLiteral("mus_draftcheck_rb6");
            const QString midPath = midiDir + label + QStringLiteral(".mid");
            check(SongRegistry::writeSongFlags(QDir::cleanPath(midiDir), label,
                                               {QStringLiteral("-E"), QStringLiteral("-V080")},
                                               &error),
                  "could not write the leftover .mid's flags directly");
            {
                PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                closeTab(m_tabs->indexOf(flagged->view));
                check(prompt.answered() && !sessionForLabel(label),
                      "the draft over a flagged .mid did not close through the prompt");
            }
            check(QFile::exists(midPath), "Discard deleted a .mid that has a flags line");
            SongRegistry::removeSongFlags(QDir::cleanPath(midiDir), label, &error);
            QFile::remove(midPath);
            check(reloadProject(&error), "reloading the project failed");
        }
        // 22g. The hub already had a (dangling) include line for the new
        // voicegroup's name (review E): the commit adds none, and Discard
        // removes the .inc but keeps the user's line.
        {
            const QString name = QStringLiteral("mus_draftcheck_rb8");
            const QString vgPath = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            const QByteArray hubOriginal = readBytes(hubPath);
            const QByteArray eol = hubOriginal.contains("\r\n") ? "\r\n" : "\n";
            QByteArray hubPlanted = hubOriginal;
            if (!hubPlanted.isEmpty() && !hubPlanted.endsWith('\n'))
                hubPlanted += eol;
            hubPlanted += ".include \"sound/voicegroups/" + name.toUtf8() + ".inc\"" + eol;
            QFile hubOut(hubPath);
            check(hubOut.open(QIODevice::WriteOnly) &&
                      hubOut.write(hubPlanted) == hubPlanted.size(),
                  "could not plant the dangling include line");
            hubOut.close();
            SongSession *s = nullptr;
            if (partialVgCommit(name, QStringLiteral("MUS_DRAFTCHECK_RB8"), true, &s)) {
                PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                QString info;
                prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
                closeTab(m_tabs->indexOf(s->view));
                check(prompt.answered() && !sessionForLabel(name),
                      "the planted-include draft did not close through the prompt");
                check(info.contains(
                          shownPath(QStringLiteral("sound/voicegroups/%1.inc").arg(name))) &&
                          !info.contains(QStringLiteral("(and its include line)")) &&
                          info.endsWith(QStringLiteral("Discard removes what it wrote.")),
                      "the close prompt claims the user's include line as the save's");
                check(!QFile::exists(vgPath) && readBytes(hubPath) == hubPlanted,
                      "Discard removed an include line the commit did not add (or kept the .inc)");
            }
            check(VoicegroupSource::removeIncludeLine(root, name, &error) &&
                      readBytes(hubPath) == hubOriginal,
                  "could not remove the planted include line");
            check(reloadProject(&error), "reloading the project failed");
        }
        // 22h. Another voicegroup on disk names the partly written one as a
        // drumkit (voice_keysplit_all) or keysplit sub-group (review I):
        // Discard keeps its .inc and include line, and the prompt says so.
        // 22i. Another tab switched to it and undid the switch: its undo
        // history can bring the -G back, so Discard keeps it too.
        const QString keptText =
            QStringLiteral("The project uses %1 by now, so Discard keeps that and removes the "
                           "rest.");
        for (const bool viaHistory : {false, true}) {
            const QString name = viaHistory ? QStringLiteral("mus_draftcheck_rb10")
                                            : QStringLiteral("mus_draftcheck_rb9");
            const QString vgPath = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            SongSession *s = nullptr;
            if (!partialVgCommit(name, name.toUpper(), false, &s))
                continue;
            check(!draftVoicegroupUsedElsewhere(*s, *s->draft, m_sessions, m_project),
                  "the guard called the draft's voicegroup used before anything used it");
            QString referrerPath;
            QByteArray referrerBytes;
            SongSession *o = nullptr;
            if (!viaHistory) {
                // An existing voicegroup file gains a drumkit line naming it.
                QDirIterator vgs(root + QStringLiteral("/sound/voicegroups"),
                                 {QStringLiteral("*.inc")}, QDir::Files);
                while (vgs.hasNext()) {
                    const QString path = vgs.next();
                    if (path != vgPath) {
                        referrerPath = path;
                        break;
                    }
                }
                referrerBytes = readBytes(referrerPath);
                const QByteArray eol = referrerBytes.contains("\r\n") ? "\r\n" : "\n";
                QByteArray planted = referrerBytes;
                if (!planted.isEmpty() && !planted.endsWith('\n'))
                    planted += eol;
                planted += "\tvoice_keysplit_all voicegroup_" + name.toUtf8() + eol;
                QFile out(referrerPath);
                check(!referrerPath.isEmpty() && out.open(QIODevice::WriteOnly) &&
                          out.write(planted) == planted.size(),
                      "could not plant the drumkit reference");
            } else {
                QString other;
                for (const SongInfo &song : m_project.songs()) {
                    if (song.isPlayable() && song.registered && !sessionForLabel(song.label)) {
                        other = song.label;
                        break;
                    }
                }
                loadSongByLabel(other, /*newTab=*/true);
                o = m_active;
                if (check(o && o != s && o->doc.label() == other,
                          "the undoing song did not open in its own tab")) {
                    const QString before = o->doc.cfg().voicegroupArg;
                    SongCfg taken = o->doc.cfg();
                    taken.voicegroupArg = QStringLiteral("_") + name;
                    o->doc.setCfg(taken);
                    o->doc.undoStack()->undo();
                    check(o->doc.cfg().voicegroupArg == before &&
                              o->doc.historyNamesVoicegroupArg(QStringLiteral("_") + name),
                          "the undone switch is not in the other tab's history");
                }
                m_tabs->setCurrentWidget(s->view);
            }
            check(draftVoicegroupUsedElsewhere(*s, *s->draft, m_sessions, m_project),
                  viaHistory ? "the guard missed a -G in another tab's undo history"
                             : "the guard missed a drumkit reference on disk");
            {
                PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                QString info;
                prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
                closeTab(m_tabs->indexOf(s->view));
                check(prompt.answered() && !sessionForLabel(name),
                      "the referenced-voicegroup draft did not close through the prompt");
                check(info.contains(keptText.arg(
                          shownPath(QStringLiteral("sound/voicegroups/%1.inc").arg(name)))),
                      "the close prompt does not say the referenced voicegroup stays");
            }
            check(QFile::exists(vgPath) && includeCount(name) == 1,
                  viaHistory ? "Discard removed a voicegroup another tab's undo could bring back"
                             : "Discard removed a voicegroup another voicegroup references");
            check(!QFile::exists(midiDir + name + QStringLiteral(".mid")),
                  "Discard kept the referenced-voicegroup draft's own .mid");
            // Back to how it was.
            if (!referrerPath.isEmpty()) {
                QFile out(referrerPath);
                check(out.open(QIODevice::WriteOnly) &&
                          out.write(referrerBytes) == referrerBytes.size(),
                      "could not restore the referring voicegroup");
            }
            if (o) {
                PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                closeTab(m_tabs->indexOf(o->view));
            }
            check(VoicegroupSource::deleteVoicegroup(root, name, &error),
                  "could not delete the referenced voicegroup");
            check(reloadProject(&error), "reloading the project failed");
        }
        // 22j. The partial commit's .mid registered behind the draft's back
        // (as 22e), then Save (step 6, A): song_table.inc forces a Rename
        // even with the waiver, and accepting it must not delete the .mid
        // the project now uses. The dialog says it stays.
        SongSession *claimed = nullptr;
        if (!lockCfg()) {
            check(false, "midi.cfg could no longer be made unwritable");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_rb11"),
                                 QStringLiteral("MUS_DRAFTCHECK_RB11"), &claimed)) {
            const QString label = QStringLiteral("mus_draftcheck_rb11");
            const QString constant = QStringLiteral("MUS_DRAFTCHECK_RB11");
            const QString label2 = QStringLiteral("mus_draftcheck_rb11b");
            const QString midPath = midiDir + label + QStringLiteral(".mid");
            int regId = -1;
            check(SongRegistry::registerSong(root, label, constant, player, &error, &regId),
                  "could not register the leftover .mid directly");
            QString explanation;
            {
                PromptAnswerer rename(QMessageBox::Cancel, modalFail);
                rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
                    explanation = renameExplanation(dialog);
                    auto *buttons = dialog->findChild<QDialogButtonBox *>();
                    QPushButton *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
                    QLineEdit *name = nameField(dialog);
                    if (!ok || !name) {
                        dialog->reject();
                        return;
                    }
                    name->setText(label2);
                    if (ok->isEnabled())
                        ok->click();
                    else
                        dialog->reject();
                });
                check(saveSession(*claimed), "saving through the Rename dialog failed");
                check(rename.handled(QStringLiteral("Rename Song")),
                      "a registered leftover .mid did not force a Rename");
            }
            check(explanation.contains(
                      shownPath(QStringLiteral("sound/songs/midi/") + label +
                                QStringLiteral(".mid")) +
                      QStringLiteral(" (the project uses it by now, so a rename keeps it)")),
                  "the Rename dialog does not say the registered .mid stays");
            check(QFile::exists(midPath), "accepting the Rename deleted a .mid the project uses");
            check(!claimed->isDraft() && claimed->doc.label() == label2 &&
                      QFile::exists(midiDir + label2 + QStringLiteral(".mid")),
                  "the renamed draft was not committed under its new name");
            SongRegistry::unregisterSong(root, label, constant, &error);
            QFile::remove(midPath);
            check(reloadProject(&error), "reloading the project failed");
        }
        QFile::setPermissions(cfgPath, cfgPerms);
    }

#ifdef Q_OS_UNIX
    // 22k. A voicegroup save that fails after creating its file (step 6,
    // B): RLIMIT_FSIZE lets save() create the .inc and then refuses the
    // write, leaving a short file. The draft records the file as its own
    // (voicegroupFileCreated), so (1) a plain retry commits over it with no
    // Rename dialog, (2) a Discard removes it, and (3) a Rename (forced by
    // a label registered since) moves the voicegroup and removes the old
    // short file.
    {
        // Saves s with every file write past a few bytes refused, answering
        // the error box (the limit lifted first). True when the save failed
        // as intended: a short .inc on disk, recorded as the draft's own.
        const auto failVgSave = [&](SongSession *s, const QString &name) {
            const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            struct rlimit before;
            getrlimit(RLIMIT_FSIZE, &before);
            const auto restore = [before] { setrlimit(RLIMIT_FSIZE, &before); };
            void (*oldHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
            bool saved = true;
            {
                PromptAnswerer failBox(QMessageBox::Cancel, modalFail);
                failBox.onDialog(QStringLiteral("Save Song"), [&](QDialog *dialog) {
                    restore();
                    dialog->reject();
                });
                struct rlimit tight = before;
                tight.rlim_cur = 16;
                setrlimit(RLIMIT_FSIZE, &tight);
                saved = saveSession(*s);
                restore();
                check(failBox.handled(QStringLiteral("Save Song")),
                      "the failed voicegroup save did not report its error");
            }
            std::signal(SIGXFSZ, oldHandler);
            return check(!saved && s->isDraft() && !s->draft->voicegroupWritten &&
                             s->draft->voicegroupFileCreated && QFile::exists(target) &&
                             QFileInfo(target).size() <= 16 && includeCount(name) == 0 &&
                             s->draft->wroteMidPath.isEmpty() &&
                             !QFile::exists(midiDir + name + QStringLiteral(".mid")),
                         "the failed voicegroup save did not leave a short .inc recorded as the "
                         "draft's own (and nothing else)");
        };
        // (1) Retry.
        {
            const QString name = QStringLiteral("mus_draftcheck_rb12");
            const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            if (check(openDraftSong(draftSmf(), name, name.toUpper(), player, newVgCfg(name), name,
                                    &error),
                      "the short-write draft did not open")) {
                SongSession *s = m_active;
                editVoices(s);
                if (failVgSave(s, name)) {
                    {
                        NoPromptGuard noPrompt(modalFail, __LINE__);
                        check(saveSession(*s), "retrying after a short voicegroup write failed");
                    }
                    const QByteArray written = readBytes(target);
                    check(!s->isDraft() && written == s->vgSource->renderPreview() &&
                              written.contains("voice_square_2") && includeCount(name) == 1,
                          "the retry did not write the whole voicegroup over its short file");
                }
            }
        }
        // (2) Discard.
        {
            const QString name = QStringLiteral("mus_draftcheck_rb13");
            const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            const QByteArray before = treeFingerprint(root, {hubRel});
            if (check(openDraftSong(draftSmf(), name, name.toUpper(), player, newVgCfg(name), name,
                                    &error),
                      "the short-write draft did not open")) {
                SongSession *s = m_active;
                if (failVgSave(s, name)) {
                    PromptAnswerer prompt(QMessageBox::Discard, modalFail);
                    QString info;
                    prompt.inspect([&info](QMessageBox *box) { info = box->informativeText(); });
                    closeTab(m_tabs->indexOf(s->view));
                    check(prompt.answered() && !sessionForLabel(name),
                          "the short-write draft did not close through the prompt");
                    check(info.contains(wroteText) &&
                              info.contains(QDir::toNativeSeparators(
                                  QStringLiteral("sound/voicegroups/%1.inc").arg(name))),
                          "the close prompt does not list the short .inc");
                    check(!QFile::exists(target) && treeFingerprint(root, {hubRel}) == before,
                          "Discard left the short .inc behind");
                }
            }
        }
        // (3) Rename.
        {
            const QString name = QStringLiteral("mus_draftcheck_rb14");
            const QString name2 = QStringLiteral("mus_draftcheck_rb14b");
            const QString target = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name);
            const QString target2 = root + QStringLiteral("/sound/voicegroups/%1.inc").arg(name2);
            if (check(openDraftSong(draftSmf(), name, name.toUpper(), player, newVgCfg(name), name,
                                    &error),
                      "the short-write draft did not open")) {
                SongSession *s = m_active;
                if (failVgSave(s, name)) {
                    int regId = -1;
                    check(SongRegistry::registerSong(root, name, name.toUpper(), player, &error,
                                                     &regId),
                          "could not register the short-write draft's label directly");
                    QString explanation;
                    {
                        PromptAnswerer rename(QMessageBox::Cancel, modalFail);
                        rename.onDialog(QStringLiteral("Rename Song"), [&](QDialog *dialog) {
                            explanation = renameExplanation(dialog);
                            auto *buttons = dialog->findChild<QDialogButtonBox *>();
                            QPushButton *ok =
                                buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
                            QLineEdit *field = nameField(dialog);
                            if (!ok || !field) {
                                dialog->reject();
                                return;
                            }
                            field->setText(name2);
                            if (ok->isEnabled())
                                ok->click();
                            else
                                dialog->reject();
                        });
                        check(saveSession(*s),
                              "saving the short-write draft through Rename failed");
                        check(rename.handled(QStringLiteral("Rename Song")),
                              "a registered label did not force a Rename");
                    }
                    check(explanation.contains(QDir::toNativeSeparators(
                              QStringLiteral("sound/voicegroups/%1.inc (partly)").arg(name))),
                          "the Rename dialog does not list the short .inc");
                    check(!s->isDraft() && s->doc.label() == name2 &&
                              s->doc.cfg().voicegroupArg == QStringLiteral("_") + name2 &&
                              QFile::exists(target2) && includeCount(name2) == 1,
                          "the renamed short-write draft was not committed with its voicegroup");
                    check(!QFile::exists(target) && includeCount(name) == 0,
                          "the Rename left the old short .inc behind");
                    SongRegistry::unregisterSong(root, name, name.toUpper(), &error);
                    check(reloadProject(&error), "reloading the project failed");
                }
            }
        }
    }
#else
    std::printf("draftcheck: note: no RLIMIT_FSIZE here; section 22k skipped\n");
#endif

    // 23. The dock's New Voicegroup (and project.createVoicegroup, through
    // the same helper) refuses a name an open draft's new voicegroup holds,
    // with the wizard's message; nothing is written.
    {
        const QString name = QStringLiteral("mus_draftcheck_res");
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_RES"), player,
                                 newVgCfg(name), name, &error),
                   "the reserving draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        const QByteArray before = treeFingerprint(root);
        QString refusal;
        check(!createVoicegroupNamed(name, QString(), &refusal) &&
                  refusal ==
                      QStringLiteral("A voicegroup named voicegroup_%1 already exists.").arg(name),
              "New Voicegroup took a name an open draft reserves (or not with the wizard's "
              "message)");
        check(treeFingerprint(root) == before, "the refused New Voicegroup wrote");
        PromptAnswerer prompt(QMessageBox::Discard, modalFail);
        closeTab(m_tabs->indexOf(s->view));
    }

    // 24. A project switch with a draft open: Add commits it into the old
    // project before the switch; Discard (no partial commit) writes nothing.
    {
        const QString name = QStringLiteral("mus_draftcheck_sw");
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_SW"), player, cfg,
                                 QString(), &error),
                   "the switch draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        {
            PromptAnswerer prompt(QMessageBox::Save, modalFail);
            check(openProjectDir(root, /*interactive=*/false), "reopening the project failed");
            check(prompt.answered(), "the project switch did not prompt for the draft (Add)");
        }
        const SongInfo *song = songNamed(name);
        check(song && song->registered && QFile::exists(midiDir + name + QStringLiteral(".mid")),
              "Add on a project switch did not commit the draft into the project");
        const QString name2 = QStringLiteral("mus_draftcheck_sw2");
        if (check(openDraftSong(draftSmf(), name2, QStringLiteral("MUS_DRAFTCHECK_SW2"), player,
                                cfg, QString(), &error),
                  "the second switch draft did not open")) {
            const QByteArray before = treeFingerprint(root);
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            check(openProjectDir(root, /*interactive=*/false), "reopening the project failed");
            check(prompt.answered() && !sessionForLabel(name2) && treeFingerprint(root) == before,
                  "Discard on a project switch wrote the draft");
        }
    }

    // 25. Quit with a draft open (last: the window is closed afterwards):
    // Cancel keeps it; Add commits it; Discard of a partly committed draft
    // removes what the failed save wrote.
    {
        const QString name = QStringLiteral("mus_draftcheck_q1");
        if (!check(openDraftSong(draftSmf(), name, QStringLiteral("MUS_DRAFTCHECK_Q1"), player, cfg,
                                 QString(), &error),
                   "the quit draft did not open")) {
            std::fprintf(stderr, "draftcheck: %s\n", qUtf8Printable(error));
            return failures;
        }
        SongSession *s = m_active;
        {
            PromptAnswerer prompt(QMessageBox::Cancel, modalFail);
            check(!close(), "quit went ahead after Cancel");
            check(prompt.answered() && sessionForLabel(name) == s && s->isDraft(),
                  "Cancel on quit did not keep the draft");
        }
        {
            PromptAnswerer prompt(QMessageBox::Save, modalFail);
            check(close(), "quit answered Add did not go ahead");
            check(prompt.answered() && !s->isDraft() && songNamed(name) &&
                      songNamed(name)->registered,
                  "Add on quit did not commit the draft");
        }
        SongSession *partial = nullptr;
        if (!lockCfg()) {
            std::printf("draftcheck: note: midi.cfg stays writable after chmod; the quit "
                        "rollback is skipped\n");
        } else if (partialCommit(QStringLiteral("mus_draftcheck_q2"),
                                 QStringLiteral("MUS_DRAFTCHECK_Q2"), &partial)) {
            PromptAnswerer prompt(QMessageBox::Discard, modalFail);
            check(close(), "quit answered Discard did not go ahead");
            check(prompt.answered() &&
                      !QFile::exists(midiDir + QStringLiteral("mus_draftcheck_q2.mid")),
                  "Discard on quit left the partial commit's .mid");
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
