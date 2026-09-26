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
#include <QFileInfo>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
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
#include <cstdio>
#include <functional>
#include <memory>

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
// no-op. QSettings is
// redirected into a temp dir; the commit writes into the project — run
// against a scratch copy. PORYDAW_DRAFTCHECK_SHOTS=<dir> saves screenshots.

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

// Answers the close/save prompt ("Unsaved Changes", or "Unsaved Draft" for a
// draft, whose Save button reads "Add to Project") with button from inside
// its own exec() loop, for as long as the guard is in scope. A 10 ms poll
// on the active modal widget: any OTHER modal that shows meanwhile (an
// error box, a second prompt) is recorded as a failure and dismissed, so a
// surprise dialog fails the section instead of hanging the harness —
// unless onDialog registered a handler for its title, which then runs once
// and must close it. The guard's destructor stops the poll, so nothing
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
    QSet<QString> m_handled;
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
        check(statusBar()->currentMessage().endsWith(
                  QStringLiteral(" — configure its new voicegroup in the Voicegroup dock")),
              "the commit's status message lacks the new-voicegroup hint");
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
