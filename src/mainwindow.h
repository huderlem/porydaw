#pragma once

#include <QMainWindow>

#include "audio/audioengine.h"
#include "project/decompproject.h"

class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTimer;
class SongView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Headless smoke test (--selftest <projectRoot> <songLabel>): opens the
    // project, loads the song, plays ~3 seconds through the real audio path,
    // and reports whether the playhead advanced.
    bool runSelfTest(const QString &projectRoot, const QString &songLabel);

private slots:
    void openProject();
    void songActivated(QListWidgetItem *item);
    void uiTick();

private:
    void buildUi();
    void populateSongList();
    void loadSong(const SongInfo &song);
    void updateTransportActions();
    QString formatTime(uint64_t samples) const;

    AudioEngine m_audio;
    bool m_audioOk = false;
    DecompProject m_project;
    int m_loadedSongId = -1;

    QListWidget *m_songList = nullptr;
    SongView *m_songView = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_loopAction = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_songLabel = nullptr;
    QLabel *m_polyLabel = nullptr;
    QTimer *m_uiTimer = nullptr;
};
