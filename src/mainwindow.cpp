#include "mainwindow.h"

#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>

#include "core/miditimeline.h"
#include "ui/songview.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("porydaw"));
    resize(1100, 680);
    buildUi();

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
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quitAction->setShortcut(QKeySequence::Quit);

    // Transport toolbar
    QToolBar *transport = addToolBar(tr("Transport"));
    transport->setMovable(false);

    m_playAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), this);
    m_playAction->setShortcut(Qt::Key_Space);
    connect(m_playAction, &QAction::triggered, this, [this] { m_audio.play(); });
    transport->addAction(m_playAction);

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
    dock->setFeatures(QDockWidget::DockWidgetMovable);
    m_songList = new QListWidget(dock);
    connect(m_songList, &QListWidget::itemActivated, this, &MainWindow::songActivated);
    dock->setWidget(m_songList);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    // Song viewer: piano roll, automation lanes, other-events strip.
    m_songView = new SongView(this);
    connect(m_songView, &SongView::muteMaskChanged, this,
            [this](uint32_t mask) { m_audio.setMuteMask(mask); });
    connect(m_songView, &SongView::soloMaskChanged, this,
            [this](uint32_t mask) { m_audio.setSoloMask(mask); });
    setCentralWidget(m_songView);

    // Status bar: polyphony meter
    m_polyLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_polyLabel);
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

    QElapsedTimer timer;
    timer.start();

    QString error;
    if (!m_project.open(dir, &error)) {
        QMessageBox::warning(this, tr("Open Project"), error);
        return;
    }
    settings.setValue(QStringLiteral("lastProjectDir"), dir);

    m_songView->setSong(nullptr, nullptr);
    m_audio.unloadSong();
    m_loadedSongId = -1;
    m_songLabel->clear();
    populateSongList();

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
}

void MainWindow::populateSongList()
{
    m_songList->clear();
    for (const SongInfo &song : m_project.songs()) {
        if (!song.isPlayable())
            continue;
        QString text = song.label;
        if (!song.constant.isEmpty())
            text += QStringLiteral("  (%1)").arg(song.constant);
        auto *item = new QListWidgetItem(text, m_songList);
        item->setData(Qt::UserRole, song.id);
    }
}

void MainWindow::songActivated(QListWidgetItem *item)
{
    const int songId = item->data(Qt::UserRole).toInt();
    if (songId < 0 || songId >= m_project.songs().size())
        return;
    loadSong(m_project.songs().at(songId));
}

void MainWindow::loadSong(const SongInfo &song)
{
    if (!m_audioOk)
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QElapsedTimer timer;
    timer.start();

    QString error;
    auto timeline = MidiTimeline::load(song.midPath, m_audio.sampleRate(), &error);
    if (!timeline) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Load Song"), error);
        return;
    }

    LoadedVoiceGroup *vg = nullptr;
    const QStringList candidates = DecompProject::voicegroupCandidates(song);
    const QByteArray rootUtf8 = m_project.root().toLocal8Bit();
    for (const QString &name : candidates) {
        vg = voicegroup_load(rootUtf8.constData(), name.toLocal8Bit().constData(), nullptr);
        if (vg)
            break;
    }
    if (!vg) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(
            this, tr("Load Song"),
            tr("Could not load the voicegroup for %1 (tried: %2).")
                .arg(song.label, candidates.join(QStringLiteral(", "))));
        return;
    }

    SongSettings settings;
    settings.songVolume = uint8_t(song.cfg.masterVolume);
    settings.reverb = uint8_t(song.cfg.reverb > 0 ? song.cfg.reverb : 0);
    // The view must let go of the old timeline before loadSong frees it.
    m_songView->setSong(nullptr, nullptr);
    m_audio.loadSong(std::move(timeline), vg, settings);
    m_loadedSongId = song.id;

    m_songView->setSong(m_audio.timeline(), m_audio.voicegroup());
    m_songLabel->setText(QStringLiteral("  %1").arg(song.label));

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

void MainWindow::updateTransportActions()
{
    const bool loaded = m_audioOk && m_audio.songLoaded();
    const Transport t = m_audio.transport();
    m_playAction->setEnabled(loaded && t != Transport::Playing);
    m_pauseAction->setEnabled(loaded && t == Transport::Playing);
    m_stopAction->setEnabled(loaded && t != Transport::Stopped);
    m_loopAction->setEnabled(loaded);
}

bool MainWindow::runSelfTest(const QString &projectRoot, const QString &songLabel)
{
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
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();

    const double playedSeconds = double(m_audio.playheadSamples()) / m_audio.sampleRate();
    qInfo("selftest: after 3s wall clock — playhead %.2fs, transport %d, PCM %d/%d active",
          playedSeconds, int(m_audio.transport()), m_audio.activePcmChannels(),
          m_audio.maxPcmChannels());

    const bool ok = m_audio.transport() == Transport::Playing && playedSeconds > 1.0;
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
