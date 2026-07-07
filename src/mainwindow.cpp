#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
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
#include <QTimer>
#include <QToolBar>

#include <QCloseEvent>

#include <algorithm>
#include <cstring>

#include "audio/wavexport.h"
#include "core/miditimeline.h"
#include "project/songregistry.h"
#include "ui/newsongwizard.h"
#include "ui/songlistpanel.h"
#include "ui/songsettingsdialog.h"
#include "ui/songview.h"
#include "ui/viewsidecar.h"
#include "ui/voicegroupbrowser.h"

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
    m_uiTimer->setInterval(33);
    connect(m_uiTimer, &QTimer::timeout, this, &MainWindow::uiTick);
    m_uiTimer->start();

    updateTransportActions();
}

MainWindow::~MainWindow()
{
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
    fileMenu->addSeparator();
    m_exportWavAction = fileMenu->addAction(tr("Export &WAV..."), this,
                                            &MainWindow::exportWav);
    m_exportWavAction->setEnabled(false);
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction *undoAction = m_doc.undoStack()->createUndoAction(this, tr("&Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undoAction);
    QAction *redoAction = m_doc.undoStack()->createRedoAction(this, tr("&Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    m_settingsAction = editMenu->addAction(tr("Song Se&ttings..."), this,
                                           &MainWindow::openSongSettings);
    m_settingsAction->setEnabled(false);
    // Global GBA-accuracy knobs (SPEC §7); not song-scoped, so always enabled.
    editMenu->addAction(tr("&Engine Settings..."), this,
                        &MainWindow::openEngineSettings);

    connect(&m_doc, &SongDocument::documentChanged, this, &MainWindow::onDocumentChanged);
    connect(m_doc.undoStack(), &QUndoStack::cleanChanged, this,
            [this](bool) { updateWindowTitle(); });

    // Transport toolbar
    QToolBar *transport = addToolBar(tr("Transport"));
    // saveState() persists dock/toolbar layout by objectName only.
    transport->setObjectName(QStringLiteral("transportToolbar"));
    transport->setMovable(false);

    m_goToStartAction = new QAction(style()->standardIcon(QStyle::SP_MediaSkipBackward),
                                    tr("Go to Start"), this);
    m_goToStartAction->setShortcut(Qt::Key_Home);
    connect(m_goToStartAction, &QAction::triggered, this,
            [this] { m_songView->goToStart(); });
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
            m_audio.pause();
        else
            startPlayback(/*fromEditCursor=*/true);
    });
    addAction(m_playPauseAction);

    m_pauseAction = new QAction(style()->standardIcon(QStyle::SP_MediaPause), tr("Pause"), this);
    connect(m_pauseAction, &QAction::triggered, this, [this] { m_audio.pause(); });
    transport->addAction(m_pauseAction);

    m_stopAction = new QAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"), this);
    connect(m_stopAction, &QAction::triggered, this, [this] { m_audio.stop(); });
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
    dock->setWidget(m_songList);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    auto *findAction = new QAction(tr("Find Song"), this);
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this,
            [this] { m_songList->focusSearch(); });
    addAction(findAction);

    // Voicegroup browser dock (SPEC §6.1): the current song's instruments,
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
    connect(m_vgBrowser, &VoicegroupBrowser::voiceEdited, this,
            &MainWindow::onVoiceEdited);
    connect(m_vgBrowser, &VoicegroupBrowser::saveRequested, this,
            &MainWindow::saveVoicegroup);
    connect(m_vgBrowser, &VoicegroupBrowser::revertRequested, this,
            &MainWindow::revertVoicegroup);
    connect(m_vgBrowser, &VoicegroupBrowser::newVoicegroupRequested, this,
            &MainWindow::newVoicegroup);
    m_vgDock->setWidget(m_vgBrowser);
    addDockWidget(Qt::LeftDockWidgetArea, m_vgDock);

    // Song viewer: piano roll, automation lanes, other-events strip.
    m_songView = new SongView(this);
    connect(m_songView, &SongView::muteMaskChanged, this,
            [this](uint32_t mask) { m_audio.setMuteMask(mask); });
    connect(m_songView, &SongView::soloMaskChanged, this,
            [this](uint32_t mask) { m_audio.setSoloMask(mask); });
    connect(m_songView, &SongView::auditionNote, this,
            [this](int track, int key, int velocity) {
                if (m_audioOk && m_audio.songLoaded())
                    m_audio.previewNote(uint8_t(track), uint8_t(key), uint8_t(velocity));
            });
    connect(m_songView, &SongView::auditionVoice, this,
            [this](int voice, int key, int velocity) {
                if (m_audioOk)
                    m_audio.previewVoice(uint8_t(voice), uint8_t(key), uint8_t(velocity));
            });
    connect(m_songView, &SongView::statusMessage, this,
            [this](const QString &text) { statusBar()->showMessage(text, 6000); });
    // Moving the edit cursor while playing (or paused) seeks playback there,
    // chasing controller state to the landing position.
    connect(m_songView, &SongView::editCursorMoved, this, [this](uint64_t tick) {
        if (m_audioOk && m_audio.songLoaded() && m_audio.transport() != Transport::Stopped)
            m_audio.seek(m_audio.timeline()->sampleForTick(tick));
    });
    setCentralWidget(m_songView);

    // Status bar: polyphony meter
    m_polyLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_polyLabel);

    // Initial focus goes to the song list (via the panel's focus proxy), not
    // its filter box — first in tab order, which otherwise wins on show and
    // swallowed the first keystrokes into the search field.
    m_songList->setFocus();
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
    // Read before openProjectDir clears it for the fresh project.
    const QString songLabel =
        settings.value(QStringLiteral("lastSongLabel")).toString();
    if (!openProjectDir(dir, /*interactive=*/false))
        return;
    if (!songLabel.isEmpty())
        loadSongByLabel(songLabel);
}

bool MainWindow::openProjectDir(const QString &dir, bool interactive)
{
    QElapsedTimer timer;
    timer.start();

    if (!maybeSaveSong() || !maybeSaveVoicegroup())
        return false;
    saveViewState(); // against the old project root, before open() swaps it
    cleanupVgPreview();
    m_vgSource.reset();

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
    // The new project starts with no song loaded; loadSong re-records it.
    settings.remove(QStringLiteral("lastSongLabel"));

    m_songView->setDocument(nullptr);
    m_songView->setSong(nullptr, nullptr);
    m_vgBrowser->setVoicegroup(nullptr);
    m_audio.unloadSong();
    updateVgDockTitle();
    m_doc.undoStack()->clear();
    m_loadedSongId = -1;
    m_songLabel->clear();
    m_saveAction->setEnabled(false);
    m_exportWavAction->setEnabled(false);
    m_settingsAction->setEnabled(false);
    m_registerAction->setEnabled(false);
    m_newSongAction->setEnabled(true);
    m_importAction->setEnabled(true);
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

void MainWindow::loadSong(const SongInfo &song)
{
    if (!m_audioOk)
        return;
    if (!maybeSaveSong() || !maybeSaveVoicegroup())
        return;
    saveViewState(); // the outgoing song's, while its view is still up
    cleanupVgPreview();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();

    QString error;
    if (!m_doc.load(song, &error)) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Load Song"), error);
        return;
    }
    auto timeline = m_doc.buildTimeline(m_audio.sampleRate());

    QString tried;
    LoadedVoiceGroup *vg = loadVoicegroupFor(song.cfg, &tried);
    if (!vg) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Load Song"),
                             tr("Could not load the voicegroup for %1 (tried: %2).")
                                 .arg(song.label, tried));
        return;
    }

    // The views must let go of the old timeline/voicegroup before loadSong
    // frees them.
    m_songView->setDocument(nullptr);
    m_songView->setSong(nullptr, nullptr);
    m_vgBrowser->setVoicegroup(nullptr);
    m_audio.loadSong(std::move(timeline), vg, currentSongSettings());
    openVoicegroupSource(song.cfg);
    m_loadedSongId = song.id;
    m_appliedVoicegroupArg = song.cfg.voicegroupArg;
    m_appliedVolume = song.cfg.masterVolume;
    m_appliedReverb = song.cfg.reverb;
    if (m_persistSession)
        QSettings().setValue(QStringLiteral("lastSongLabel"), song.label);

    m_songView->setSong(m_audio.timeline(), m_audio.voicegroup());
    m_songView->setDocument(&m_doc);
    SongView::ViewState viewState;
    if (ViewSidecar::load(m_project.root(), song.label, &viewState))
        m_songView->applyViewState(viewState);
    updateVoicegroupBrowser();
    m_saveAction->setEnabled(true);
    m_exportWavAction->setEnabled(true);
    m_settingsAction->setEnabled(true);
    m_registerAction->setEnabled(!song.registered);
    updateWindowTitle();
    m_songLabel->setText(QStringLiteral("  %1").arg(song.label));
    m_songList->setCurrentSong(song.id);

    const MidiTimeline *tl = m_audio.timeline();
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

void MainWindow::onDocumentChanged()
{
    if (!m_audioOk || !m_audio.songLoaded())
        return;

    const SongCfg &cfg = m_doc.cfg();
    if (cfg.voicegroupArg != m_appliedVoicegroupArg) {
        // The -G switch replaces the edited voicegroup; the change can't be
        // blocked here, so the prompt offers Save/Discard only.
        maybeSaveVoicegroup(/*allowCancel=*/false);
        cleanupVgPreview();
        QString tried;
        if (LoadedVoiceGroup *vg = loadVoicegroupFor(cfg, &tried)) {
            m_songView->setVoicegroup(nullptr);
            m_vgBrowser->setVoicegroup(nullptr);
            m_audio.updateVoicegroup(vg);
            openVoicegroupSource(cfg);
            m_songView->setVoicegroup(m_audio.voicegroup());
            updateVoicegroupBrowser();
            m_appliedVoicegroupArg = cfg.voicegroupArg;
        } else {
            statusBar()->showMessage(
                tr("Voicegroup not found (tried: %1) — keeping the previous one until save.")
                    .arg(tried),
                8000);
        }
    }
    if (cfg.masterVolume != m_appliedVolume || cfg.reverb != m_appliedReverb) {
        m_audio.updateSettings(currentSongSettings());
        m_appliedVolume = cfg.masterVolume;
        m_appliedReverb = cfg.reverb;
    }

    m_audio.updateTimeline(m_doc.buildTimeline(m_audio.sampleRate()));
    m_songView->updateSong(m_audio.timeline());
    updateWindowTitle();
}

void MainWindow::saveSong()
{
    if (m_loadedSongId < 0)
        return;
    QString error;
    if (!m_doc.save(&error)) {
        QMessageBox::warning(this, tr("Save Song"), error);
        return;
    }
    m_project.setSongCfg(m_loadedSongId, m_doc.cfg());
    statusBar()->showMessage(tr("Saved %1").arg(m_doc.midPath()), 5000);
    updateWindowTitle();
}

void MainWindow::exportWav()
{
    if (m_loadedSongId < 0 || !m_audio.songLoaded())
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
                                        QFileInfo(m_doc.midPath()).path())
                                 .toString();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export WAV"), startDir + QLatin1Char('/') + m_doc.label() + ".wav",
        tr("WAV files (*.wav)"));
    if (path.isEmpty())
        return;
    appSettings.setValue(QStringLiteral("lastWavExportDir"), QFileInfo(path).path());

    m_audio.stop();

    QProgressDialog progress(tr("Rendering %1...").arg(m_doc.label()), tr("Cancel"), 0,
                             1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);

    // The render reads the same voicegroup the audio engine holds (the
    // engine only reads it too), against a fresh timeline at the export
    // rate — so unsaved document and voice edits export as heard.
    auto timeline = m_doc.buildTimeline(double(opts.sampleRate));
    QString error;
    const bool ok = ::exportWav(path, *timeline, m_audio.voicegroup(),
                                currentSongSettings(), opts,
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
    if (m_loadedSongId < 0)
        return;
    SongSettingsDialog dialog(m_doc.cfg(), m_doc.label(),
                              SongRegistry::voicegroupArgs(m_project.root()), this);
    if (dialog.exec() == QDialog::Accepted)
        m_doc.setCfg(dialog.cfg());
}

void MainWindow::openEngineSettings()
{
    EngineSettingsDialog dialog(m_engineSettings, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_engineSettings = dialog.settings();
    m_engineSettings.save();
    if (m_audioOk && m_audio.songLoaded())
        m_audio.updateSettings(currentSongSettings());
}

SongSettings MainWindow::currentSongSettings() const
{
    const SongCfg &cfg = m_doc.cfg();
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
    NewSongWizard wizard(&m_project, &m_audio, this);
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
    NewSongWizard wizard(&m_project, &m_audio, std::move(smf), path, this);
    if (wizard.exec() != QDialog::Accepted)
        return;
    finishCreateSong(wizard.songFile(), wizard.label(), wizard.constant(),
                     wizard.player(), wizard.cfg(), wizard.newVoicegroupName());
}

void MainWindow::finishCreateSong(const SmfFile &smf, const QString &label,
                                  const QString &constant, const QString &player,
                                  const SongCfg &cfg, const QString &newVoicegroup)
{
    if (!maybeSaveSong() || !maybeSaveVoicegroup())
        return;

    QString error;
    // The voicegroup first: the song's -G already points at it, so nothing
    // else may be written if it can't exist. Starts as the dummy template —
    // the user configures it in the Voicegroup dock.
    if (!newVoicegroup.isEmpty()) {
        if (!confirmVoicegroupWrite())
            return;
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
    loadSongByLabel(label);
}

void MainWindow::registerLoadedSong()
{
    if (m_loadedSongId < 0 || m_loadedSongId >= m_project.songs().size())
        return;
    const SongInfo song = m_project.songs().at(m_loadedSongId);
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
    reloadProject();
    loadSongByLabel(song.label);
}

void MainWindow::reloadProject()
{
    QString error;
    if (!m_project.reload(&error)) {
        QMessageBox::warning(this, tr("Reload Project"), error);
        return;
    }
    populateSongList();
}

void MainWindow::loadSongByLabel(const QString &label)
{
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == label && song.isPlayable()) {
            loadSong(song);
            return;
        }
    }
}

void MainWindow::updateVoicegroupBrowser()
{
    if (!m_audio.voicegroup()) {
        m_vgBrowser->setVoicegroup(nullptr);
        updateVgDockTitle();
        return;
    }
    const QString arg = m_doc.cfg().voicegroupArg.isEmpty()
                            ? QStringLiteral("_dummy")
                            : m_doc.cfg().voicegroupArg;
    m_vgBrowser->setVoicegroup(m_audio.voicegroup(),
                               QStringLiteral("voicegroup%1").arg(arg));
    m_vgBrowser->setSource(m_vgSource.get(),
                           VoicegroupSource::directSoundSymbols(m_project.root()),
                           VoicegroupSource::progWaveSymbols(m_project.root()),
                           VoicegroupSource::keysplitInstruments(m_project.root()),
                           VoicegroupSource::drumkitInstruments(m_project.root()),
                           VoicegroupSource::typicalAdsr(m_project.root()));
    updateVgDockTitle();
}

void MainWindow::openVoicegroupSource(const SongCfg &cfg)
{
    m_vgSource = std::make_unique<VoicegroupSource>();
    QString error;
    if (!m_vgSource->open(m_project.root(), cfg.voicegroupArg, &error)) {
        m_vgSource.reset();
        statusBar()->showMessage(
            tr("Voicegroup editing unavailable: %1").arg(error), 8000);
    }
}

void MainWindow::onVoiceEdited(int slot, bool structural)
{
    if (!m_vgSource)
        return;
    if (structural) {
        reloadVoicegroupPreview(slot);
    } else {
        m_vgSource->applyScalarsToToneData(slot, m_audio.voiceForEdit(slot));
        // Playing tracks hold a copy of their instrument; refresh so the
        // edit is heard from the next note without a pause/play cycle.
        m_audio.refreshVoices();
    }
    updateVgDockTitle();
}

void MainWindow::reloadVoicegroupPreview(int keepSlot)
{
    const QString previewDir =
        m_project.root() + QStringLiteral("/.porydaw/vgpreview");
    QDir().mkpath(previewDir);
    {
        QFile out(previewDir + QLatin1Char('/') + m_vgSource->loadName()
                  + QStringLiteral(".inc"));
        if (!out.open(QIODevice::WriteOnly)) {
            statusBar()->showMessage(tr("Cannot write voicegroup preview file."), 8000);
            return;
        }
        out.write(m_vgSource->renderPreview());
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
                        m_vgSource->loadName().toLocal8Bit().constData(), &config);
    if (!vg) {
        statusBar()->showMessage(
            tr("Edited voicegroup failed to load — keeping the previous sound."), 8000);
        return;
    }
    swapVoicegroup(vg, keepSlot);
}

void MainWindow::swapVoicegroup(LoadedVoiceGroup *vg, int keepSlot)
{
    m_songView->setVoicegroup(nullptr);
    m_vgBrowser->setVoicegroup(nullptr);
    m_audio.updateVoicegroup(vg);
    m_songView->setVoicegroup(m_audio.voicegroup());
    updateVoicegroupBrowser();
    m_vgBrowser->selectSlot(keepSlot);
}

void MainWindow::saveVoicegroup()
{
    if (!m_vgSource || !m_vgSource->dirty())
        return;
    if (!confirmVoicegroupWrite())
        return;
    QString error;
    if (!m_vgSource->save(&error)) {
        QMessageBox::warning(this, tr("Save Voicegroup"), error);
        return;
    }
    cleanupVgPreview();
    // Reload from the project: verifies the saved file parses and replaces
    // any preview-loaded state.
    const int slot = m_vgBrowser->currentSlot();
    QString tried;
    if (LoadedVoiceGroup *vg = loadVoicegroupFor(m_doc.cfg(), &tried))
        swapVoicegroup(vg, slot);
    statusBar()->showMessage(tr("Saved %1").arg(m_vgSource->filePath()), 5000);
    updateVgDockTitle();
}

void MainWindow::revertVoicegroup()
{
    if (!m_vgSource)
        return;
    QString error;
    if (!m_vgSource->reload(&error)) {
        QMessageBox::warning(this, tr("Revert Voicegroup"), error);
        return;
    }
    cleanupVgPreview();
    const int slot = m_vgBrowser->currentSlot();
    QString tried;
    if (LoadedVoiceGroup *vg = loadVoicegroupFor(m_doc.cfg(), &tried))
        swapVoicegroup(vg, slot);
    updateVgDockTitle();
}

bool MainWindow::maybeSaveVoicegroup(bool allowCancel)
{
    if (!m_vgSource || !m_vgSource->dirty())
        return true;
    QMessageBox::StandardButtons buttons = QMessageBox::Save | QMessageBox::Discard;
    if (allowCancel)
        buttons |= QMessageBox::Cancel;
    const auto choice = QMessageBox::question(
        this, tr("Unsaved Voicegroup Changes"),
        tr("The voicegroup %1 has unsaved voice edits. Save them?")
            .arg(QFileInfo(m_vgSource->filePath()).fileName()),
        buttons, QMessageBox::Save);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save) {
        if (!confirmVoicegroupWrite())
            return false;
        QString error;
        if (!m_vgSource->save(&error)) {
            QMessageBox::warning(this, tr("Save Voicegroup"), error);
            return false;
        }
    }
    return true;
}

bool MainWindow::confirmVoicegroupWrite()
{
    QSettings settings;
    const QString key = QStringLiteral("allowVoicegroupWrites");
    if (settings.value(key, false).toBool())
        return true;
    QMessageBox box(QMessageBox::Question, tr("Edit Voicegroup Files?"),
                    tr("porydaw will rewrite only the edited voice lines of the "
                       "voicegroup file — every other byte is preserved.\n\n"
                       "Allow porydaw to edit voicegroup files in this project?"),
                    QMessageBox::Yes | QMessageBox::No, this);
    auto *dontAsk = new QCheckBox(tr("Don't ask again"), &box);
    box.setCheckBox(dontAsk);
    if (box.exec() != QMessageBox::Yes)
        return false;
    if (dontAsk->isChecked())
        settings.setValue(key, true);
    return true;
}

void MainWindow::cleanupVgPreview()
{
    if (!m_project.isOpen())
        return;
    QDir(m_project.root() + QStringLiteral("/.porydaw/vgpreview")).removeRecursively();
}

void MainWindow::updateVgDockTitle()
{
    const bool dirty = m_vgSource && m_vgSource->dirty();
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

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Voicegroup"));
    auto *form = new QFormLayout(&dialog);
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z][A-Za-z0-9_]*")), nameEdit));
    form->addRow(tr("Name"), nameEdit);
    auto *sourceCombo = new QComboBox(&dialog);
    if (m_vgSource)
        sourceCombo->addItem(tr("Copy of %1")
                                 .arg(QFileInfo(m_vgSource->filePath()).fileName()),
                             m_vgSource->filePath());
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
    if (!confirmVoicegroupWrite())
        return;

    const QString copyFrom = sourceCombo->currentData().toString();
    const QString sectionLabel =
        (!copyFrom.isEmpty() && m_vgSource && copyFrom == m_vgSource->filePath())
        ? m_vgSource->sectionLabel()
        : QString();
    QString error;
    if (!VoicegroupSource::createVoicegroup(m_project.root(), name, copyFrom,
                                            sectionLabel, &error)
        || !VoicegroupSource::appendIncludeLine(m_project.root(), name, &error)) {
        QMessageBox::warning(this, tr("New Voicegroup"), error);
        return;
    }
    statusBar()->showMessage(
        tr("Created sound/voicegroups/%1.inc — assign it to a song in Song "
           "Settings (voicegroup _%1).")
            .arg(name),
        10000);
}

bool MainWindow::maybeSaveSong()
{
    if (m_loadedSongId < 0 || !m_doc.isDirty())
        return true;
    const auto choice = QMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("%1 has unsaved changes. Save them?").arg(m_doc.label()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save) {
        QString error;
        if (!m_doc.save(&error)) {
            QMessageBox::warning(this, tr("Save Song"), error);
            return false;
        }
        m_project.setSongCfg(m_loadedSongId, m_doc.cfg());
    }
    return true;
}

void MainWindow::saveViewState()
{
    if (m_loadedSongId < 0)
        return;
    ViewSidecar::save(m_project.root(), m_doc.label(), m_songView->viewState());
}

void MainWindow::updateWindowTitle()
{
    const QString project =
        m_project.isOpen() ? QDir(m_project.root()).dirName() : QString();
    if (m_loadedSongId >= 0) {
        setWindowTitle(
            QStringLiteral("%1[*] — %2 — porydaw").arg(m_doc.label(), project));
        setWindowModified(m_doc.isDirty());
    } else {
        setWindowTitle(project.isEmpty()
                           ? QStringLiteral("porydaw")
                           : QStringLiteral("%1 — porydaw").arg(project));
        setWindowModified(false);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSaveSong() && maybeSaveVoicegroup()) {
        saveViewState();
        cleanupVgPreview();
        if (m_persistSession) {
            QSettings settings;
            settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
            settings.setValue(QStringLiteral("windowState"), saveState());
        }
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::uiTick()
{
    if (!m_audioOk)
        return;

    if (m_audio.songLoaded()) {
        const uint64_t length = m_audio.timeline()->lengthSamples;
        m_timeLabel->setText(QStringLiteral("%1 / %2")
                                 .arg(formatTime(m_audio.playheadSamples()),
                                      formatTime(length)));
        m_songView->setPlayheadSample(m_audio.playheadSamples(),
                                      m_audio.transport() == Transport::Playing);

        const uint64_t lost = m_audio.polyLostTotal();
        QString poly = tr("PCM %1/%2 · CGB %3/4")
                           .arg(m_audio.activePcmChannels())
                           .arg(m_audio.maxPcmChannels())
                           .arg(m_audio.activeCgbChannels());
        if (lost > 0)
            poly += tr(" · %1 notes lost").arg(lost);
        m_polyLabel->setText(poly);
    }
    updateTransportActions();
}

void MainWindow::startPlayback(bool fromEditCursor)
{
    if (!m_audioOk || !m_audio.songLoaded())
        return;
    if (fromEditCursor || m_audio.transport() == Transport::Stopped)
        m_audio.seek(m_audio.timeline()->sampleForTick(m_songView->editCursorTick()));
    m_audio.play();
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
    if (!m_audio.songLoaded()) {
        qWarning("selftest: song failed to load");
        return false;
    }
    qInfo("selftest: loaded %s (%zu events, %d tracks)", qUtf8Printable(target->label),
          m_audio.timeline()->events.size(), m_audio.timeline()->usedTrackCount);

    m_audio.play();
    QEventLoop loop;
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();

    // M2: edit during playback — exercises the documentChanged plumbing
    // (timeline rebuild, playhead-preserving audio swap, view refresh).
    const uint64_t posBeforeEdit = m_audio.playheadSamples();
    m_doc.addNote(m_songView->selectedTrack(), 0, 60, 24, 100);
    m_doc.addLanePoint(m_songView->selectedTrack(), 7, 0, 100);
    if (!m_doc.isDirty()) {
        qWarning("selftest: document not dirty after edits");
        return false;
    }
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    m_doc.undoStack()->undo();
    m_doc.undoStack()->undo();
    if (m_doc.isDirty()) {
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

    // Voicegroup editing: a scalar edit pokes the live ToneData, a sample
    // swap goes through the .porydaw/vgpreview shadow reload, and Revert
    // restores the on-disk state — all without changing project files.
    bool vgEditOk = true;
    if (m_vgSource) {
        int dsSlot = -1, donorSlot = -1;
        for (int i = 0; i < VOICEGROUP_SIZE; i++) {
            const VgVoice *v = m_vgSource->voiceAt(i);
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
            else if (donorSlot < 0 && v->symbol != m_vgSource->voiceAt(dsSlot)->symbol)
                donorSlot = i;
        }
        if (dsSlot >= 0) {
            m_vgBrowser->selectSlot(dsSlot); // exercises the editor panel too
            QByteArray fileBefore;
            {
                QFile in(m_vgSource->filePath());
                in.open(QIODevice::ReadOnly);
                fileBefore = in.readAll();
            }
            VgVoice v = *m_vgSource->voiceAt(dsSlot);
            v.release = v.release == 25 ? 26 : 25;
            m_vgSource->setVoice(dsSlot, v);
            onVoiceEdited(dsSlot, false);
            vgEditOk = m_vgSource->dirty()
                && m_audio.voicegroup()->voices[dsSlot].release == uint8_t(v.release);
            if (donorSlot >= 0) {
                const QByteArray donorName(m_audio.voicegroup()->voiceNames[donorSlot]);
                v.symbol = m_vgSource->voiceAt(donorSlot)->symbol;
                m_vgSource->setVoice(dsSlot, v);
                onVoiceEdited(dsSlot, true);
                vgEditOk = vgEditOk
                    && QByteArray(m_audio.voicegroup()->voiceNames[dsSlot]) == donorName
                    && m_audio.transport() == Transport::Playing;
            }
            revertVoicegroup();
            QByteArray fileAfter;
            {
                QFile in(m_vgSource->filePath());
                in.open(QIODevice::ReadOnly);
                fileAfter = in.readAll();
            }
            vgEditOk = vgEditOk && !m_vgSource->dirty() && fileAfter == fileBefore
                && !QDir(m_project.root() + QStringLiteral("/.porydaw/vgpreview")).exists();
            if (vgEditOk)
                qInfo("selftest: voicegroup edit + preview reload + revert OK "
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
        SongSettings tweaked = currentSongSettings();
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
        m_audio.updateSettings(currentSongSettings());
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
        NewSongWizard wizard(&m_project, &m_audio, this);
        EngineSettingsDialog engineDialog(m_engineSettings, this);
        qInfo("selftest: New Song wizard + engine settings dialog constructed");
    }

    const double playedSeconds = double(m_audio.playheadSamples()) / m_audio.sampleRate();
    qInfo("selftest: after 3s wall clock — playhead %.2fs, transport %d, PCM %d/%d active",
          playedSeconds, int(m_audio.transport()), m_audio.activePcmChannels(),
          m_audio.maxPcmChannels());

    bool ok = m_audio.transport() == Transport::Playing && playedSeconds > 1.0
        && m_audio.playheadSamples() >= posBeforeEdit && vgEditOk;

    // M2 polish: edit-cursor seek mid-playback, then play-from-cursor out of
    // Stopped (both go through AudioEngine::seek + chase). Loop disabled so
    // a wrap can't drop the playhead below the seek target.
    if (ok) {
        m_audio.setLoopEnabled(false);
        const MidiTimeline *tl = m_audio.timeline();
        const uint64_t seekTick =
            std::min<uint64_t>(tl->lengthTicks / 2, uint64_t(tl->ticksPerBeat) * 16);
        const uint64_t seekSample = tl->sampleForTick(seekTick);
        m_songView->commitEditCursor(seekTick); // transport is Playing: seeks
        QTimer::singleShot(300, &loop, &QEventLoop::quit);
        loop.exec();
        const uint64_t afterSeek = m_audio.playheadSamples();
        m_audio.stop();
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
            m_audio.pause();
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
        const SongView::ViewState saved = m_songView->viewState();
        ok = ViewSidecar::save(m_project.root(), target->label, saved);
        m_songView->zoomAroundContentX(2.0, 0); // knock the view off the state
        SongView::ViewState loaded;
        ok = ok && ViewSidecar::load(m_project.root(), target->label, &loaded);
        if (ok) {
            m_songView->applyViewState(loaded);
            const SongView::ViewState restored = m_songView->viewState();
            QString constant, player;
            ok = std::abs(restored.pxPerBeat - saved.pxPerBeat) < 0.001
                && restored.keyHeight == saved.keyHeight
                && restored.scrollPx == saved.scrollPx
                && restored.selectedTrack == saved.selectedTrack
                && restored.editCursorTick == saved.editCursorTick
                && restored.laneHeight == saved.laneHeight
                && SongRegistry::loadRegistrationMeta(m_project.root(), target->label,
                                                      &constant, &player)
                && constant == QLatin1String("MUS_SELFTEST");
        }
        QFile::remove(ViewSidecar::pathFor(m_project.root(), target->label));
        if (ok)
            qInfo("selftest: sidecar view-state round trip OK");
        else
            qWarning("selftest: sidecar view-state round trip FAILED");
    }
    m_audio.stop();
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
