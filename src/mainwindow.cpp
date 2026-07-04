#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>

#include "core/miditimeline.h"

namespace {
enum TrackColumn {
    ColTrack = 0,
    ColName,
    ColNotes,
    ColMute,
    ColSolo,
    ColCount,
};
} // namespace

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

    // Track table
    m_trackTable = new QTableWidget(0, ColCount, this);
    m_trackTable->setHorizontalHeaderLabels(
        {tr("Track"), tr("Name"), tr("Notes"), tr("Mute"), tr("Solo")});
    m_trackTable->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_trackTable->verticalHeader()->setVisible(false);
    m_trackTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_trackTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_trackTable, &QTableWidget::cellChanged, this, &MainWindow::trackTableChanged);
    setCentralWidget(m_trackTable);

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

    m_audio.unloadSong();
    m_loadedSongId = -1;
    m_songLabel->clear();
    m_trackTable->setRowCount(0);
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
    m_audio.loadSong(std::move(timeline), vg, settings);
    m_loadedSongId = song.id;

    populateTrackTable();
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

void MainWindow::populateTrackTable()
{
    const MidiTimeline *tl = m_audio.timeline();
    m_updatingTable = true;
    m_trackTable->setRowCount(0);
    if (tl) {
        for (int t = 0; t < 16; t++) {
            const TimelineTrack &track = tl->tracks[t];
            if (!track.used)
                continue;
            const int row = m_trackTable->rowCount();
            m_trackTable->insertRow(row);

            auto *numItem = new QTableWidgetItem(QString::number(t + 1));
            numItem->setData(Qt::UserRole, t);
            m_trackTable->setItem(row, ColTrack, numItem);

            QString name = track.name;
            if (name.isEmpty())
                name = tr("Track %1").arg(t + 1);
            m_trackTable->setItem(row, ColName, new QTableWidgetItem(name));
            m_trackTable->setItem(row, ColNotes,
                                  new QTableWidgetItem(QString::number(track.noteCount)));

            auto *muteItem = new QTableWidgetItem();
            muteItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            muteItem->setCheckState(Qt::Unchecked);
            m_trackTable->setItem(row, ColMute, muteItem);

            auto *soloItem = new QTableWidgetItem();
            soloItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            soloItem->setCheckState(Qt::Unchecked);
            m_trackTable->setItem(row, ColSolo, soloItem);
        }
    }
    m_updatingTable = false;
}

void MainWindow::trackTableChanged(int row, int column)
{
    Q_UNUSED(row);
    if (m_updatingTable || (column != ColMute && column != ColSolo))
        return;

    uint32_t muteMask = 0;
    uint32_t soloMask = 0;
    for (int r = 0; r < m_trackTable->rowCount(); r++) {
        const int track = m_trackTable->item(r, ColTrack)->data(Qt::UserRole).toInt();
        if (m_trackTable->item(r, ColMute)->checkState() == Qt::Checked)
            muteMask |= 1u << track;
        if (m_trackTable->item(r, ColSolo)->checkState() == Qt::Checked)
            soloMask |= 1u << track;
    }
    m_audio.setMuteMask(muteMask);
    m_audio.setSoloMask(soloMask);
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
