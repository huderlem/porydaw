#pragma once

#include <QMainWindow>

#include <memory>

#include "audio/audioengine.h"
#include "core/songdocument.h"
#include "project/decompproject.h"
#include "project/voicegroupsource.h"
#include "ui/enginesettingsdialog.h"

class QAction;
class QDockWidget;
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
    void exportWav();
    void openSongSettings();
    void openEngineSettings();
    void newSong();
    void importMidi();
    void registerLoadedSong();
    void onDocumentChanged();
    void uiTick();
    void onVoiceEdited(int slot, bool structural);
    void saveVoicegroup();
    void revertVoicegroup();
    void newVoicegroup();

private:
    void buildUi();
    void populateSongList();
    void loadSong(const SongInfo &song);
    void loadSongByLabel(const QString &label);
    // Shared New Song / Import finish: creates the new voicegroup when the
    // wizard asked for one, writes the .mid + midi.cfg line, registers the
    // song in the three registration files, reloads the project, and loads
    // the song.
    void finishCreateSong(const SmfFile &smf, const QString &label,
                          const QString &constant, const QString &player,
                          const SongCfg &cfg, const QString &newVoicegroup);
    void reloadProject();
    void updateVoicegroupBrowser();
    // Prompts to save a dirty document; false = user cancelled the action.
    bool maybeSaveSong();
    // Same for unsaved voicegroup edits. allowCancel=false (the in-document
    // -G switch, which can't be blocked) prompts Save/Discard only.
    bool maybeSaveVoicegroup(bool allowCancel = true);
    // Locates + parses the source behind the loaded voicegroup (nullptr on
    // exotic layouts — the editor degrades to read-only).
    void openVoicegroupSource(const SongCfg &cfg);
    // Auditions unsaved structural edits: renders the edited source into
    // .porydaw/vgpreview/ and reloads through the loader's config override,
    // which shadows the real file without touching it.
    void reloadVoicegroupPreview(int keepSlot);
    // Swaps in a freshly loaded voicegroup, reattaching the views around it.
    void swapVoicegroup(LoadedVoiceGroup *vg, int keepSlot);
    // One-time (per "don't ask again") confirmation that porydaw may write
    // voicegroup files (SPEC §5.3).
    bool confirmVoicegroupWrite();
    void cleanupVgPreview();
    void updateVgDockTitle();
    // Sidecar view state (SPEC §4.4): written whenever the loaded song is
    // let go (song switch, project switch, app close). Cosmetic; silent on
    // failure.
    void saveViewState();
    LoadedVoiceGroup *loadVoicegroupFor(const SongCfg &cfg, QString *tried);
    // Starts (or resumes) playback; from Stopped, seeks to the edit cursor
    // first so playback begins there. fromEditCursor forces that seek even
    // out of Paused — the Space binding (Reaper-style restart), while the
    // Play button resumes from the pause point.
    void startPlayback(bool fromEditCursor = false);
    // The loaded document's cfg (volume/reverb) merged with the global
    // engine knobs — everything AudioEngine::updateSettings applies.
    SongSettings currentSongSettings() const;
    void updateTransportActions();
    void updateWindowTitle();
    QString formatTime(uint64_t samples) const;

    AudioEngine m_audio;
    bool m_audioOk = false;
    EngineSettings m_engineSettings;
    DecompProject m_project;
    SongDocument m_doc;
    std::unique_ptr<VoicegroupSource> m_vgSource;
    int m_loadedSongId = -1;
    // Engine-applied cfg values, to react only to real changes on edits.
    QString m_appliedVoicegroupArg;
    int m_appliedVolume = 127;
    int m_appliedReverb = -1;

    QListWidget *m_songList = nullptr;
    SongView *m_songView = nullptr;
    VoicegroupBrowser *m_vgBrowser = nullptr;
    QDockWidget *m_vgDock = nullptr;
    QAction *m_newSongAction = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_registerAction = nullptr;
    QAction *m_goToStartAction = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_playPauseAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_loopAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_exportWavAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_songLabel = nullptr;
    QLabel *m_polyLabel = nullptr;
    QTimer *m_uiTimer = nullptr;
};
