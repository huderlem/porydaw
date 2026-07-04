#pragma once

#include <QMainWindow>

#include "audio/audioengine.h"
#include "core/songdocument.h"
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

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openProject();
    void songActivated(QListWidgetItem *item);
    void saveSong();
    void openSongSettings();
    void onDocumentChanged();
    void uiTick();

private:
    void buildUi();
    void populateSongList();
    void loadSong(const SongInfo &song);
    // Prompts to save a dirty document; false = user cancelled the action.
    bool maybeSaveSong();
    LoadedVoiceGroup *loadVoicegroupFor(const SongCfg &cfg, QString *tried);
    void updateTransportActions();
    void updateWindowTitle();
    QString formatTime(uint64_t samples) const;

    AudioEngine m_audio;
    bool m_audioOk = false;
    DecompProject m_project;
    SongDocument m_doc;
    int m_loadedSongId = -1;
    // Engine-applied cfg values, to react only to real changes on edits.
    QString m_appliedVoicegroupArg;
    int m_appliedVolume = 127;
    int m_appliedReverb = -1;

    QListWidget *m_songList = nullptr;
    SongView *m_songView = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_loopAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_songLabel = nullptr;
    QLabel *m_polyLabel = nullptr;
    QTimer *m_uiTimer = nullptr;
};
