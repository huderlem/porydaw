#include "mainwindow.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFormLayout>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QUndoGroup>

#include <QCloseEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/sampleimport.h"
#include "audio/sf2reader.h"
#include "audio/wavexport.h"
#include "core/miditimeline.h"
#include "project/samplereg.h"
#include "project/songregistry.h"
#include "ui/newsongwizard.h"
#include "ui/sampleeditordialog.h"
#include "ui/sf2zonepicker.h"
#include "ui/polyphonypanel.h"
#include "ui/songlistpanel.h"
#include "ui/songsettingsdialog.h"
#include "ui/songview.h"
#include "ui/viewsidecar.h"
#include "ui/voicegroupbrowser.h"

#include <QUndoCommand>

namespace {
constexpr int kVoiceEditCommandId = 0x7661; // 'va': voice-edit merge id

const QString kLastOpenSongsKey = QStringLiteral("lastOpenSongs");
const QString kLastSongLabelKey = QStringLiteral("lastSongLabel");
} // namespace

// A voicegroup voice edit, on its tab's undo stack: song and voicegroup
// share one undo/save pipeline, so Ctrl+Z walks both kinds of edit in order.
// Value-based (before/after per slot): it applies through whichever source
// the session has open, which keeps undo/redo correct even after a -G
// voicegroup switch reopened the file from disk (see
// MainWindow::replayVoiceEdits). The session pointer is safe to hold: the
// command lives in that session's own undo stack, so they die together.
class VoiceEditCommand : public QUndoCommand
{
public:
    VoiceEditCommand(MainWindow *window, SongSession *session, const QString &loadName,
                     int slot, const VgVoice &before, const VgVoice &after,
                     bool structural)
        : QUndoCommand(QObject::tr("edit voice %1").arg(slot)), m_window(window),
          m_session(session), m_loadName(loadName), m_slot(slot), m_before(before),
          m_after(after), m_structural(structural)
    {
    }

    const QString &loadName() const { return m_loadName; }
    int slot() const { return m_slot; }
    const VgVoice &after() const { return m_after; }

    int id() const override { return kVoiceEditCommandId; }

    // Spin boxes commit every step of a click-and-hold gesture; steps that
    // keep changing the same fields of the same voice collapse into one undo
    // entry. A run that lands back on its starting value vanishes entirely.
    bool mergeWith(const QUndoCommand *other) override
    {
        auto *o = static_cast<const VoiceEditCommand *>(other);
        if (o->m_loadName != m_loadName || o->m_slot != m_slot || m_structural
            || o->m_structural
            || changedFields(o->m_before, o->m_after)
                != changedFields(m_before, m_after))
            return false;
        m_after = o->m_after;
        if (m_after == m_before)
            setObsolete(true);
        return true;
    }

    void redo() override
    {
        m_window->applyVoiceEdit(*m_session, m_loadName, m_slot, m_after, m_structural);
    }
    void undo() override
    {
        m_window->applyVoiceEdit(*m_session, m_loadName, m_slot, m_before, m_structural);
    }

private:
    static uint changedFields(const VgVoice &a, const VgVoice &b)
    {
        uint mask = 0;
        mask |= uint(a.macro != b.macro) << 0;
        mask |= uint(a.key != b.key) << 1;
        mask |= uint(a.pan != b.pan) << 2;
        mask |= uint(a.symbol != b.symbol) << 3;
        mask |= uint(a.keysplitTable != b.keysplitTable) << 4;
        mask |= uint(a.sweep != b.sweep) << 5;
        mask |= uint(a.duty != b.duty) << 6;
        mask |= uint(a.period != b.period) << 7;
        mask |= uint(a.attack != b.attack) << 8;
        mask |= uint(a.decay != b.decay) << 9;
        mask |= uint(a.sustain != b.sustain) << 10;
        mask |= uint(a.release != b.release) << 11;
        return mask;
    }

    MainWindow *m_window;
    SongSession *m_session;
    QString m_loadName;
    int m_slot;
    VgVoice m_before;
    VgVoice m_after;
    bool m_structural;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("porydaw"));
    resize(1100, 680);
    m_engineSettings = EngineSettings::load();
    buildUi();

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("windowGeometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
    m_songList->restoreFilters(
        settings.value(QStringLiteral("songFilterText")).toString(),
        settings.value(QStringLiteral("songFilterSort")).toInt(),
        settings.value(QStringLiteral("songFilterCategory")).toString());

    QString audioError;
    m_audioOk = m_audio.init(&audioError);
    if (!m_audioOk) {
        QMessageBox::warning(this, tr("Audio Error"),
                             tr("%1\n\nPlayback will be unavailable.").arg(audioError));
    }
    statusBar()->showMessage(
        m_audioOk ? tr("Audio ready (%1 Hz). Open a decomp project to get started.")
                        .arg(int(m_audio.sampleRate()))
                  : tr("No audio device."));

    m_uiTimer = new QTimer(this);
    m_playheadTimer = new QTimer(this);

    m_uiTimer->setInterval(33);
    connect(m_uiTimer, &QTimer::timeout, this, &MainWindow::uiTick);
    m_uiTimer->start();
    m_playheadTimer->setTimerType(Qt::PreciseTimer);
    // 60hz is 16.6 ms; Qt can tick faster than this, making the playhead move
    // faster than 60hz and wasting time.
    m_playheadTimer->setInterval(17);
    connect(m_playheadTimer, &QTimer::timeout, this, &MainWindow::synchronizePlayhead);

    updateTransportActions();
}

MainWindow::~MainWindow()
{
    // The sessions delete their views below; the tab widget notices the
    // pages vanishing and would fire currentChanged into handlers that walk
    // the half-destroyed session list.
    if (m_tabs)
        disconnect(m_tabs, nullptr, this, nullptr);
    // Stop the audio thread before the sessions free the timeline and
    // voicegroup it borrows.
    m_audio.shutdown();
}

void MainWindow::buildUi()
{
    // Menu
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *openAction = fileMenu->addAction(tr("&Open Project..."), this,
                                              &MainWindow::openProject);
    openAction->setShortcut(QKeySequence::Open);
    m_newSongAction = fileMenu->addAction(tr("&New Song..."), this, &MainWindow::newSong);
    m_newSongAction->setShortcut(QKeySequence::New);
    m_newSongAction->setEnabled(false);
    m_importAction =
        fileMenu->addAction(tr("&Import MIDI..."), this, &MainWindow::importMidi);
    m_importAction->setEnabled(false);
    m_saveAction = fileMenu->addAction(tr("&Save Song"), this, &MainWindow::saveSong);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setEnabled(false);
    m_registerAction = fileMenu->addAction(tr("Re&gister Song"), this,
                                           &MainWindow::registerLoadedSong);
    m_registerAction->setEnabled(false);
    m_closeTabAction = fileMenu->addAction(tr("&Close Tab"), this, [this] {
        if (m_tabs->currentIndex() >= 0)
            closeTab(m_tabs->currentIndex());
    });
    m_closeTabAction->setShortcut(QKeySequence::Close);
    m_closeTabAction->setEnabled(false);
    fileMenu->addSeparator();
    m_exportWavAction = fileMenu->addAction(tr("Export &WAV..."), this,
                                            &MainWindow::exportWav);
    m_exportWavAction->setEnabled(false);
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

    // Edit menu: undo/redo route to the active tab's stack.
    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    m_undoGroup = new QUndoGroup(this);
    QAction *undoAction = m_undoGroup->createUndoAction(this, tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undoAction);
    QAction *redoAction = m_undoGroup->createRedoAction(this, tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    m_settingsAction = editMenu->addAction(tr("Song Se&ttings..."), this,
                                           &MainWindow::openSongSettings);
    m_settingsAction->setEnabled(false);
    // Global GBA-accuracy knobs (SPEC §7); not song-scoped, so always enabled.
    editMenu->addAction(tr("&Engine Settings..."), this,
                        &MainWindow::openEngineSettings);

    // View menu: piano roll vs raw MIDI event list, per tab.
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    m_eventListAction = viewMenu->addAction(tr("MIDI &Event List"));
    m_eventListAction->setCheckable(true);
    m_eventListAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    m_eventListAction->setEnabled(false);
    connect(m_eventListAction, &QAction::toggled, this, [this](bool on) {
        if (m_active)
            m_active->view->setEventListVisible(on);
    });

    // Tools menu: project-level utilities that aren't song-scoped.
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    m_importSampleAction = toolsMenu->addAction(tr("Import &Sample..."), this,
                                                &MainWindow::importSample);
    m_importSampleAction->setEnabled(false);

    // Transport toolbar
    QToolBar *transport = addToolBar(tr("Transport"));
    // saveState() persists dock/toolbar layout by objectName only.
    transport->setObjectName(QStringLiteral("transportToolbar"));
    transport->setMovable(false);

    m_goToStartAction = new QAction(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                    tr("Go to Start"), this);
    m_goToStartAction->setShortcut(Qt::Key_Home);
    connect(m_goToStartAction, &QAction::triggered, this, [this] {
        if (m_active)
            m_active->view->goToStart();
    });
    transport->addAction(m_goToStartAction);

    m_playAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), this);
    connect(m_playAction, &QAction::triggered, this, [this] { startPlayback(); });
    transport->addAction(m_playAction);

    // Space toggles play/pause (Reaper-style); a window-level action so it
    // works wherever focus is, like the old Space=Play binding did. Starting
    // (or unpausing) with Space always restarts from the edit cursor; the
    // Play button is the resume-from-pause path.
    m_playPauseAction = new QAction(tr("Play/Pause"), this);
    m_playPauseAction->setShortcut(Qt::Key_Space);
    connect(m_playPauseAction, &QAction::triggered, this, [this] {
        if (m_audio.transport() == Transport::Playing)
            pausePlayback();
        else
            startPlayback(/*fromEditCursor=*/true);
    });
    addAction(m_playPauseAction);

    m_pauseAction = new QAction(style()->standardIcon(QStyle::SP_MediaPause), tr("Pause"), this);
    connect(m_pauseAction, &QAction::triggered, this, [this] { pausePlayback(); });
    transport->addAction(m_pauseAction);

    m_stopAction = new QAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"), this);
    connect(m_stopAction, &QAction::triggered, this, [this] { stopPlayback(); });
    transport->addAction(m_stopAction);

    m_loopAction = new QAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Loop"), this);
    m_loopAction->setCheckable(true);
    m_loopAction->setChecked(true);
    connect(m_loopAction, &QAction::toggled, this,
            [this](bool on) { m_audio.setLoopEnabled(on); });
    transport->addAction(m_loopAction);

    transport->addSeparator();
    m_timeLabel = new QLabel(QStringLiteral("--:--.- / --:--.-"), this);
    m_timeLabel->setContentsMargins(8, 0, 8, 0);
    transport->addWidget(m_timeLabel);
    m_songLabel = new QLabel(this);
    transport->addWidget(m_songLabel);

    // Song browser dock
    auto *dock = new QDockWidget(tr("Songs"), this);
    dock->setObjectName(QStringLiteral("songsDock"));
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    m_songList = new SongListPanel(dock);
    connect(m_songList, &SongListPanel::songActivated, this, &MainWindow::songActivated);
    connect(m_songList, &SongListPanel::songOpenInNewTabRequested, this,
            &MainWindow::songOpenInNewTab);
    dock->setWidget(m_songList);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    auto *findAction = new QAction(tr("Find Song"), this);
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this,
            [this] { m_songList->focusSearch(); });
    addAction(findAction);

    // Voicegroup browser dock (SPEC §6.1): the active tab's instruments,
    // click-and-hold to audition through the preview engine instance, with
    // an editor panel for the selected voice.
    m_vgDock = new QDockWidget(tr("Voicegroup"), this);
    m_vgDock->setObjectName(QStringLiteral("voicegroupDock"));
    m_vgDock->setFeatures(QDockWidget::DockWidgetMovable);
    m_vgBrowser = new VoicegroupBrowser(m_vgDock);
    connect(m_vgBrowser, &VoicegroupBrowser::auditionVoice, this,
            [this](int voice, int key, int velocity) {
                if (m_audioOk)
                    m_audio.previewVoice(uint8_t(voice), uint8_t(key), uint8_t(velocity));
            });
    connect(m_vgBrowser, &VoicegroupBrowser::voiceEditRequested, this,
            &MainWindow::onVoiceEditRequested);
    connect(m_vgBrowser, &VoicegroupBrowser::newVoicegroupRequested, this,
            &MainWindow::newVoicegroup);
    connect(m_vgBrowser, &VoicegroupBrowser::newSampleRequested, this,
            &MainWindow::importSampleForSlot);
    connect(m_vgBrowser, &VoicegroupBrowser::editSampleRequested, this,
            &MainWindow::editSampleForSlot);
    // The dock's voicegroup selector: same undoable cfg edit as Song
    // Settings; onDocumentChanged does the actual swap (and, on a
    // not-found arg, keeps the old voicegroup with a status message).
    connect(m_vgBrowser, &VoicegroupBrowser::voicegroupChangeRequested, this,
            [this](const QString &arg) {
                if (!m_active)
                    return;
                SongCfg cfg = m_active->doc.cfg();
                // "_dummy" IS the empty arg's meaning; don't write it out.
                if (arg == cfg.voicegroupArg
                    || (cfg.voicegroupArg.isEmpty()
                        && arg == QLatin1String("_dummy")))
                    return;
                cfg.voicegroupArg = arg;
                m_active->doc.setCfg(cfg);
            });
    m_vgDock->setWidget(m_vgBrowser);
    addDockWidget(Qt::LeftDockWidgetArea, m_vgDock);

    // Polyphony overflow debugger dock (SPEC §6.1): hidden by default (it's
    // a diagnostic tool); closable, with a View-menu toggle. The saved
    // window state restores visibility/placement on later runs.
    m_polyDock = new QDockWidget(tr("Polyphony"), this);
    m_polyDock->setObjectName(QStringLiteral("polyphonyDock"));
    m_polyDock->setFeatures(QDockWidget::DockWidgetMovable
                            | QDockWidget::DockWidgetClosable);
    m_polyPanel = new PolyphonyPanel(m_polyDock);
    connect(m_polyPanel, &PolyphonyPanel::invertToggled, this, [this](bool on) {
        if (m_audioOk)
            m_audio.setPolyDebugInvert(on);
    });
    connect(m_polyPanel, &PolyphonyPanel::resetRequested, this, [this] {
        if (m_audioOk)
            m_audio.resetPolyStats();
    });
    connect(m_polyPanel, &PolyphonyPanel::jumpToEvent, this,
            [this](uint64_t tick, int track, int midiKey) {
                // commitEditCursor's editCursorMoved connect already seeks
                // the engine while playing/paused; when stopped, playback
                // starts from the edit cursor. revealNote selects the losing
                // track and the lost note itself.
                if (!m_active)
                    return;
                m_active->view->revealNote(track, uint8_t(midiKey), tick);
                m_active->view->commitEditCursor(tick);
                m_active->view->ensureTickVisible(tick);
            });
    m_polyDock->setWidget(m_polyPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_polyDock);
    m_polyDock->hide();
    QAction *polyDockAction = m_polyDock->toggleViewAction();
    polyDockAction->setText(tr("&Polyphony Debugger"));
    polyDockAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    viewMenu->addAction(polyDockAction);

    // Song tabs: each open song lives in its own tab with its own view,
    // document, and undo stack.
    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::tabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this,
            [this](int, int) { persistOpenTabs(); });
    setCentralWidget(m_tabs);

    // Status bar: polyphony meter
    m_polyLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_polyLabel);

    // Initial focus goes to the song list (via the panel's focus proxy), not
    // its filter box — first in tab order, which otherwise wins on show and
    // swallowed the first keystrokes into the search field.
    m_songList->setFocus();
}

SongSession *MainWindow::sessionForWidget(QWidget *widget) const
{
    for (const auto &session : m_sessions) {
        if (session->view == widget)
            return session.get();
    }
    return nullptr;
}

SongSession *MainWindow::sessionForLabel(const QString &label) const
{
    for (const auto &session : m_sessions) {
        if (session->doc.label() == label)
            return session.get();
    }
    return nullptr;
}

SongSession *MainWindow::createSession()
{
    auto owned = std::make_unique<SongSession>();
    SongSession *s = owned.get();
    s->view = new SongView;
    connect(s->view, &SongView::muteMaskChanged, this, [this, s](uint32_t mask) {
        if (s == m_active)
            m_audio.setMuteMask(mask);
    });
    connect(s->view, &SongView::soloMaskChanged, this, [this, s](uint32_t mask) {
        if (s == m_active)
            m_audio.setSoloMask(mask);
    });
    connect(s->view, &SongView::auditionNote, this,
            [this, s](int track, int key, int velocity) {
                if (s == m_active && m_audioOk && m_audio.songLoaded())
                    m_audio.previewNote(uint8_t(track), uint8_t(key), uint8_t(velocity));
            });
    connect(s->view, &SongView::auditionNoteTimed, this,
            [this, s](int track, int key, int velocity, quint32 durationSamples) {
                if (s == m_active && m_audioOk && m_audio.songLoaded())
                    m_audio.previewNoteTimed(uint8_t(track), uint8_t(key),
                                             uint8_t(velocity), durationSamples);
            });
    connect(s->view, &SongView::auditionVoice, this,
            [this, s](int voice, int key, int velocity) {
                if (s == m_active && m_audioOk)
                    m_audio.previewVoice(uint8_t(voice), uint8_t(key), uint8_t(velocity));
            });
    connect(s->view, &SongView::statusMessage, this,
            [this](const QString &text) { statusBar()->showMessage(text, 6000); });
    // Jump-from-context voice navigation (header voice line, event list):
    // raise the dock and select the slot. No keyboard focus moves — Space
    // and the roll's shortcuts keep working.
    connect(s->view, &SongView::revealVoiceRequested, this, [this, s](int program) {
        if (s != m_active)
            return;
        m_vgDock->show();
        m_vgDock->raise();
        m_vgBrowser->revealSlot(program);
    });
    // A sidecar restore (applyViewState) can flip the view under the menu.
    connect(s->view, &SongView::eventListVisibilityChanged, this, [this, s](bool on) {
        if (s == m_active) {
            QSignalBlocker blocker(m_eventListAction);
            m_eventListAction->setChecked(on);
        }
    });
    // Moving the edit cursor while playing (or paused) seeks playback there,
    // chasing controller state to the landing position.
    connect(s->view, &SongView::editCursorMoved, this, [this, s](uint64_t tick) {
        if (s == m_active && m_audioOk && m_audio.songLoaded()
            && m_audio.transport() != Transport::Stopped) {
            m_audio.seek(m_audio.timeline()->sampleForTick(tick));
            synchronizePlayhead();
        }
    });
    connect(&s->doc, &SongDocument::documentChanged, this,
            [this, s] { onDocumentChanged(*s); });
    connect(s->doc.undoStack(), &QUndoStack::cleanChanged, this, [this, s](bool) {
        updateTabTitle(*s);
        if (s == m_active)
            updateWindowTitle();
    });
    m_undoGroup->addStack(s->doc.undoStack());
    m_sessions.push_back(std::move(owned));
    return s;
}

void MainWindow::destroySession(SongSession *session)
{
    const int index = m_tabs->indexOf(session->view);
    if (index >= 0)
        m_tabs->removeTab(index); // fires currentChanged → activateSession
    // Removing the current tab re-activated a neighbor (rebinding the audio
    // engine) through currentChanged; this only fires if that didn't happen.
    if (m_active == session)
        activateSession(nullptr);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it->get() == session) {
            m_sessions.erase(it);
            break;
        }
    }
}

bool MainWindow::promptToSaveAllSessions()
{
    for (const auto &session : m_sessions) {
        if (!maybeSaveSession(*session))
            return false;
    }
    return true;
}

void MainWindow::teardownSessions()
{
    m_tearingDown = true;
    // One engine/dock detach up front; the guarded currentChanged handler
    // then lets the tabs vanish without rebinding each doomed neighbor.
    activateSession(nullptr, /*force=*/true);
    while (!m_sessions.empty())
        destroySession(m_sessions.back().get());
    m_tearingDown = false;
}

void MainWindow::activateSession(SongSession *session, bool force)
{
    if (session == m_active && !force)
        return;
    const SongSession *previous = m_active;
    // Switching tabs stops playback in the tab being left.
    if (m_audioOk)
        m_audio.stop();
    m_active = session;
    m_undoGroup->setActiveStack(session ? session->doc.undoStack() : nullptr);

    const bool loaded = session != nullptr;
    m_saveAction->setEnabled(loaded);
    m_exportWavAction->setEnabled(loaded);
    m_settingsAction->setEnabled(loaded);
    m_closeTabAction->setEnabled(loaded);
    m_eventListAction->setEnabled(loaded);
    {
        // Reflect the incoming tab's roll/event-list state without the
        // checkbox driving a redundant toggle.
        QSignalBlocker blocker(m_eventListAction);
        m_eventListAction->setChecked(session && session->view->eventListVisible());
    }

    if (!session) {
        if (m_audioOk)
            m_audio.unloadSong();
        m_polyPanel->clearSession();
        m_vgBrowser->setVoicegroup(nullptr);
        updateVgDockTitle();
        m_registerAction->setEnabled(false);
        m_songLabel->clear();
        m_timeLabel->setText(QStringLiteral("--:--.- / --:--.-"));
        m_polyLabel->clear();
        m_songList->setCurrentSong(-1);
        updateWindowTitle();
        updateTransportActions();
        persistOpenTabs();
        synchronizePlayhead();
        return;
    }

    // Only on a genuine switch: while re-binding the same session (force),
    // the engine may still borrow this session's voicegroup, which the
    // refresh would free.
    if (session != previous)
        maybeRefreshVoicegroup(*session);
    if (m_audioOk)
        attachEngine(*session);
    synchronizePlayhead();
    updateVoicegroupBrowser();
    updatePolyPanelContext(session);

    bool registered = true;
    if (session->songId >= 0 && session->songId < m_project.songs().size())
        registered = m_project.songs().at(session->songId).registered;
    m_registerAction->setEnabled(!registered);
    m_songLabel->setText(QStringLiteral("  %1").arg(session->doc.label()));
    m_songList->setCurrentSong(session->songId);
    updateWindowTitle();
    updateTransportActions();
    persistOpenTabs();
}

void MainWindow::attachEngine(SongSession &session)
{
    m_audio.loadSong(session.timeline.get(), session.voicegroup,
                     songSettingsFor(session));
    // loadSong resets the engine's masks; the view remembers the tab's.
    m_audio.setMuteMask(session.view->muteMask());
    m_audio.setSoloMask(session.view->soloMask());
}

void MainWindow::updatePolyPanelContext(SongSession *session)
{
    if (!session) {
        m_polyPanel->clearSession();
        return;
    }
    m_polyPanel->setTimeline(session->timeline.get());
    QStringList trackNames;
    for (int t = 0; t < MAX_TRACKS; t++)
        trackNames.append(session->doc.trackName(t));
    m_polyPanel->setTrackNames(trackNames);
    // Copied out so a voicegroup swap can't leave the panel with a dangling
    // pointer.
    QStringList voiceNames;
    if (session->voicegroup) {
        for (int v = 0; v < VOICEGROUP_SIZE; v++)
            voiceNames.append(
                QString::fromLatin1(session->voicegroup->voiceNames[v]));
    }
    m_polyPanel->setVoiceNames(voiceNames);
}

void MainWindow::maybeRefreshVoicegroup(SongSession &session)
{
    // Another tab may have saved this session's voicegroup since it was
    // opened; a clean session silently follows the disk. A dirty one keeps
    // its unsaved edits — last save wins, as with any two-editor overlap.
    if (!session.vgSource || session.vgSource->dirty())
        return;
    const QDateTime onDisk = QFileInfo(session.vgSource->filePath()).lastModified();
    if (!onDisk.isValid() || onDisk == session.vgFileTime)
        return;
    QString tried;
    LoadedVoiceGroup *vg = loadVoicegroupFor(session.doc.cfg(), &tried);
    if (!vg)
        return; // keep the previous sound
    session.view->setVoicegroup(nullptr);
    if (session.voicegroup)
        voicegroup_free(session.voicegroup);
    session.voicegroup = vg;
    session.view->setVoicegroup(vg);
    // Fresh parse of the saved file (also re-records its mtime).
    openVoicegroupSource(session, session.doc.cfg());
}

void MainWindow::refreshSessionsAfterVgSave(const QString &filePath, SongSession *except)
{
    for (const auto &owned : m_sessions) {
        SongSession *session = owned.get();
        if (session == except || !session->vgSource
            || session->vgSource->filePath() != filePath)
            continue;
        if (session->vgSource->dirty())
            continue; // unsaved edits stay; last save wins, as documented
        QString tried;
        LoadedVoiceGroup *vg = loadVoicegroupFor(session->doc.cfg(), &tried);
        if (!vg)
            continue; // keep the previous sound
        const int keepSlot =
            session == m_active ? m_vgBrowser->currentSlot() : 0;
        swapVoicegroup(*session, vg, keepSlot);
        // Fresh parse of the saved file (also re-records its mtime), then
        // re-point the browser at it — swapVoicegroup bound the old one.
        openVoicegroupSource(*session, session->doc.cfg());
        if (session == m_active)
            updateVoicegroupBrowser();
    }
}

void MainWindow::persistOpenTabs()
{
    // Never persist mid-teardown: during a project switch the settings
    // already point at the NEW project, and the dying tabs' labels would
    // be recorded against it if a crash landed in this window.
    if (m_tearingDown || !m_persistSession || !m_project.isOpen())
        return;
    QSettings settings;
    QStringList labels;
    for (int i = 0; i < m_tabs->count(); i++) {
        if (SongSession *s = sessionForWidget(m_tabs->widget(i)))
            labels << s->doc.label();
    }
    if (labels.isEmpty()) {
        settings.remove(kLastOpenSongsKey);
        settings.remove(kLastSongLabelKey);
        return;
    }
    settings.setValue(kLastOpenSongsKey, labels);
    settings.setValue(kLastSongLabelKey,
                      m_active ? m_active->doc.label() : labels.first());
}

void MainWindow::refreshSessionSongIds()
{
    for (const auto &session : m_sessions) {
        session->songId = -1;
        for (const SongInfo &song : m_project.songs()) {
            if (song.label == session->doc.label()) {
                session->songId = song.id;
                break;
            }
        }
    }
    if (m_active)
        m_songList->setCurrentSong(m_active->songId);
}

void MainWindow::updateTabTitle(SongSession &session)
{
    const int index = m_tabs->indexOf(session.view);
    if (index < 0)
        return;
    m_tabs->setTabText(index, session.isDirty()
                                  ? session.doc.label() + QLatin1Char('*')
                                  : session.doc.label());
}

void MainWindow::tabChanged(int index)
{
    if (m_tearingDown || m_restoringSession)
        return;
    activateSession(index >= 0 ? sessionForWidget(m_tabs->widget(index)) : nullptr);
}

void MainWindow::closeTab(int index)
{
    SongSession *session = sessionForWidget(m_tabs->widget(index));
    if (!session)
        return;
    if (!maybeSaveSession(*session))
        return;
    saveViewState(*session);
    destroySession(session);
    persistOpenTabs();
}

void MainWindow::openProject()
{
    QSettings settings;
    const QString startDir =
        settings.value(QStringLiteral("lastProjectDir"), QDir::homePath()).toString();
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Open Decomp Project"), startDir);
    if (dir.isEmpty())
        return;
    openProjectDir(dir);
}

void MainWindow::restoreSession()
{
    QSettings settings;
    const QString dir = settings.value(QStringLiteral("lastProjectDir")).toString();
    if (dir.isEmpty() || !QDir(dir).exists())
        return;
    // Read before openProjectDir clears them for the fresh project.
    QStringList labels = settings.value(kLastOpenSongsKey).toStringList();
    const QString activeLabel = settings.value(kLastSongLabelKey).toString();
    if (labels.isEmpty() && !activeLabel.isEmpty())
        labels << activeLabel; // session recorded before tabs existed
    if (!openProjectDir(dir, /*interactive=*/false))
        return;
    // Load the tabs without activating each in turn — the per-activation
    // work (engine rebind, voicegroup-dock rebuild, tab persistence) would
    // run N times with all but the last discarded. One activation at the
    // end, for the remembered active tab.
    m_restoringSession = true;
    for (const QString &label : labels)
        loadSongByLabel(label, /*newTab=*/true);
    m_restoringSession = false;
    SongSession *toActivate = sessionForLabel(activeLabel);
    if (!toActivate && m_tabs->count() > 0)
        toActivate = sessionForWidget(m_tabs->currentWidget());
    if (toActivate) {
        m_tabs->setCurrentWidget(toActivate->view);
        if (m_active != toActivate)
            activateSession(toActivate);
    }
}

bool MainWindow::openProjectDir(const QString &dir, bool interactive)
{
    QElapsedTimer timer;
    timer.start();

    // Every prompt before the project (or any tab) changes: a Cancel
    // aborts the switch, though Saves answered before it have already
    // written — standard save-all behavior, not a transaction.
    if (!promptToSaveAllSessions())
        return false;
    for (const auto &session : m_sessions)
        saveViewState(*session); // against the old project root
    cleanupVgPreview();

    QString error;
    if (!m_project.open(dir, &error)) {
        if (interactive)
            QMessageBox::warning(this, tr("Open Project"), error);
        else
            statusBar()->showMessage(
                tr("Couldn't reopen last project %1: %2").arg(dir, error));
        return false;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("lastProjectDir"), dir);
    // The new project starts with no tabs; loadSong re-records them.
    settings.remove(kLastSongLabelKey);
    settings.remove(kLastOpenSongsKey);

    // Sessions were prompted above; closing them now needs no questions.
    teardownSessions();
    invalidateVgCatalog();
    m_pendingSynths.clear(); // unsaved synth definitions die with the project

    m_newSongAction->setEnabled(true);
    m_importAction->setEnabled(true);
    m_importSampleAction->setEnabled(true);
    updateWindowTitle();
    populateSongList();
    m_songList->setCurrentSong(-1);

    int playable = 0;
    for (const SongInfo &song : m_project.songs()) {
        if (song.isPlayable())
            playable++;
    }
    statusBar()->showMessage(tr("Opened %1 — %2 songs, %3 with MIDI sources (%4 ms)")
                                 .arg(QDir(dir).dirName())
                                 .arg(m_project.songs().size())
                                 .arg(playable)
                                 .arg(timer.elapsed()));
    updateTransportActions();
    return true;
}

void MainWindow::populateSongList()
{
    m_songList->setSongs(m_project.songs());
}

void MainWindow::songActivated(int songId)
{
    if (songId < 0 || songId >= m_project.songs().size())
        return;
    loadSong(m_project.songs().at(songId));
}

void MainWindow::songOpenInNewTab(int songId)
{
    if (songId < 0 || songId >= m_project.songs().size())
        return;
    loadSong(m_project.songs().at(songId), /*newTab=*/true);
}

LoadedVoiceGroup *MainWindow::loadVoicegroupFor(const SongCfg &cfg, QString *tried)
{
    const QStringList candidates = DecompProject::voicegroupCandidates(cfg);
    if (tried)
        *tried = candidates.join(QStringLiteral(", "));
    const QByteArray rootUtf8 = m_project.root().toLocal8Bit();
    for (const QString &name : candidates) {
        LoadedVoiceGroup *vg =
            voicegroup_load(rootUtf8.constData(), name.toLocal8Bit().constData(), nullptr);
        if (vg)
            return vg;
    }
    return nullptr;
}

void MainWindow::loadSong(const SongInfo &song, bool newTab)
{
    if (!m_audioOk)
        return;
    // Already open somewhere? Focus that tab: two documents over one .mid
    // would fight over the file on save. Except the song already in the
    // CURRENT tab — re-activating it falls through to an in-place reload
    // from disk (the pre-tabs behavior, and the only reload path for a
    // .mid changed externally).
    if (SongSession *open = sessionForLabel(song.label)) {
        if (newTab || open != m_active) {
            m_tabs->setCurrentWidget(open->view);
            return;
        }
    }

    const bool created = newTab || !m_active;
    SongSession *session = created ? nullptr : m_active;
    if (session) {
        if (!maybeSaveSession(*session))
            return;
        saveViewState(*session); // the outgoing song's, while its view is up
    }
    cleanupVgPreview();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();

    QString tried;
    LoadedVoiceGroup *vg = loadVoicegroupFor(song.cfg, &tried);
    if (!vg) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Load Song"),
                             tr("Could not load the voicegroup for %1 (tried: %2).")
                                 .arg(song.label, tried));
        return;
    }

    if (!session)
        session = createSession();
    QString error;
    if (!session->doc.load(song, &error)) {
        voicegroup_free(vg);
        if (created)
            destroySession(session); // not in the tab bar yet
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Load Song"), error);
        return;
    }

    auto timeline = session->doc.buildTimeline(m_audio.sampleRate());

    // The view (and, when replacing the active tab in place, the engine)
    // must let go of the old timeline/voicegroup before they are freed.
    session->view->setDocument(nullptr);
    session->view->setSong(nullptr, nullptr);
    if (session == m_active) {
        m_vgBrowser->setVoicegroup(nullptr);
        m_audio.unloadSong();
    }
    if (session->voicegroup)
        voicegroup_free(session->voicegroup);
    session->voicegroup = vg;
    session->timeline = std::move(timeline);
    openVoicegroupSource(*session, song.cfg);
    session->songId = song.id;
    session->appliedVoicegroupArg = song.cfg.voicegroupArg;
    session->appliedVolume = song.cfg.masterVolume;
    session->appliedReverb = song.cfg.reverb;

    session->view->setSong(session->timeline.get(), session->voicegroup);
    session->view->setDocument(&session->doc);
    SongView::ViewState viewState;
    if (ViewSidecar::load(m_project.root(), song.label, &viewState))
        session->view->applyViewState(viewState);

    if (created) {
        const int index = m_tabs->addTab(session->view, song.label);
        m_tabs->setTabToolTip(index, song.midPath);
        if (!m_restoringSession) {
            // The first tab activates inside addTab; later ones here.
            m_tabs->setCurrentIndex(index);
            if (m_active != session)
                activateSession(session);
        }
    } else {
        const int index = m_tabs->indexOf(session->view);
        m_tabs->setTabText(index, song.label);
        m_tabs->setTabToolTip(index, song.midPath);
        activateSession(session, /*force=*/true);
    }

    const MidiTimeline *tl = session->timeline.get();
    QString loopNote = tl->hasLoop() ? tr(", loops") : tr(", no loop markers");
    QString dropNote;
    if (tl->droppedTracks > 0)
        dropNote = tr(", %1 tracks beyond 16 ignored").arg(tl->droppedTracks);
    statusBar()->showMessage(tr("Loaded %1 in %2 ms — %3 events%4%5")
                                 .arg(song.label)
                                 .arg(timer.elapsed())
                                 .arg(tl->events.size())
                                 .arg(loopNote, dropNote));

    QApplication::restoreOverrideCursor();
    updateTransportActions();
}

void MainWindow::onDocumentChanged(SongSession &session)
{
    if (!m_audioOk)
        return;
    const bool active = &session == m_active;

    const SongCfg &cfg = session.doc.cfg();
    if (cfg.voicegroupArg != session.appliedVoicegroupArg) {
        // The -G switch (or its undo/redo) swaps the voicegroup. Unsaved
        // voice edits need no prompt: they live in the undo history, and
        // replayVoiceEdits restores them whenever their voicegroup is the
        // open one again.
        cleanupVgPreview();
        QString tried;
        if (LoadedVoiceGroup *vg = loadVoicegroupFor(cfg, &tried)) {
            session.view->setVoicegroup(nullptr);
            if (active) {
                m_vgBrowser->setVoicegroup(nullptr);
                m_audio.updateVoicegroup(vg);
            }
            if (session.voicegroup)
                voicegroup_free(session.voicegroup);
            session.voicegroup = vg;
            openVoicegroupSource(session, cfg);
            replayVoiceEdits(session);
            session.view->setVoicegroup(session.voicegroup);
            if (active)
                updateVoicegroupBrowser();
            session.appliedVoicegroupArg = cfg.voicegroupArg;
            // Replayed edits aren't in the disk file just loaded; audition
            // them through the preview shadow like any unsaved edit.
            if (session.vgSource && session.vgSource->dirty())
                reloadVoicegroupPreview(session,
                                        active ? m_vgBrowser->currentSlot() : 0);
        } else {
            statusBar()->showMessage(
                tr("Voicegroup not found (tried: %1) — keeping the previous one until save.")
                    .arg(tried),
                8000);
        }
    }
    if (cfg.masterVolume != session.appliedVolume
        || cfg.reverb != session.appliedReverb) {
        if (active && m_audio.songLoaded())
            m_audio.updateSettings(songSettingsFor(session));
        session.appliedVolume = cfg.masterVolume;
        session.appliedReverb = cfg.reverb;
    }

    auto timeline = session.doc.buildTimeline(m_audio.sampleRate());
    if (active && m_audio.songLoaded())
        m_audio.updateTimeline(timeline.get());
    // The old timeline is freed here — after the engine let go of it.
    session.timeline = std::move(timeline);
    session.view->updateSong(session.timeline.get());
    updateTabTitle(session);
    if (active) {
        // The timeline was rebuilt, track names may have changed, and the
        // voicegroup may have been swapped above.
        updatePolyPanelContext(&session);
        updateWindowTitle();
        // Keep the dock's voicegroup selector on the cfg even when no swap
        // ran (the arg's voicegroup wasn't found, or its change was undone).
        m_vgBrowser->setCurrentVoicegroupArg(cfg.voicegroupArg.isEmpty()
                                                 ? QStringLiteral("_dummy")
                                                 : cfg.voicegroupArg);
        // Program changes may have been added/removed; refresh the dock's
        // used-voice marks (no-op when the set is unchanged).
        m_vgBrowser->setUsedVoices(session.view->usedVoices());
    }
}

void MainWindow::saveSong()
{
    if (m_active)
        saveSession(*m_active);
}

bool MainWindow::saveSession(SongSession &session)
{
    if (session.doc.midPath().isEmpty())
        return false;

    // The voicegroup first: the document save below marks the undo stack
    // clean, and a failed voicegroup write must leave the session dirty so
    // the user can retry.
    const bool vgWasDirty = session.vgSource && session.vgSource->dirty();
    if (vgWasDirty) {
        QString error;
        // Golden Sun synth definitions this voicegroup references that only
        // exist in memory (minted by param edits) must land on disk first —
        // the saved file's symbols have to resolve. Only what the SAVED
        // state references is written; abandoned tweaks never persist.
        QList<QPair<QString, VgSynthDesc>> newDefs;
        for (int slot = 0; slot < VOICEGROUP_SIZE; slot++) {
            const VgVoice *v = session.vgSource->voiceAt(slot);
            if (!v)
                continue;
            const auto it = m_pendingSynths.constFind(v->symbol);
            if (it == m_pendingSynths.constEnd())
                continue;
            const QPair<QString, VgSynthDesc> def{it.key(), it.value()};
            if (!newDefs.contains(def))
                newDefs.append(def);
        }
        if (!newDefs.isEmpty()) {
            if (!VoicegroupSource::writeSynthDefinitions(m_project.root(),
                                                         newDefs, &error)) {
                QMessageBox::warning(this, tr("Save Voicegroup"), error);
                return false;
            }
            // The definitions are on disk now whatever the voicegroup save
            // below does; the catalog must rescan or a failed save would
            // leave them invisible to lookups, dedupe, and the dropdown.
            invalidateVgCatalog();
        }
        if (!session.vgSource->save(&error)) {
            QMessageBox::warning(this, tr("Save Voicegroup"), error);
            return false;
        }
        // Written definitions graduate from pending to on-disk only once the
        // voicegroup save landed: a failed save keeps them pending so synth
        // lookups still resolve them (the writer skips value-equal defs, so
        // the retry save stays a no-op for them).
        for (const auto &def : newDefs)
            m_pendingSynths.remove(def.first);
        cleanupVgPreview();
        // Invalidate before the reload below repopulates the browser: the
        // rebuilt catalog must include any synth definitions written above.
        invalidateVgCatalog();
        // Reload from the project: verifies the saved file parses and
        // replaces any preview-loaded state.
        const int slot = &session == m_active ? m_vgBrowser->currentSlot() : 0;
        QString tried;
        if (LoadedVoiceGroup *vg = loadVoicegroupFor(session.doc.cfg(), &tried))
            swapVoicegroup(session, vg, slot);
        updateVgDockTitle();
        // Only a real write may refresh the stamp: recording the mtime on a
        // doc-only save would silently absorb another tab's voicegroup save
        // and defeat the staleness reload for good.
        session.vgFileTime = QFileInfo(session.vgSource->filePath()).lastModified();
        // Sibling tabs on this voicegroup can't wait for their next
        // activation: the ACTIVE tab never gets one, and a stale parse
        // there would revert this save on its own next voicegroup write.
        refreshSessionsAfterVgSave(session.vgSource->filePath(), &session);
    }

    QString error;
    if (!session.doc.save(&error)) {
        QMessageBox::warning(this, tr("Save Song"), error);
        return false;
    }
    if (session.songId >= 0)
        m_project.setSongCfg(session.songId, session.doc.cfg());
    statusBar()->showMessage(
        vgWasDirty ? tr("Saved %1 and %2")
                         .arg(session.doc.midPath(), session.vgSource->filePath())
                   : tr("Saved %1").arg(session.doc.midPath()),
        5000);
    updateTabTitle(session);
    if (&session == m_active)
        updateWindowTitle();
    return true;
}

void MainWindow::exportWav()
{
    SongSession *session = m_active;
    if (!session || !m_audio.songLoaded())
        return;
    const bool hasLoop = m_audio.timeline()->hasLoop();

    // Options: loop count + fadeout for looping songs, ring-out tail
    // otherwise (SPEC §7 — poryaaaa_render parity), with a live duration
    // preview. The timeline is rebuilt per rate change only for that
    // preview math; the real render builds its own below.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Export WAV"));
    auto *form = new QFormLayout(&dialog);

    auto *rateBox = new QComboBox(&dialog);
    for (int rate : {32000, 44100, 48000})
        rateBox->addItem(tr("%1 Hz").arg(rate), rate);
    rateBox->setCurrentIndex(2);
    form->addRow(tr("Sample rate:"), rateBox);

    QSpinBox *loopCountBox = nullptr;
    QDoubleSpinBox *fadeoutBox = nullptr;
    QDoubleSpinBox *tailBox = nullptr;
    if (hasLoop) {
        loopCountBox = new QSpinBox(&dialog);
        loopCountBox->setRange(1, 99);
        loopCountBox->setValue(2);
        form->addRow(tr("Loop count:"), loopCountBox);
        fadeoutBox = new QDoubleSpinBox(&dialog);
        fadeoutBox->setRange(0.0, 60.0);
        fadeoutBox->setDecimals(1);
        fadeoutBox->setValue(5.0);
        fadeoutBox->setSuffix(tr(" s"));
        form->addRow(tr("Fadeout:"), fadeoutBox);
    } else {
        tailBox = new QDoubleSpinBox(&dialog);
        tailBox->setRange(0.0, 60.0);
        tailBox->setDecimals(1);
        tailBox->setValue(3.0);
        tailBox->setSuffix(tr(" s"));
        form->addRow(tr("Tail (no loop markers):"), tailBox);
    }

    auto *durationLabel = new QLabel(&dialog);
    form->addRow(tr("Duration:"), durationLabel);

    auto currentOptions = [&] {
        WavExportOptions opts;
        opts.sampleRate = rateBox->currentData().toInt();
        if (loopCountBox)
            opts.loopCount = loopCountBox->value();
        if (fadeoutBox)
            opts.fadeoutSeconds = fadeoutBox->value();
        if (tailBox)
            opts.tailSeconds = tailBox->value();
        return opts;
    };
    auto updateDuration = [&] {
        const WavExportOptions opts = currentOptions();
        // Loop/length sample positions scale exactly with the rate, so the
        // loaded timeline's positions can be rescaled for the preview
        // instead of rebuilding the timeline on every spin.
        const double scale = double(opts.sampleRate) / m_audio.timeline()->sampleRate;
        MidiTimeline scaled;
        scaled.lengthSamples = uint64_t(double(m_audio.timeline()->lengthSamples) * scale);
        if (hasLoop) {
            scaled.loopStartSample =
                uint64_t(double(m_audio.timeline()->loopStartSample) * scale);
            scaled.loopEndSample =
                uint64_t(double(m_audio.timeline()->loopEndSample) * scale);
        }
        const uint64_t total = wavExportTotals(scaled, opts).totalSamples;
        const int seconds = int(double(total) / opts.sampleRate + 0.5);
        durationLabel->setText(
            QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10,
                                                          QLatin1Char('0')));
    };
    connect(rateBox, &QComboBox::currentIndexChanged, &dialog, updateDuration);
    if (loopCountBox)
        connect(loopCountBox, &QSpinBox::valueChanged, &dialog, updateDuration);
    if (fadeoutBox)
        connect(fadeoutBox, &QDoubleSpinBox::valueChanged, &dialog, updateDuration);
    if (tailBox)
        connect(tailBox, &QDoubleSpinBox::valueChanged, &dialog, updateDuration);
    updateDuration();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const WavExportOptions opts = currentOptions();

    QSettings appSettings;
    const QString startDir = appSettings
                                 .value(QStringLiteral("lastWavExportDir"),
                                        QFileInfo(session->doc.midPath()).path())
                                 .toString();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export WAV"),
        startDir + QLatin1Char('/') + session->doc.label() + ".wav",
        tr("WAV files (*.wav)"));
    if (path.isEmpty())
        return;
    appSettings.setValue(QStringLiteral("lastWavExportDir"), QFileInfo(path).path());

    stopPlayback();

    QProgressDialog progress(tr("Rendering %1...").arg(session->doc.label()),
                             tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    // The render reads the same voicegroup the audio engine borrows (the
    // engine only reads it too), against a fresh timeline at the export
    // rate — so unsaved document and voice edits export as heard.
    auto timeline = session->doc.buildTimeline(double(opts.sampleRate));
    QString error;
    const bool ok = ::exportWav(path, *timeline, session->voicegroup,
                                songSettingsFor(*session), opts,
                              [&](double fraction) {
                                  progress.setValue(int(fraction * 1000));
                                  return !progress.wasCanceled();
                              },
                              &error);
    progress.setValue(1000);
    if (!ok) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Export WAV"), error);
        else
            statusBar()->showMessage(tr("Export cancelled."), 5000);
        return;
    }
    const uint64_t total = wavExportTotals(*timeline, opts).totalSamples;
    statusBar()->showMessage(tr("Exported %1 (%2 @ %3 Hz)")
                                 .arg(path,
                                      QStringLiteral("%1:%2")
                                          .arg(int(total / uint64_t(opts.sampleRate)) / 60)
                                          .arg(int(total / uint64_t(opts.sampleRate)) % 60,
                                               2, 10, QLatin1Char('0')))
                                 .arg(opts.sampleRate),
                             8000);
}

void MainWindow::openSongSettings()
{
    if (!m_active)
        return;
    SongSettingsDialog dialog(m_active->doc.cfg(), m_active->doc.label(),
                              SongRegistry::voicegroupArgs(m_project.root()), this);
    if (dialog.exec() == QDialog::Accepted)
        m_active->doc.setCfg(dialog.cfg());
}

void MainWindow::openEngineSettings()
{
    EngineSettingsDialog dialog(m_engineSettings, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_engineSettings = dialog.settings();
    m_engineSettings.save();
    if (m_audioOk && m_active && m_audio.songLoaded())
        m_audio.updateSettings(songSettingsFor(*m_active));
}

SongSettings MainWindow::songSettingsFor(const SongSession &session) const
{
    const SongCfg &cfg = session.doc.cfg();
    SongSettings settings;
    settings.songVolume = uint8_t(cfg.masterVolume);
    settings.reverb = uint8_t(cfg.reverb > 0 ? cfg.reverb : 0);
    settings.maxPcmChannels = uint8_t(m_engineSettings.maxPcmChannels);
    settings.pcmMixRate = m_engineSettings.pcmMixRate;
    settings.analogFilter = m_engineSettings.analogFilter;
    return settings;
}

void MainWindow::newSong()
{
    if (!m_project.isOpen())
        return;
    NewSongWizard wizard(&m_project, this);
    if (wizard.exec() != QDialog::Accepted)
        return;
    finishCreateSong(wizard.songFile(), wizard.label(), wizard.constant(),
                     wizard.player(), wizard.cfg(), wizard.newVoicegroupName());
}

void MainWindow::importMidi()
{
    if (!m_project.isOpen())
        return;
    QSettings settings;
    const QString startDir =
        settings.value(QStringLiteral("lastImportDir"), QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import MIDI"), startDir, tr("MIDI files (*.mid *.midi)"));
    if (path.isEmpty())
        return;
    settings.setValue(QStringLiteral("lastImportDir"), QFileInfo(path).path());

    SmfFile smf;
    QString error;
    if (!SmfFile::readFile(path, &smf, &error)) {
        QMessageBox::warning(this, tr("Import MIDI"), error);
        return;
    }
    NewSongWizard wizard(&m_project, std::move(smf), path, this);
    if (wizard.exec() != QDialog::Accepted)
        return;
    finishCreateSong(wizard.songFile(), wizard.label(), wizard.constant(),
                     wizard.player(), wizard.cfg(), wizard.newVoicegroupName());
}

void MainWindow::importSample()
{
    importSampleForSlot(-1);
}

void MainWindow::importSampleForSlot(int slot)
{
    if (!m_project.isOpen())
        return;
    // Refuse before the file dialog: a legacy-aif or unwired project can't
    // take samples no matter which file is picked.
    const SampleFormatProbe probe =
        SampleRegistrar::probeSampleFormat(m_project.root());
    if (!probe.ok()) {
        QMessageBox::warning(this, tr("Import Sample"), probe.refusal);
        return;
    }
    QSettings settings;
    const QString startDir =
        settings.value(QStringLiteral("lastSampleDir"), QDir::homePath())
            .toString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Sample"), startDir,
        tr("Audio files (*.wav *.aif *.aiff *.mp3 *.flac *.ogg *.sf2);;"
           "All files (*)"));
    if (path.isEmpty())
        return;
    settings.setValue(QStringLiteral("lastSampleDir"), QFileInfo(path).path());

    QFile sourceFile(path);
    if (!sourceFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Sample"),
                             tr("Cannot read %1.").arg(path));
        return;
    }
    const QByteArray sourceBytes = sourceFile.readAll();
    sourceFile.close();

    ImportedSample sample;
    QString error;
    // Decode choices beyond the source bytes, recorded in the provenance
    // sidecar so "Edit sample…" can re-decode identically.
    bool leftOnly = false;
    int sf2Zone = -1;
    if (sf2Magic(sourceBytes)) {
        // SoundFonts hold many samples: pick a zone first (FORMATS.md §5);
        // the chosen zone then rides the ordinary editor pipeline.
        Sf2File font;
        if (!readSf2Bytes(sourceBytes, path, &font, &error)) {
            QMessageBox::warning(
                this, tr("Import Sample"),
                tr("%1: %2").arg(QFileInfo(path).fileName(), error));
            return;
        }
        Sf2ZonePicker picker(font, this);
        if (picker.exec() != QDialog::Accepted)
            return;
        if (!extractSf2Zone(font, picker.selectedZone(), &sample, &error)) {
            QMessageBox::warning(
                this, tr("Import Sample"),
                tr("%1: %2").arg(QFileInfo(path).fileName(), error));
            return;
        }
        sf2Zone = picker.selectedZone();
    } else {
        if (!importAudioBytes(sourceBytes, path, &sample, &error)) {
            QMessageBox::warning(
                this, tr("Import Sample"),
                tr("%1: %2").arg(QFileInfo(path).fileName(), error));
            return;
        }
        if (sample.phaseCancelStereo
            && QMessageBox::question(
                   this, tr("Import Sample"),
                   tr("The left and right channels of %1 are "
                      "phase-cancelling — the mono mix may sound hollow.\n\n"
                      "Import the left channel only instead?")
                       .arg(QFileInfo(path).fileName()))
                == QMessageBox::Yes) {
            if (!importAudioBytes(sourceBytes, path, &sample, &error, true)) {
                QMessageBox::warning(
                    this, tr("Import Sample"),
                    tr("%1: %2").arg(QFileInfo(path).fileName(), error));
                return;
            }
            leftOnly = true;
        }
    }

    const QString root = m_project.root();
    const QStringList symbols = vgCatalog().directSound;
    // Browser-initiated: audition with the destination voice's envelope
    // when that slot already holds a DirectSound-family voice.
    AuditionSlots::Adsr destAdsr;
    bool hasDestAdsr = false;
    if (slot >= 0 && m_active && m_active->vgSource) {
        const VgVoice *dest = m_active->vgSource->voiceAt(slot);
        if (dest && !vgMacroIsCgb(dest->macro)) {
            destAdsr = {uint8_t(dest->attack), uint8_t(dest->decay),
                        uint8_t(dest->sustain), uint8_t(dest->release)};
            hasDestAdsr = true;
        }
    }
    SampleEditorDialog dialog(
        std::move(sample),
        [root, symbols](const QString &name, QString *validationError) {
            return SampleRegistrar::validateSampleName(root, name, symbols,
                                                       validationError);
        },
        m_audioOk ? &m_audio : nullptr, hasDestAdsr ? &destAdsr : nullptr,
        this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // Write-through commit (not undoable, like song registration): the
    // rendered .wav (FORMATS.md §1) plus its direct_sound_data.inc block.
    if (!SampleRegistrar::registerSample(root, dialog.sampleName(),
                                         dialog.wavBytes(), &error)) {
        QMessageBox::warning(this, tr("Import Sample"), error);
        return;
    }
    // Provenance sidecar (PLAN.md §3): source identity + the edit params, so
    // "Edit sample…" reopens from the hi-res source. Auxiliary — a failed
    // write never fails the commit that just landed.
    SampleSidecar sidecar;
    sidecar.sourcePath = QFileInfo(path).absoluteFilePath();
    sidecar.sourceSha256 = SampleRegistrar::sourceHashHex(sourceBytes);
    sidecar.leftOnly = leftOnly;
    sidecar.sf2Zone = sf2Zone;
    sidecar.params = dialog.document()->params();
    QString sidecarError;
    if (!SampleRegistrar::writeSampleSidecar(root, dialog.sampleName(), sidecar,
                                             &sidecarError))
        statusBar()->showMessage(
            tr("Sample imported, but saving its edit history failed: %1")
                .arg(sidecarError),
            8000);
    invalidateVgCatalog();
    updateVoicegroupBrowser();

    // Browser-initiated: point the requesting slot's voice at the new sample
    // via the session undo stack — the file creation above is write-through,
    // but the voice assignment stays undoable (PLAN.md §3).
    if (slot >= 0 && m_active && m_active->vgSource) {
        // A DirectSound-family slot keeps its voice and only swaps the
        // sample: the dialog auditioned with that voice's envelope, so
        // replacing its ADSR (or key/pan/macro variant) would commit
        // something other than what was heard.
        const VgVoice *dest = m_active->vgSource->voiceAt(slot);
        const bool keepDest = dest
            && (dest->macro == VgMacro::DirectSound
                || dest->macro == VgMacro::DirectSoundNoResample
                || dest->macro == VgMacro::DirectSoundAlt);
        VgVoice voice;
        if (keepDest) {
            voice = *dest;
        } else {
            voice.macro = VgMacro::DirectSound;
            voice.key = 60;
            voice.pan = 0;
            const VgAdsr adsr = vgDefaultAdsr(
                vgCatalog().typicalAdsr, voice.macro,
                QStringLiteral("DirectSoundWaveData_") + dialog.sampleName());
            voice.attack = adsr.attack;
            voice.decay = adsr.decay;
            voice.sustain = adsr.sustain;
            voice.release = adsr.release;
        }
        voice.symbol =
            QStringLiteral("DirectSoundWaveData_") + dialog.sampleName();
        onVoiceEditRequested(slot, voice, true);
        m_vgBrowser->revealSlot(slot);
    }
    statusBar()->showMessage(
        tr("Imported %1 — DirectSoundWaveData_%1 is now available to "
           "voicegroups")
            .arg(dialog.sampleName()),
        8000);
}

void MainWindow::editSampleForSlot(int slot)
{
    if (!m_project.isOpen() || !m_active || !m_active->vgSource)
        return;
    const QString prefix = QStringLiteral("DirectSoundWaveData_");
    const VgVoice *voice = m_active->vgSource->voiceAt(slot);
    if (!voice || !voice->symbol.startsWith(prefix)) {
        QMessageBox::warning(
            this, tr("Edit Sample"),
            tr("This voice does not reference a DirectSound sample."));
        return;
    }
    const QString name = voice->symbol.mid(prefix.size());
    const QString root = m_project.root();
    const SampleFormatProbe probe = SampleRegistrar::probeSampleFormat(root);
    if (!probe.ok()) {
        QMessageBox::warning(this, tr("Edit Sample"), probe.refusal);
        return;
    }
    const QString wavPath =
        probe.samplesDir + QStringLiteral("/%1.wav").arg(name);
    if (!QFile::exists(wavPath)) {
        QMessageBox::warning(
            this, tr("Edit Sample"),
            tr("%1.wav does not exist in sound/direct_sound_samples — only "
               "samples with a .wav source can be edited here.")
                .arg(name));
        return;
    }

    // Provenance: reopen from the sidecar's hi-res source while it still
    // checks out; otherwise the committed 8-bit .wav (the project is
    // canonical without the sidecar — still crop/loop-editable).
    ImportedSample sample;
    SampleSidecar sidecar;
    bool fromSource = false; // decoding the original hi-res source
    bool haveParams = false; // sidecar params apply to that source
    QString error;
    if (SampleRegistrar::readSampleSidecar(root, name, &sidecar)) {
        const auto decodeSource = [&](const QByteArray &bytes,
                                      ImportedSample *out, QString *err) {
            if (sidecar.sf2Zone >= 0) {
                Sf2File font;
                return readSf2Bytes(bytes, sidecar.sourcePath, &font, err)
                    && extractSf2Zone(font, sidecar.sf2Zone, out, err);
            }
            return importAudioBytes(bytes, sidecar.sourcePath, out, err,
                                    sidecar.leftOnly);
        };
        QFile sourceFile(sidecar.sourcePath);
        QByteArray sourceBytes;
        if (sourceFile.open(QIODevice::ReadOnly))
            sourceBytes = sourceFile.readAll();
        if (sourceBytes.isEmpty()) {
            QMessageBox::information(
                this, tr("Edit Sample"),
                tr("The original source (%1) is no longer readable; editing "
                   "the committed 8-bit sample instead.")
                    .arg(sidecar.sourcePath));
        } else if (SampleRegistrar::sourceHashHex(sourceBytes)
                   != sidecar.sourceSha256) {
            const QMessageBox::StandardButton pick = QMessageBox::question(
                this, tr("Edit Sample"),
                tr("%1 has changed since this sample was created, so the "
                   "saved edit settings no longer apply to it.\n\n"
                   "Re-import the changed file with fresh settings? "
                   "(\"No\" edits the committed 8-bit sample instead.)")
                    .arg(sidecar.sourcePath),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (pick == QMessageBox::Cancel)
                return;
            if (pick == QMessageBox::Yes) {
                if (!decodeSource(sourceBytes, &sample, &error)) {
                    QMessageBox::warning(
                        this, tr("Edit Sample"),
                        tr("%1: %2").arg(sidecar.sourcePath, error));
                    return;
                }
                sidecar.sourceSha256 =
                    SampleRegistrar::sourceHashHex(sourceBytes);
                fromSource = true;
            }
        } else if (decodeSource(sourceBytes, &sample, &error)) {
            fromSource = true;
            haveParams = true;
        } else {
            QMessageBox::information(
                this, tr("Edit Sample"),
                tr("The original source (%1) no longer decodes (%2); editing "
                   "the committed 8-bit sample instead.")
                    .arg(sidecar.sourcePath, error));
        }
    }
    if (!fromSource) {
        QFile wavFile(wavPath);
        if (!wavFile.open(QIODevice::ReadOnly)
            || !importAudioBytes(wavFile.readAll(), wavPath, &sample,
                                 &error)) {
            QMessageBox::warning(this, tr("Edit Sample"),
                                 tr("%1: %2").arg(wavPath, error));
            return;
        }
    }

    AuditionSlots::Adsr destAdsr;
    bool hasDestAdsr = false;
    if (!vgMacroIsCgb(voice->macro)) {
        destAdsr = {uint8_t(voice->attack), uint8_t(voice->decay),
                    uint8_t(voice->sustain), uint8_t(voice->release)};
        hasDestAdsr = true;
    }
    SampleEditorDialog dialog(
        std::move(sample),
        [name](const QString &candidate, QString *validationError) {
            if (candidate == name)
                return true;
            if (validationError)
                *validationError =
                    tr("the sample keeps its registered name (%1).").arg(name);
            return false;
        },
        m_audioOk ? &m_audio : nullptr, hasDestAdsr ? &destAdsr : nullptr,
        this);
    dialog.setEditTarget(name);
    if (haveParams)
        dialog.applyParamsExternal(sidecar.params);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // Write-through commit: overwrite the .wav in place; the registration
    // block already exists and stays untouched.
    if (!SampleRegistrar::updateSample(root, name, dialog.wavBytes(),
                                       &error)) {
        QMessageBox::warning(this, tr("Edit Sample"), error);
        return;
    }
    if (fromSource) {
        sidecar.params = dialog.document()->params();
        QString sidecarError;
        if (!SampleRegistrar::writeSampleSidecar(root, name, sidecar,
                                                 &sidecarError))
            statusBar()->showMessage(
                tr("Sample saved, but saving its edit history failed: %1")
                    .arg(sidecarError),
                8000);
    } else {
        // The session came from the committed bytes, which were just
        // replaced — a stale sidecar would misdescribe them on the next
        // reopen (the committed .wav is its own provenance now).
        SampleRegistrar::removeSampleSidecar(root, name);
    }
    invalidateVgCatalog();
    updateVoicegroupBrowser();
    // The loaded voicegroup decoded the old .wav at load time; reload so the
    // edit is audible without reopening the song.
    if (m_active && m_active->vgSource)
        reloadVoicegroupPreview(*m_active, slot);
    statusBar()->showMessage(
        tr("Saved %1 — the ROM's .bin recompiles on the next build")
            .arg(name),
        8000);
}

void MainWindow::finishCreateSong(const SmfFile &smf, const QString &label,
                                  const QString &constant, const QString &player,
                                  const SongCfg &cfg, const QString &newVoicegroup)
{
    QString error;
    // The voicegroup first: the song's -G already points at it, so nothing
    // else may be written if it can't exist. Starts as the dummy template —
    // the user configures it in the Voicegroup dock.
    if (!newVoicegroup.isEmpty()) {
        if (!VoicegroupSource::createVoicegroup(m_project.root(), newVoicegroup,
                                                QString(), QString(), &error)
            || !VoicegroupSource::appendIncludeLine(m_project.root(), newVoicegroup,
                                                    &error)) {
            QMessageBox::warning(this, tr("New Song"), error);
            return;
        }
    }

    const QString midiDir = m_project.root() + QStringLiteral("/sound/songs/midi");
    if (!smf.writeFile(midiDir + QStringLiteral("/%1.mid").arg(label), &error)
        || !SongRegistry::writeSongFlags(midiDir, label,
                                         SongRegistry::mergeCfgFlags(cfg), &error)) {
        QMessageBox::warning(this, tr("New Song"), error);
        return;
    }
    int songId = -1;
    if (!SongRegistry::registerSong(m_project.root(), label, constant, player, &error,
                                    &songId)) {
        // Keep the chosen constant/player so Register Song can retry later;
        // the song shows a badge in the browser until then.
        SongRegistry::saveRegistrationMeta(m_project.root(), label, constant, player);
        QMessageBox::warning(this, tr("New Song"),
                             tr("Wrote %1/%2.mid, but registering it failed: %3\n"
                                "Use File → Register Song to retry.")
                                 .arg(midiDir, label, error));
    } else {
        SongRegistry::clearRegistrationMeta(m_project.root(), label);
        QString message = tr("Created and registered %1 as %2 (song ID %3)")
                              .arg(label, constant)
                              .arg(songId);
        if (!newVoicegroup.isEmpty())
            message += tr(" — configure its new voicegroup in the Voicegroup dock");
        statusBar()->showMessage(message, 8000);
    }

    reloadProject();
    // The fresh song opens in its own tab, next to whatever is being worked
    // on.
    loadSongByLabel(label, /*newTab=*/true);
}

void MainWindow::registerLoadedSong()
{
    SongSession *session = m_active;
    if (!session || session->songId < 0 || session->songId >= m_project.songs().size())
        return;
    const SongInfo song = m_project.songs().at(session->songId);
    const QString constant = song.constant.isEmpty()
                                 ? SongRegistry::constantForLabel(song.label)
                                 : song.constant;
    const QString player = song.player.isEmpty() ? QStringLiteral("MUSIC_PLAYER_BGM")
                                                 : song.player;
    QString error;
    int songId = -1;
    if (!SongRegistry::registerSong(m_project.root(), song.label, constant, player,
                                    &error, &songId)) {
        QMessageBox::warning(this, tr("Register Song"), error);
        return;
    }
    SongRegistry::clearRegistrationMeta(m_project.root(), song.label);
    statusBar()->showMessage(
        tr("Registered %1 as %2 (song ID %3)").arg(song.label, constant).arg(songId),
        8000);
    // The open tab keeps its document; only the registry-derived state
    // (badge, song ids) needs refreshing.
    reloadProject();
    m_registerAction->setEnabled(false);
}

void MainWindow::reloadProject()
{
    QString error;
    if (!m_project.reload(&error)) {
        QMessageBox::warning(this, tr("Reload Project"), error);
        return;
    }
    populateSongList();
    refreshSessionSongIds();
    invalidateVgCatalog();
}

void MainWindow::loadSongByLabel(const QString &label, bool newTab)
{
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == label && song.isPlayable()) {
            loadSong(song, newTab);
            return;
        }
    }
}

void MainWindow::updateVoicegroupBrowser()
{
    SongSession *session = m_active;
    if (!session || !session->voicegroup) {
        m_vgBrowser->setVoicegroup(nullptr);
        updateVgDockTitle();
        return;
    }
    const QString arg = session->doc.cfg().voicegroupArg.isEmpty()
                            ? QStringLiteral("_dummy")
                            : session->doc.cfg().voicegroupArg;
    m_vgBrowser->setVoicegroup(session->voicegroup);
    m_vgBrowser->setUsedVoices(session->view->usedVoices());
    const VgCatalog &catalog = vgCatalog();
    m_vgBrowser->setVoicegroupChoices(catalog.groupArgs);
    m_vgBrowser->setCurrentVoicegroupArg(arg);
    m_vgBrowser->setSource(
        session->vgSource.get(), catalog.directSound, catalog.progWave,
        catalog.keysplits, catalog.drumkits, catalog.typicalAdsr, catalog.synths,
        m_pendingSynths, [this](const VgSynthDesc &desc) -> QString {
            // Mint a pending symbol for the descriptor — nothing is written;
            // the definition reaches disk when a voicegroup referencing it
            // saves. Value-equal definitions (on disk or pending) are reused.
            const VgSynthCatalog &synths = vgCatalog().synths;
            QString symbol = synths.symbolFor(desc);
            if (!symbol.isEmpty())
                return symbol;
            for (auto it = m_pendingSynths.constBegin();
                 it != m_pendingSynths.constEnd(); ++it) {
                if (it.value() == desc)
                    return it.key();
            }
            if (!synths.creatable()) {
                statusBar()->showMessage(
                    tr("Cannot create synth instrument: this project doesn't "
                       "define the set_synth_* macros (Golden Sun synths need "
                       "ipatix's improved mixer)."),
                    8000);
                return QString();
            }
            // Param-named; a hand-written symbol with the same name but
            // different bytes (or a plain sample) forces a suffix.
            symbol = vgSynthSymbolName(desc);
            const QString base = symbol;
            for (int i = 2; synths.find(symbol)
                 || vgCatalog().directSound.contains(symbol);
                 i++)
                symbol = base + QStringLiteral("_%1").arg(i);
            m_pendingSynths.insert(symbol, desc);
            return symbol;
        });
    updateVgDockTitle();
}

const MainWindow::VgCatalog &MainWindow::vgCatalog()
{
    if (!m_vgCatalog.valid) {
        const QString root = m_project.root();
        m_vgCatalog.groupArgs = SongRegistry::voicegroupArgs(root);
        m_vgCatalog.directSound = VoicegroupSource::directSoundSymbols(root);
        m_vgCatalog.progWave = VoicegroupSource::progWaveSymbols(root);
        m_vgCatalog.keysplits = VoicegroupSource::keysplitInstruments(root);
        m_vgCatalog.drumkits = VoicegroupSource::drumkitInstruments(root);
        m_vgCatalog.synths = VoicegroupSource::synthInstruments(root);
        m_vgCatalog.typicalAdsr = VoicegroupSource::typicalAdsr(root);
        m_vgCatalog.valid = true;
    }
    return m_vgCatalog;
}

void MainWindow::openVoicegroupSource(SongSession &session, const SongCfg &cfg)
{
    session.vgSource = std::make_unique<VoicegroupSource>();
    QString error;
    if (!session.vgSource->open(m_project.root(), cfg.voicegroupArg, &error)) {
        session.vgSource.reset();
        session.vgFileTime = QDateTime();
        statusBar()->showMessage(
            tr("Voicegroup editing unavailable: %1").arg(error), 8000);
        return;
    }
    session.vgFileTime = QFileInfo(session.vgSource->filePath()).lastModified();
}

void MainWindow::onVoiceEditRequested(int slot, const VgVoice &voice, bool structural)
{
    SongSession *session = m_active;
    if (!session || !session->vgSource)
        return;
    const VgVoice *before = session->vgSource->voiceAt(slot);
    if (!before || *before == voice)
        return;
    // push() applies the edit (redo) via applyVoiceEdit.
    session->doc.undoStack()->push(
        new VoiceEditCommand(this, session, session->vgSource->loadName(), slot,
                             *before, voice, structural));
}

void MainWindow::applyVoiceEdit(SongSession &session, const QString &loadName, int slot,
                                const VgVoice &voice, bool structural)
{
    if (!session.vgSource || session.vgSource->loadName() != loadName)
        return; // stale target; replayVoiceEdits re-syncs when it reopens
    session.vgSource->setVoice(slot, voice);
    onVoiceEdited(session, slot, structural);
    if (!structural && &session == m_active)
        m_vgBrowser->voiceChanged(slot);
    updateTabTitle(session);
    if (&session == m_active)
        updateWindowTitle();
}

void MainWindow::replayVoiceEdits(SongSession &session)
{
    if (!session.vgSource)
        return;
    // Every command below the current index is applied; poke the ones that
    // target the just-reopened voicegroup back into it. Called from inside a
    // cfg command's undo()/redo(), where QUndoStack::index() still counts
    // that cfg command as applied — it isn't a voice edit, so it's skipped.
    const QUndoStack *stack = session.doc.undoStack();
    for (int i = 0; i < stack->index(); i++) {
        const QUndoCommand *cmd = stack->command(i);
        if (cmd->id() != kVoiceEditCommandId)
            continue;
        auto *edit = static_cast<const VoiceEditCommand *>(cmd);
        if (edit->loadName() == session.vgSource->loadName())
            session.vgSource->setVoice(edit->slot(), edit->after());
    }
}

void MainWindow::onVoiceEdited(SongSession &session, int slot, bool structural)
{
    if (!session.vgSource)
        return;
    if (structural) {
        reloadVoicegroupPreview(session, slot);
    } else {
        ToneData *tone = nullptr;
        if (session.voicegroup && slot >= 0 && slot < VOICEGROUP_SIZE)
            tone = &session.voicegroup->voices[slot];
        session.vgSource->applyScalarsToToneData(slot, tone);
        // Synth param edits are scalar pokes too: the descriptor bytes are
        // patched straight into the loaded tone (pending definitions have
        // nothing on disk to reload from). The poke can rename the voice
        // (param-named symbols), which track headers display — repaint.
        if (applyPendingSynthTones(session, session.voicegroup))
            session.view->update();
        // Playing tracks hold a copy of their instrument; refresh so the
        // edit is heard from the next note without a pause/play cycle.
        if (&session == m_active && m_audioOk)
            m_audio.refreshVoices();
    }
    updateVgDockTitle();
    updateTabTitle(session);
}

void MainWindow::reloadVoicegroupPreview(SongSession &session, int keepSlot)
{
    const QString previewDir =
        m_project.root() + QStringLiteral("/.porydaw/vgpreview");
    QDir().mkpath(previewDir);
    {
        QFile out(previewDir + QLatin1Char('/') + session.vgSource->loadName()
                  + QStringLiteral(".inc"));
        if (!out.open(QIODevice::WriteOnly)) {
            statusBar()->showMessage(tr("Cannot write voicegroup preview file."), 8000);
            return;
        }
        out.write(session.vgSource->renderPreview());
    }
    // The config's voicegroup paths are searched before the project's own
    // (voicegroup_loader.c discovery), so the preview file shadows the real
    // one while samples and keysplits still resolve from the project.
    VoicegroupLoaderConfig config;
    std::memset(&config, 0, sizeof(config));
    std::strncpy(config.voicegroupPaths[0], ".porydaw/vgpreview", VG_MAX_PATH_LEN - 1);
    config.voicegroupPathCount = 1;
    LoadedVoiceGroup *vg =
        voicegroup_load(m_project.root().toLocal8Bit().constData(),
                        session.vgSource->loadName().toLocal8Bit().constData(), &config);
    if (!vg) {
        statusBar()->showMessage(
            tr("Edited voicegroup failed to load — keeping the previous sound."), 8000);
        return;
    }
    swapVoicegroup(session, vg, keepSlot);
}

const VgSynthDesc *MainWindow::synthDescForSymbol(const QString &symbol)
{
    const auto pending = m_pendingSynths.constFind(symbol);
    if (pending != m_pendingSynths.constEnd())
        return &pending.value();
    return vgCatalog().synths.find(symbol);
}

bool MainWindow::applyPendingSynthTones(SongSession &session, LoadedVoiceGroup *vg)
{
    if (!session.vgSource || !vg)
        return false;
    bool changed = false;
    for (int slot = 0; slot < VOICEGROUP_SIZE; slot++) {
        const VgVoice *v = session.vgSource->voiceAt(slot);
        if (!v
            || (v->macro != VgMacro::DirectSound
                && v->macro != VgMacro::DirectSoundNoResample
                && v->macro != VgMacro::DirectSoundAlt))
            continue;
        const VgSynthDesc *desc = synthDescForSymbol(v->symbol);
        if (!desc)
            continue;
        // A synth param edit rides the scalar path, so the reload that would
        // rebuild voiceNames never runs — sync the slot's name here or track
        // labels and the browser tree keep showing the pre-edit symbol.
        const QByteArray name = v->symbol.toUtf8();
        if (qstrncmp(vg->voiceNames[slot], name.constData(),
                     VG_VOICE_NAME_LEN - 1)
            != 0) {
            std::strncpy(vg->voiceNames[slot], name.constData(),
                         VG_VOICE_NAME_LEN - 1);
            vg->voiceNames[slot][VG_VOICE_NAME_LEN - 1] = '\0';
            changed = true;
        }
        ToneData &td = vg->voices[slot];
        // Already sounding these bytes (the loader resolved an on-disk
        // definition, or an earlier patch)? Leave the tone alone.
        if (td.wav && td.wav->size == 0 && td.wav->data) {
            const auto *d = reinterpret_cast<const uint8_t *>(td.wav->data);
            const VgSynthDesc current{d[1] > 2 ? 2 : d[1], d[2], d[3], d[4], d[5]};
            if (current == *desc)
                continue;
        }
        // Never mutate a loader-owned WaveData (shared across voices via its
        // cache): point the tone at a session-owned descriptor instead.
        // Re-patching an installed buffer pokes its bytes in place, which the
        // engine reads every tick — live tweaks sound without a reload.
        std::unique_ptr<SynthToneBuf> &tone = session.synthTones[slot];
        if (!tone) {
            tone = std::make_unique<SynthToneBuf>();
            std::memset(tone.get(), 0, sizeof(SynthToneBuf));
            tone->wd.status = 0x4000;     // loop flag, as the synth header sets
            tone->wd.freq = 0x01058920;   // 64-sample period lands on middle C
            tone->wd.size = 0;            // size 0 = synth descriptor
            tone->wd.data = reinterpret_cast<int8_t *>(tone->bytes);
        }
        tone->bytes[0] = 0x80;
        tone->bytes[1] = uint8_t(desc->waveform);
        tone->bytes[2] = uint8_t(desc->baseDuty);
        tone->bytes[3] = uint8_t(desc->dutyStep);
        tone->bytes[4] = uint8_t(desc->modDepth);
        tone->bytes[5] = uint8_t(desc->phase);
        td.wav = &tone->wd;
        changed = true;
    }
    return changed;
}

void MainWindow::swapVoicegroup(SongSession &session, LoadedVoiceGroup *vg, int keepSlot)
{
    // Pending synth definitions aren't on disk, so a fresh load can't have
    // resolved them; patch before anything (views, engine) sees the group.
    applyPendingSynthTones(session, vg);
    session.view->setVoicegroup(nullptr);
    if (&session == m_active) {
        m_vgBrowser->setVoicegroup(nullptr);
        if (m_audioOk)
            m_audio.updateVoicegroup(vg);
    }
    if (session.voicegroup)
        voicegroup_free(session.voicegroup);
    session.voicegroup = vg;
    session.view->setVoicegroup(vg);
    if (&session == m_active) {
        updateVoicegroupBrowser();
        m_vgBrowser->selectSlot(keepSlot);
    }
}

void MainWindow::cleanupVgPreview()
{
    if (!m_project.isOpen())
        return;
    QDir(m_project.root() + QStringLiteral("/.porydaw/vgpreview")).removeRecursively();
}

void MainWindow::updateVgDockTitle()
{
    const bool dirty = m_active && m_active->vgSource && m_active->vgSource->dirty();
    m_vgDock->setWindowTitle(dirty ? tr("Voicegroup*") : tr("Voicegroup"));
}

void MainWindow::newVoicegroup()
{
    if (!m_project.isOpen())
        return;
    if (!QDir(m_project.root() + QStringLiteral("/sound/voicegroups")).exists()) {
        QMessageBox::information(
            this, tr("New Voicegroup"),
            tr("This project keeps all voicegroups in one file; creating new "
               "per-file voicegroups isn't supported for that layout."));
        return;
    }
    VoicegroupSource *activeSource =
        m_active ? m_active->vgSource.get() : nullptr;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Voicegroup"));
    auto *form = new QFormLayout(&dialog);
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z][A-Za-z0-9_]*")), nameEdit));
    form->addRow(tr("Name"), nameEdit);
    auto *sourceCombo = new QComboBox(&dialog);
    if (activeSource)
        sourceCombo->addItem(tr("Copy of %1")
                                 .arg(QFileInfo(activeSource->filePath()).fileName()),
                             activeSource->filePath());
    sourceCombo->addItem(tr("Empty (dummy template)"), QString());
    form->addRow(tr("Start from"), sourceCombo);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty())
        return;
    if (SongRegistry::voicegroupArgs(m_project.root())
            .contains(QStringLiteral("_") + name)) {
        QMessageBox::warning(this, tr("New Voicegroup"),
                             tr("A voicegroup named %1 already exists.").arg(name));
        return;
    }

    const QString copyFrom = sourceCombo->currentData().toString();
    const QString sectionLabel =
        (!copyFrom.isEmpty() && activeSource && copyFrom == activeSource->filePath())
        ? activeSource->sectionLabel()
        : QString();
    QString error;
    if (!VoicegroupSource::createVoicegroup(m_project.root(), name, copyFrom,
                                            sectionLabel, &error)
        || !VoicegroupSource::appendIncludeLine(m_project.root(), name, &error)) {
        QMessageBox::warning(this, tr("New Voicegroup"), error);
        return;
    }
    invalidateVgCatalog();
    updateVoicegroupBrowser(); // the selector's choices now include it
    statusBar()->showMessage(
        tr("Created sound/voicegroups/%1.inc — assign it with the voicegroup "
           "selector above the instrument list (voicegroup _%1).")
            .arg(name),
        10000);
}

bool MainWindow::maybeSaveSession(SongSession &session)
{
    // Voicegroup edits count as the song's unsaved changes: to the user the
    // song and its voicegroup are one document (a normally-clean undo stack
    // can still leave the voicegroup dirty when a save was refused mid-way).
    if (!session.isDirty())
        return true;
    const bool vgDirty = session.vgSource && session.vgSource->dirty();
    // Show the tab being asked about: Save/Discard for edits the user
    // can't see is a data-loss trap.
    if (&session != m_active && m_tabs->indexOf(session.view) >= 0)
        m_tabs->setCurrentWidget(session.view);
    const auto choice = QMessageBox::question(
        this, tr("Unsaved Changes"),
        vgDirty ? tr("%1 has unsaved changes (including voicegroup edits). Save them?")
                      .arg(session.doc.label())
                : tr("%1 has unsaved changes. Save them?").arg(session.doc.label()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save)
        return saveSession(session);
    return true;
}

void MainWindow::saveViewState(SongSession &session)
{
    if (session.doc.label().isEmpty())
        return;
    ViewSidecar::save(m_project.root(), session.doc.label(),
                      session.view->viewState());
}

void MainWindow::updateWindowTitle()
{
    const QString project =
        m_project.isOpen() ? QDir(m_project.root()).dirName() : QString();
    if (m_active) {
        setWindowTitle(
            QStringLiteral("%1[*] — %2 — porydaw").arg(m_active->doc.label(), project));
        setWindowModified(m_active->isDirty());
    } else {
        setWindowTitle(project.isEmpty()
                           ? QStringLiteral("porydaw")
                           : QStringLiteral("%1 — porydaw").arg(project));
        setWindowModified(false);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Every dirty tab gets its prompt; a Cancel keeps the window open
    // (Saves answered before it have already written, as with any
    // save-all).
    if (!promptToSaveAllSessions()) {
        event->ignore();
        return;
    }
    for (const auto &session : m_sessions)
        saveViewState(*session);
    cleanupVgPreview();
    if (m_persistSession) {
        QSettings settings;
        settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
        settings.setValue(QStringLiteral("windowState"), saveState());
        settings.setValue(QStringLiteral("songFilterText"), m_songList->searchText());
        settings.setValue(QStringLiteral("songFilterSort"), m_songList->sortIndex());
        settings.setValue(QStringLiteral("songFilterCategory"),
                          m_songList->categoryPrefix());
    }
    event->accept();
}

void MainWindow::uiTick()
{
    if (!m_audioOk)
        return;
    if (m_active && m_audio.songLoaded()) {
        const uint64_t length = m_audio.timeline()->lengthSamples;
        m_timeLabel->setText(QStringLiteral("%1 / %2")
                                 .arg(formatTime(m_audio.playheadSamples()),
                                      formatTime(length)));

        const uint64_t lost = m_audio.polyLostTotal();
        QString poly = tr("PCM %1/%2 · CGB %3/4")
                           .arg(m_audio.activePcmChannels())
                           .arg(m_audio.maxPcmChannels())
                           .arg(m_audio.activeCgbChannels());
        if (lost > 0)
            poly += tr(" · %1 notes lost").arg(lost);
        m_polyLabel->setText(poly);

        if (m_polyDock->isVisible()) {
            AudioEngine::PolySnapshot snap;
            m_audio.polySnapshot(&snap);
            m_polyPanel->updateSnapshot(snap);
        }
    }
    updateTransportActions();
}

void MainWindow::synchronizePlayhead()
{
    if (!m_audioOk || !m_active || !m_audio.songLoaded()) {
        // This also runs synchronously from activateSession(nullptr).
        m_playheadTimer->stop();
        return;
    }

    const bool playing = m_audio.transport() == Transport::Playing;
    m_active->view->setPlayheadSample(m_audio.playheadSamples(), playing);
    if (playing) {
        if (!m_playheadTimer->isActive())
            m_playheadTimer->start();
    } else {
        m_playheadTimer->stop();
    }
}

void MainWindow::startPlayback(bool fromEditCursor)
{
    if (!m_audioOk || !m_active || !m_audio.songLoaded())
        return;
    if (fromEditCursor || m_audio.transport() == Transport::Stopped)
        m_audio.seek(
            m_audio.timeline()->sampleForTick(m_active->view->editCursorTick()));
   
      
    m_audio.play();
    synchronizePlayhead();
}

void MainWindow::pausePlayback()
{
    m_audio.pause();
    synchronizePlayhead();
}

void MainWindow::stopPlayback()
{
    m_audio.stop();
    synchronizePlayhead();
}

void MainWindow::updateTransportActions()
{
    const bool loaded = m_audioOk && m_audio.songLoaded();
    const Transport t = m_audio.transport();
    m_goToStartAction->setEnabled(loaded);
    m_playAction->setEnabled(loaded && t != Transport::Playing);
    m_playPauseAction->setEnabled(loaded);
    m_pauseAction->setEnabled(loaded && t == Transport::Playing);
    m_stopAction->setEnabled(loaded && t != Transport::Stopped);
    m_loopAction->setEnabled(loaded);
}

bool MainWindow::runSelfTest(const QString &projectRoot, const QString &songLabel)
{
    m_persistSession = false;
    if (!m_audioOk) {
        qWarning("selftest: no audio device available");
        return false;
    }
    QString error;
    if (!m_project.open(projectRoot, &error)) {
        qWarning("selftest: %s", qUtf8Printable(error));
        return false;
    }
    const SongInfo *target = nullptr;
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == songLabel) {
            target = &song;
            break;
        }
    }
    if (!target || !target->isPlayable()) {
        qWarning("selftest: song '%s' not found or has no MIDI source",
                 qUtf8Printable(songLabel));
        return false;
    }
    qInfo("selftest: project opened, %lld songs", (long long)m_project.songs().size());

    loadSong(*target);
    SongSession *tab = m_active;
    if (!tab || !m_audio.songLoaded()) {
        qWarning("selftest: song failed to load");
        return false;
    }
    qInfo("selftest: loaded %s (%zu events, %d tracks)", qUtf8Printable(target->label),
          m_audio.timeline()->events.size(), m_audio.timeline()->usedTrackCount);

    startPlayback();
    QEventLoop loop;
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();

    // M2: edit during playback — exercises the documentChanged plumbing
    // (timeline rebuild, playhead-preserving audio swap, view refresh).
    const uint64_t posBeforeEdit = m_audio.playheadSamples();
    tab->doc.addNote(tab->view->selectedTrack(), 0, 60, 24, 100);
    tab->doc.addLanePoint(tab->view->selectedTrack(), 7, 0, 100);
    if (!tab->doc.isDirty()) {
        qWarning("selftest: document not dirty after edits");
        return false;
    }
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    tab->doc.undoStack()->undo();
    tab->doc.undoStack()->undo();
    if (tab->doc.isDirty()) {
        qWarning("selftest: document still dirty after undoing all edits");
        return false;
    }
    qInfo("selftest: edit + undo during playback OK (playhead %.2fs at edit)",
          double(posBeforeEdit) / m_audio.sampleRate());

    // M3: audition a voicegroup entry mid-playback — exercises the preview
    // engine instance (program change + note on a separate engine).
    m_audio.previewVoice(0, 60, 112);
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
    m_audio.previewVoice(0, 60, 0);
    qInfo("selftest: voice audition through the preview engine OK");

    // Voicegroup editing through the unified pipeline: a scalar edit pokes
    // the live ToneData, a sample swap goes through the .porydaw/vgpreview
    // shadow reload — both land on the song's undo stack, and undoing them
    // restores the on-disk state — all without changing project files.
    bool vgEditOk = true;
    if (tab->vgSource) {
        int dsSlot = -1, donorSlot = -1;
        for (int i = 0; i < VOICEGROUP_SIZE; i++) {
            const VgVoice *v = tab->vgSource->voiceAt(i);
            // DirectSound family only: keysplit/drumkit voices are non-CGB
            // too, but have no scalar fields and take sub-voicegroup symbols,
            // not samples.
            if (!v
                || (v->macro != VgMacro::DirectSound
                    && v->macro != VgMacro::DirectSoundNoResample
                    && v->macro != VgMacro::DirectSoundAlt))
                continue;
            if (dsSlot < 0)
                dsSlot = i;
            else if (donorSlot < 0 && v->symbol != tab->vgSource->voiceAt(dsSlot)->symbol)
                donorSlot = i;
        }
        if (dsSlot >= 0) {
            m_vgBrowser->selectSlot(dsSlot); // exercises the editor panel too
            QByteArray fileBefore;
            {
                QFile in(tab->vgSource->filePath());
                in.open(QIODevice::ReadOnly);
                fileBefore = in.readAll();
            }
            const VgVoice original = *tab->vgSource->voiceAt(dsSlot);
            const QByteArray originalName(m_audio.voicegroup()->voiceNames[dsSlot]);
            int undosNeeded = 1;
            VgVoice v = original;
            v.release = v.release == 25 ? 26 : 25;
            onVoiceEditRequested(dsSlot, v, false);
            vgEditOk = tab->vgSource->dirty() && tab->doc.isDirty()
                && m_audio.voicegroup()->voices[dsSlot].release == uint8_t(v.release);
            if (donorSlot >= 0) {
                undosNeeded = 2; // structural edits never merge with scalar ones
                const QByteArray donorName(m_audio.voicegroup()->voiceNames[donorSlot]);
                v.symbol = tab->vgSource->voiceAt(donorSlot)->symbol;
                onVoiceEditRequested(dsSlot, v, true);
                vgEditOk = vgEditOk
                    && QByteArray(m_audio.voicegroup()->voiceNames[dsSlot]) == donorName
                    && m_audio.transport() == Transport::Playing;
            }
            // Voice edits ride the song's undo stack; undoing them all must
            // land back on the exact on-disk state (clean, nothing written).
            for (int i = 0; i < undosNeeded; i++)
                tab->doc.undoStack()->undo();
            QByteArray fileAfter;
            {
                QFile in(tab->vgSource->filePath());
                in.open(QIODevice::ReadOnly);
                fileAfter = in.readAll();
            }
            vgEditOk = vgEditOk && !tab->vgSource->dirty() && !tab->doc.isDirty()
                && fileAfter == fileBefore
                && *tab->vgSource->voiceAt(dsSlot) == original
                && m_audio.voicegroup()->voices[dsSlot].release
                    == uint8_t(original.release)
                && QByteArray(m_audio.voicegroup()->voiceNames[dsSlot]) == originalName;
            if (vgEditOk)
                qInfo("selftest: voicegroup edit + preview reload + undo OK "
                      "(slot %d, donor %d)",
                      dsSlot, donorSlot);
            else
                qWarning("selftest: voicegroup edit FAILED (slot %d, donor %d)", dsSlot,
                         donorSlot);
        } else {
            qInfo("selftest: voicegroup edit skipped (no sample voices)");
        }
    } else {
        qInfo("selftest: voicegroup edit skipped (no editable source)");
    }

    // App settings: the global engine knobs (SPEC §7) re-applied mid-playback
    // through updateSettings — polyphony clamp, mix-rate rebuild (reverb delay
    // line resize), analog filter — must keep the transport running.
    {
        SongSettings tweaked = songSettingsFor(*tab);
        tweaked.maxPcmChannels = 8;
        tweaked.pcmMixRate = 21024.0f;
        tweaked.analogFilter = !tweaked.analogFilter;
        m_audio.updateSettings(tweaked);
        const uint64_t before = m_audio.playheadSamples();
        QTimer::singleShot(300, &loop, &QEventLoop::quit);
        loop.exec();
        const bool engineOk = m_audio.maxPcmChannels() == 8
            && m_audio.playheadSamples() != before
            && m_audio.transport() == Transport::Playing;
        m_audio.updateSettings(songSettingsFor(*tab));
        if (!engineOk) {
            qWarning("selftest: engine-settings update mid-playback FAILED "
                     "(maxPcm %d, transport %d)",
                     m_audio.maxPcmChannels(), int(m_audio.transport()));
            return false;
        }
        qInfo("selftest: engine-settings update mid-playback OK");
    }

    // M3: the onboarding UI must at least construct against a live project
    // (wizard pages enumerate voicegroups/players). Registration itself is
    // write-through now, exercised by --onboardcheck against a scratch copy.
    {
        NewSongWizard wizard(&m_project, this);
        EngineSettingsDialog engineDialog(m_engineSettings, this);
        qInfo("selftest: New Song wizard + engine settings dialog constructed");
    }

    const double playedSeconds = double(m_audio.playheadSamples()) / m_audio.sampleRate();
    qInfo("selftest: after 3s wall clock — playhead %.2fs, transport %d, PCM %d/%d active",
          playedSeconds, int(m_audio.transport()), m_audio.activePcmChannels(),
          m_audio.maxPcmChannels());

    bool ok = m_audio.transport() == Transport::Playing && playedSeconds > 1.0
        && m_audio.playheadSamples() >= posBeforeEdit && vgEditOk;
    if (ok) {
        pausePlayback();
        QTimer::singleShot(150, &loop, &QEventLoop::quit);
        loop.exec();
        const uint64_t pausedSample = m_audio.playheadSamples();
        const double pausedViewTick = tab->view->playheadTick();
        const double pausedEngineTick =
            m_audio.timeline()->tickForSample(pausedSample);
        constexpr double kPausedPlayheadToleranceTicks = 0.25;
        ok = std::abs(pausedViewTick - pausedEngineTick) <= kPausedPlayheadToleranceTicks;
        if (!ok) {
            qWarning("selftest: paused playhead reconciliation FAILED "
                     "(view %.3f ticks, engine %.3f ticks at %llu samples)",
                     pausedViewTick, pausedEngineTick,
                     static_cast<unsigned long long>(pausedSample));
        }
        startPlayback();
    }


    // M2 polish: edit-cursor seek mid-playback, then play-from-cursor out of
    // Stopped (both go through AudioEngine::seek + chase). Loop disabled so
    // a wrap can't drop the playhead below the seek target.
    if (ok) {
        m_audio.setLoopEnabled(false);
        const MidiTimeline *tl = m_audio.timeline();
        const uint64_t seekTick =
            std::min<uint64_t>(tl->lengthTicks / 2, uint64_t(tl->ticksPerBeat) * 16);
        const uint64_t seekSample = tl->sampleForTick(seekTick);
        tab->view->commitEditCursor(seekTick); // transport is Playing: seeks
        QTimer::singleShot(300, &loop, &QEventLoop::quit);
        loop.exec();
        const uint64_t afterSeek = m_audio.playheadSamples();
        stopPlayback();
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
        startPlayback(); // Stopped: must seek back to the edit cursor
        QTimer::singleShot(300, &loop, &QEventLoop::quit);
        loop.exec();
        const uint64_t afterRestart = m_audio.playheadSamples();
        ok = afterSeek >= seekSample && m_audio.transport() == Transport::Playing
            && afterRestart >= seekSample;
        if (ok)
            qInfo("selftest: edit-cursor seek + play-from-cursor OK (cursor %.2fs)",
                  double(seekSample) / m_audio.sampleRate());
        else
            qWarning("selftest: edit-cursor seek FAILED (cursor %.2fs, playhead %.2fs "
                     "after seek, %.2fs after restart)",
                     double(seekSample) / m_audio.sampleRate(),
                     double(afterSeek) / m_audio.sampleRate(),
                     double(afterRestart) / m_audio.sampleRate());

        // M2 polish: Space out of Paused restarts at the edit cursor rather
        // than resuming from the pause point (the Play button resumes). The
        // playhead had ~300ms past the cursor before the pause; a 150ms
        // check after the restart must land back inside that window.
        if (ok) {
            pausePlayback();
            QTimer::singleShot(200, &loop, &QEventLoop::quit);
            loop.exec();
            const uint64_t pausedAt = m_audio.playheadSamples();
            startPlayback(/*fromEditCursor=*/true); // the Space binding's path
            QTimer::singleShot(150, &loop, &QEventLoop::quit);
            loop.exec();
            const uint64_t afterSpace = m_audio.playheadSamples();
            ok = m_audio.transport() == Transport::Playing && afterSpace >= seekSample
                && afterSpace < pausedAt;
            if (ok)
                qInfo("selftest: Space-from-pause restarted at edit cursor OK "
                      "(paused %.2fs, restarted to %.2fs)",
                      double(pausedAt) / m_audio.sampleRate(),
                      double(afterSpace) / m_audio.sampleRate());
            else
                qWarning("selftest: Space-from-pause FAILED (cursor %.2fs, paused "
                         "%.2fs, playhead %.2fs after restart)",
                         double(seekSample) / m_audio.sampleRate(),
                         double(pausedAt) / m_audio.sampleRate(),
                         double(afterSpace) / m_audio.sampleRate());
        }
        m_audio.setLoopEnabled(true);
    }
    // M2 polish: sidecar view-state round trip (SPEC §4.4). Save the live
    // view, mutate it, then confirm applying the loaded state restores it —
    // and that SongRegistry's registration key coexists in the same file.
    if (ok) {
        SongRegistry::saveRegistrationMeta(m_project.root(), target->label,
                                           QStringLiteral("MUS_SELFTEST"),
                                           QStringLiteral("MUSIC_PLAYER_BGM"));
        tab->view->setGridMinDenom(8); // non-default grid must round-trip too
        tab->view->setGridFeel(SongView::GridFeel::Triplet);
        tab->view->setLaneDisplayRange(0, 0x01, 16); // MOD axis zoom, ditto
        const SongView::ViewState saved = tab->view->viewState();
        ok = ViewSidecar::save(m_project.root(), target->label, saved);
        tab->view->zoomAroundContentX(2.0, 0); // knock the view off the state
        tab->view->setGridMinDenom(0);
        tab->view->setGridFeel(SongView::GridFeel::Straight);
        tab->view->setLaneDisplayRange(0, 0x01, 0); // back to the MOD default
        SongView::ViewState loaded;
        ok = ok && ViewSidecar::load(m_project.root(), target->label, &loaded);
        if (ok) {
            tab->view->applyViewState(loaded);
            const SongView::ViewState restored = tab->view->viewState();
            QString constant, player;
            ok = std::abs(restored.pxPerBeat - saved.pxPerBeat) < 0.001
                && restored.keyHeight == saved.keyHeight
                && restored.scrollPx == saved.scrollPx
                && restored.selectedTrack == saved.selectedTrack
                && restored.editCursorTick == saved.editCursorTick
                && restored.laneHeight == saved.laneHeight
                && restored.gridMinDenom == 8
                && restored.gridTriplet
                && restored.laneRanges.value(QStringLiteral("cc:0:1"), -1) == 16
                && SongRegistry::loadRegistrationMeta(m_project.root(), target->label,
                                                      &constant, &player)
                && constant == QLatin1String("MUS_SELFTEST");
        }
        QFile::remove(ViewSidecar::pathFor(m_project.root(), target->label));
        tab->view->setGridMinDenom(0); // don't leak the test grid into a
        tab->view->setGridFeel(SongView::GridFeel::Straight); // shutdown save
        tab->view->setLaneDisplayRange(0, 0x01, 0); // nor the MOD axis zoom
        if (ok)
            qInfo("selftest: sidecar view-state round trip OK");
        else
            qWarning("selftest: sidecar view-state round trip FAILED");
    }
    if (ok) {
        destroySession(tab);
        ok = !m_playheadTimer->isActive();
        if (ok)
            qInfo("selftest: closing final tab stopped playhead timer");
        else
            qWarning("selftest: closing final tab left playhead timer active");
    }
    stopPlayback();
    qInfo("selftest: %s", ok ? "PASS" : "FAIL");
    return ok;
}

QString MainWindow::formatTime(uint64_t samples) const
{
    const double seconds = double(samples) / m_audio.sampleRate();
    const int mins = int(seconds) / 60;
    const int secs = int(seconds) % 60;
    const int tenths = int(seconds * 10) % 10;
    return QStringLiteral("%1:%2.%3")
        .arg(mins)
        .arg(secs, 2, 10, QLatin1Char('0'))
        .arg(tenths);
}
