#pragma once

#include <QMainWindow>

#include <memory>
#include <vector>

#include "audio/audioengine.h"
#include "project/decompproject.h"
#include "project/voicegroupsource.h"
#include "songsession.h"
#include "ui/enginesettingsdialog.h"

class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;
class QTimer;
class QUndoGroup;
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

    // Multi-tab check (--tabcheck; tabcheck.cpp): per-tab documents and undo
    // stacks, playback stopping on tab switches, tab close/replace, and
    // multi-tab session persistence. QSettings must be redirected.
    bool runTabCheck(const QString &projectRoot, const QString &songA,
                     const QString &songB);

    // Reopens the last session's project and open song tabs, if they still
    // exist. Called after show() on interactive launches only, so the
    // harnesses never inherit (or overwrite) the user's session.
    void restoreSession();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openProject();
    void songActivated(int songId);
    void songOpenInNewTab(int songId);
    void saveSong();
    void exportWav();
    void openSongSettings();
    void openEngineSettings();
    void newSong();
    void importMidi();
    void registerLoadedSong();
    void uiTick();
    void onVoiceEditRequested(int slot, const VgVoice &voice, bool structural);
    void tabChanged(int index);
    void closeTab(int index);

private:
    void buildUi();
    // The dialog-less half of openProject; also the session-restore entry.
    // On failure warns via dialog (interactive) or status bar (restore).
    bool openProjectDir(const QString &dir, bool interactive = true);
    void populateSongList();
    // Opens a song: focuses its tab when already open, otherwise loads it
    // into the current tab (prompting for unsaved changes) or a new one.
    void loadSong(const SongInfo &song, bool newTab = false);
    void loadSongByLabel(const QString &label, bool newTab = false);
    // Shared New Song / Import finish: creates the new voicegroup when the
    // wizard asked for one, writes the .mid + midi.cfg line, registers the
    // song in the three registration files, reloads the project, and opens
    // the song in a new tab.
    void finishCreateSong(const SmfFile &smf, const QString &label,
                          const QString &constant, const QString &player,
                          const SongCfg &cfg, const QString &newVoicegroup);
    void reloadProject();

    // --- tab/session plumbing ---
    SongSession *activeSession() const { return m_active; }
    SongSession *sessionForWidget(QWidget *widget) const;
    SongSession *sessionForLabel(const QString &label) const;
    // Creates an empty session with a wired-up view; not yet in the tab bar.
    SongSession *createSession();
    // Removes the session's tab (re-activating a neighbor via currentChanged)
    // and destroys it. The engine is rebound before the data is freed.
    void destroySession(SongSession *session);
    // Prompt-save every session; false = user cancelled. On success all
    // sessions are closed (sidecars saved) and the tab bar is empty.
    bool closeAllSessions();
    // Makes a session the engine-attached, dock-bound one (nullptr = none).
    // Always stops playback first. force re-binds even the already-active
    // session (used after replacing its song in place).
    void activateSession(SongSession *session, bool force = false);
    // Points the audio engine at the session's timeline/voicegroup and
    // re-applies its settings and the view's mute/solo masks.
    void attachEngine(SongSession &session);
    // A clean session whose voicegroup file changed on disk (saved from
    // another tab) silently follows the disk on activation.
    void maybeRefreshVoicegroup(SongSession &session);
    // Records the open tabs + active song in QSettings for restoreSession.
    void persistOpenTabs();
    // Re-resolves each session's songId by label after a project reload.
    void refreshSessionSongIds();
    void updateTabTitle(SongSession &session);

    void updateVoicegroupBrowser();
    void onDocumentChanged(SongSession &session);
    // Saves the session's song AND its dirty voicegroup — the two are one
    // document to the user. The voicegroup goes first, so a failed write
    // leaves the session dirty. false = nothing was marked clean.
    bool saveSession(SongSession &session);
    // Prompts to save the session's unsaved changes (song edits and
    // voicegroup edits alike); false = user cancelled the action.
    bool maybeSaveSession(SongSession &session);
    // Locates + parses the source behind the session's voicegroup (nullptr
    // on exotic layouts — the editor degrades to read-only).
    void openVoicegroupSource(SongSession &session, const SongCfg &cfg);
    // Applies a voice-edit undo command: pokes the edit into the session's
    // open source and refreshes audio + views. No-op when the command's
    // voicegroup isn't the session's loaded one (stale target;
    // replayVoiceEdits re-syncs it later).
    void applyVoiceEdit(SongSession &session, const QString &loadName, int slot,
                        const VgVoice &voice, bool structural);
    // After a voicegroup switch reopens a source from disk, reapplies every
    // applied (below the undo index) voice-edit command targeting it, so
    // undoing back across a -G change restores unsaved voice edits too.
    void replayVoiceEdits(SongSession &session);
    void onVoiceEdited(SongSession &session, int slot, bool structural);
    // Auditions unsaved structural edits: renders the edited source into
    // .porydaw/vgpreview/ and reloads through the loader's config override,
    // which shadows the real file without touching it.
    void reloadVoicegroupPreview(SongSession &session, int keepSlot);
    // Swaps in a freshly loaded voicegroup (owned by the session from here),
    // reattaching the views — and, when active, the engine — around it.
    void swapVoicegroup(SongSession &session, LoadedVoiceGroup *vg, int keepSlot);
    void cleanupVgPreview();
    void updateVgDockTitle();
    void newVoicegroup();
    // Sidecar view state (SPEC §4.4): written whenever a session is let go
    // (tab close, project switch, app close). Cosmetic; silent on failure.
    void saveViewState(SongSession &session);
    LoadedVoiceGroup *loadVoicegroupFor(const SongCfg &cfg, QString *tried);
    // Starts (or resumes) playback; from Stopped, seeks to the edit cursor
    // first so playback begins there. fromEditCursor forces that seek even
    // out of Paused — the Space binding (Reaper-style restart), while the
    // Play button resumes from the pause point.
    void startPlayback(bool fromEditCursor = false);
    // The session's cfg (volume/reverb) merged with the global engine knobs
    // — everything AudioEngine::updateSettings applies.
    SongSettings songSettingsFor(const SongSession &session) const;
    void updateTransportActions();
    void updateWindowTitle();
    QString formatTime(uint64_t samples) const;

    AudioEngine m_audio;
    bool m_audioOk = false;
    // False during harness runs so they don't overwrite the session.
    bool m_persistSession = true;
    EngineSettings m_engineSettings;
    DecompProject m_project;
    std::vector<std::unique_ptr<SongSession>> m_sessions;
    SongSession *m_active = nullptr;

    SongListPanel *m_songList = nullptr;
    QTabWidget *m_tabs = nullptr;
    QUndoGroup *m_undoGroup = nullptr;
    VoicegroupBrowser *m_vgBrowser = nullptr;
    QDockWidget *m_vgDock = nullptr;
    QAction *m_newSongAction = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_registerAction = nullptr;
    QAction *m_closeTabAction = nullptr;
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
