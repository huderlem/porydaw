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
class QTimer;
class SmfFile;
class SongListPanel;
class SongView;
class VoicegroupBrowser;

class MainWindow : public QMainWindow
{
    Q_OBJECT

    friend class VoiceEditCommand; // calls applyVoiceEdit from undo/redo

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Headless smoke test (--selftest <projectRoot> <songLabel>): opens the
    // project, loads the song, plays ~3 seconds through the real audio path,
    // and reports whether the playhead advanced.
    bool runSelfTest(const QString &projectRoot, const QString &songLabel);

    // Unified song+voicegroup undo/save check (--vgsavecheck; vgsavecheck.cpp).
    // Writes into the project: run against a scratch copy, with QSettings
    // already redirected by the caller.
    bool runVgSaveCheck(const QString &projectRoot, const QString &songLabel);

    // Reopens the last session's project and song, if they still exist.
    // Called after show() on interactive launches only, so the harnesses
    // never inherit (or overwrite) the user's session.
    void restoreSession();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openProject();
    void songActivated(int songId);
    void saveSong();
    void exportWav();
    void openSongSettings();
    void openEngineSettings();
    void newSong();
    void importMidi();
    void registerLoadedSong();
    void onDocumentChanged();
    void uiTick();
    void onVoiceEditRequested(int slot, const VgVoice &voice, bool structural);
    void onVoiceEdited(int slot, bool structural);
    void newVoicegroup();

private:
    void buildUi();
    // The dialog-less half of openProject; also the session-restore entry.
    // On failure warns via dialog (interactive) or status bar (restore).
    bool openProjectDir(const QString &dir, bool interactive = true);
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
    // Saves the loaded song AND its dirty voicegroup — the two are one
    // document to the user. The voicegroup goes first (its write is
    // permission-gated), so a refused or failed write leaves the session
    // dirty. false = nothing was marked clean.
    bool saveLoadedSong();
    // Prompts to save unsaved changes (song edits and voicegroup edits
    // alike); false = user cancelled the action.
    bool maybeSaveSong();
    // Locates + parses the source behind the loaded voicegroup (nullptr on
    // exotic layouts — the editor degrades to read-only).
    void openVoicegroupSource(const SongCfg &cfg);
    // Applies a voice-edit undo command: pokes the edit into the open source
    // and refreshes audio + views. No-op when the command's voicegroup isn't
    // the loaded one (stale target; replayVoiceEdits re-syncs it later).
    void applyVoiceEdit(const QString &loadName, int slot, const VgVoice &voice,
                        bool structural);
    // After a voicegroup switch reopens a source from disk, reapplies every
    // applied (below the undo index) voice-edit command targeting it, so
    // undoing back across a -G change restores unsaved voice edits too.
    void replayVoiceEdits();
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
    // False during --selftest so harness runs don't overwrite the session.
    bool m_persistSession = true;
    EngineSettings m_engineSettings;
    DecompProject m_project;
    SongDocument m_doc;
    std::unique_ptr<VoicegroupSource> m_vgSource;
    int m_loadedSongId = -1;
    // Engine-applied cfg values, to react only to real changes on edits.
    QString m_appliedVoicegroupArg;
    int m_appliedVolume = 127;
    int m_appliedReverb = -1;

    SongListPanel *m_songList = nullptr;
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
