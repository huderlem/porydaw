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
class SmfFile;
class SongView;
class VoicegroupBrowser;

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
    void newSong();
    void importMidi();
    void openRegistrationChecklist();
    void onDocumentChanged();
    void uiTick();

private:
    void buildUi();
    void populateSongList();
    void loadSong(const SongInfo &song);
    void loadSongByLabel(const QString &label);
    // Shared New Song / Import finish: writes the .mid + midi.cfg line and
    // the registration sidecar, reloads the project, loads the song, and
    // opens the registration checklist.
    void finishCreateSong(const SmfFile &smf, const QString &label,
                          const QString &constant, const QString &player,
                          const SongCfg &cfg);
    void reloadProject();
    void updateVoicegroupBrowser();
    // Prompts to save a dirty document; false = user cancelled the action.
    bool maybeSaveSong();
    LoadedVoiceGroup *loadVoicegroupFor(const SongCfg &cfg, QString *tried);
    // Starts (or resumes) playback; from Stopped, seeks to the edit cursor
    // first so playback begins there.
    void startPlayback();
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
    VoicegroupBrowser *m_vgBrowser = nullptr;
    QAction *m_newSongAction = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_checklistAction = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_playPauseAction = nullptr;
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
