#pragma once

#include <QMainWindow>

#include <memory>
#include <optional>
#include <vector>

#include "audio/audioengine.h"
#include "project/decompproject.h"
#include "project/voicegroupsource.h"
#include "songsession.h"
#include "ui/enginesettings.h"

class QAction;
class QChildEvent;
class QDockWidget;
class QLabel;
class QTabWidget;
class QSettings;
class QSpinBox;
class QToolBar;
class QToolButton;
class QTimer;
class QWidget;
class QUndoGroup;
class PolyphonyPanel;
class SmfFile;
class SongListPanel;
class SongView;
class VoicegroupBrowser;

namespace themes {
class ThemeController;
} // namespace themes
class SettingsDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

    friend class VoiceEditCommand; // calls applyVoiceEdit from undo/redo

  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // The live audio engine, for harnesses that verify what the Settings
    // window's controls publish to it.
    const AudioEngine &audio() const { return m_audio; }
    // The Settings window (built with the main window, shown on demand).
    SettingsDialog *settingsDialog() const { return m_settingsDialog.get(); }

    // Headless smoke test (--selftest <projectRoot> <songLabel>): opens the
    // project, loads the song, plays ~3 seconds through the real audio path,
    // and reports whether the playhead advanced.
    bool runSelfTest(const QString &projectRoot, const QString &songLabel);

    // Unified song+voicegroup undo/save check (--vgsavecheck; vgsavecheck.cpp).
    // Writes into the project: run against a scratch copy, with QSettings
    // already redirected by the caller. A non-empty screenshotPath saves the
    // sample picker's open popup for visual review.
    bool runVgSaveCheck(const QString &projectRoot, const QString &songLabel,
                        const QString &screenshotPath = QString());

    // Multi-tab check (--tabcheck; tabcheck.cpp): per-tab documents and undo
    // stacks, playback stopping on tab switches, tab close/replace, and
    // multi-tab session persistence. QSettings must be redirected.
    bool runTabCheck(const QString &projectRoot, const QString &songA, const QString &songB);

    // Register Song action wiring (part of --onboardcheck; onboardcheck.cpp):
    // a partially registered song keeps the action enabled, and running it
    // heals the registration. Writes into the project: scratch copy only,
    // QSettings must be redirected. Failures count into onboardcheck's total.
    bool runRegisterActionCheck(const QString &projectRoot, const QString &label);

    // Delete Song action wiring (part of --onboardcheck; onboardcheck.cpp):
    // deleting an open song closes its tab, drops it from the model and the
    // browser, and moves its .mid to .porydaw/trash/. Writes into the
    // project: scratch copy only, QSettings must be redirected. Failures
    // count into onboardcheck's total.
    bool runDeleteActionCheck(const QString &projectRoot, const QString &label);

    // Solo-overflow visibility gate (--polycheck stage C; polycheck.cpp):
    // the engine inverts only while the invert checkbox is checked AND the
    // Polyphony dock is visible. No project needed; QSettings must be
    // redirected.
    bool runPolyGateCheck();

    // Reopens the last session's project and open song tabs, if they still
    // exist. Called after show() on interactive launches only, so the
    // harnesses never inherit (or overwrite) the user's session.
    void restoreSession();

  protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void childEvent(QChildEvent *event) override;
    // Installed on the master-volume spinbox (and its line edit): refuses
    // the Space ShortcutOverride so play/pause keeps working while the
    // field is focused.
    bool eventFilter(QObject *watched, QEvent *event) override;

  private slots:
    void openProject();
    void songActivated(int songId);
    void songOpenInNewTab(int songId);
    void saveSong();
    void exportWav();
    void openSongSettings();
    void openSettings();
    void newSong();
    void importMidi();
    void importSample();
    // Sample Editor entry (slot >= 0: browser-initiated — after the commit
    // the new sample is auto-assigned to that voice slot as an undo command).
    void importSampleForSlot(int slot);
    // "Edit sample…" reopen (PLAN.md §6 phase 6): the slot's committed
    // sample, from its provenance sidecar (hi-res source + saved params) when
    // it checks out, else from the committed 8-bit .wav.
    void editSampleForSlot(int slot);
    void registerLoadedSong();
    // Shared by File → Register Song (the loaded song) and the song list's
    // right-click Register Song (any song, open or not).
    void registerSongById(int songId);
    // The song list's right-click Delete Song: confirms what will be removed
    // (offering the song's voicegroup when nothing else uses it), then
    // performs the deletion.
    void deleteSongById(int songId);
    void uiTick();
    void onVoiceEditRequested(int slot, const VgVoice &voice, bool structural);
    void tabChanged(int index);
    void closeTab(int index);

  private:
    void buildUi();
    void buildSettingsDialog();
    void updateWindowFrameTheme();
    void updateDockTabFonts();
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
    void finishCreateSong(const SmfFile &smf, const QString &label, const QString &constant,
                          const QString &player, const SongCfg &cfg, const QString &newVoicegroup);
    // The dialog-less half of deleteSongById (also the harness entry): closes
    // the song's tab discarding its edits, moves the .mid to .porydaw/trash/,
    // removes the flag line, unregisters, drops the sidecar, deletes the
    // named voicegroup (empty = keep), and reloads the project. Best-effort:
    // every step runs; false collects what failed into *error.
    bool performSongDeletion(const SongInfo &song, const QString &deleteVoicegroupName,
                             QString *error);
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
    // Prompts to save every dirty session (focusing each tab as it's asked
    // about); false = user cancelled. Saves answered before a Cancel have
    // already written, as with any save-all.
    bool promptToSaveAllSessions();
    // Destroys every session with no prompting and the engine/docks
    // detached once up front — currentChanged is suppressed so the doomed
    // neighbors aren't each rebound and persisted in turn.
    void teardownSessions();
    // Makes a session the engine-attached, dock-bound one (nullptr = none).
    // Always stops playback first. force re-binds even the already-active
    // session (used after replacing its song in place).
    void activateSession(SongSession *session, bool force = false);
    // Points the audio engine at the session's timeline/voicegroup and
    // re-applies its settings and the view's mute/solo masks.
    void attachEngine(SongSession &session);
    // Pushes the session's timeline/track-name/voice-name context into the
    // Polyphony dock (null clears it).
    void updatePolyPanelContext(SongSession *session);
    // A clean session whose voicegroup file changed on disk (saved from
    // another tab) silently follows the disk on activation.
    void maybeRefreshVoicegroup(SongSession &session);
    // After a voicegroup save, every OTHER clean session on the same file
    // reloads immediately — the active tab has no upcoming activation to
    // catch the change, and its stale parse would revert the save on its
    // own next voicegroup write.
    void refreshSessionsAfterVgSave(const QString &filePath, SongSession *except);
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
    // Installs/refreshes session-owned synth descriptors for every Golden Sun
    // synth voice whose loaded tone is missing (pending definition — not on
    // disk until save) or stale (a param edit patched a different desc), and
    // syncs voiceNames for symbol moves the scalar path never reloads.
    // Bytes are poked in place, so live tweaks are audible immediately.
    // Returns whether any tone or name actually changed.
    bool applyPendingSynthTones(SongSession &session, LoadedVoiceGroup *vg);
    // The descriptor a synth symbol stands for: pending first, then on-disk.
    const VgSynthDesc *synthDescForSymbol(const QString &symbol);
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
    void pausePlayback();
    void stopPlayback();
    // The session's cfg (volume/reverb) merged with the global engine knobs
    // — everything AudioEngine::updateSettings applies.
    SongSettings songSettingsFor(const SongSession &session) const;
    void refreshDerivedFonts();
    void refreshTransportIcons();
    void updateTransportActions();
    // Points the transport toolbar's master-volume spinbox at the active
    // tab's cfg (disabled with no tab). Never emits valueChanged.
    void syncMasterVolumeControl();
    // Points the transport toolbar's Tempo spinbox at the active tab's
    // starting tempo (disabled with no tab), and shows the warning beside
    // it when the song changes tempo after the start. Never emits
    // valueChanged.
    void syncTempoControl();
    void synchronizePlayhead();
    void updateTimeLabel();
    void updatePolyStatus();
    void updateWindowTitle();
    QString formatTime(uint64_t samples) const;

    // The Voicegroup dock's project-wide symbol/instrument lists: full
    // project .inc scans (catalogScan + directSoundCatalog + progWave),
    // far too slow to re-run on every tab switch. Cached per project root;
    // invalidated on project open/reload and on any voicegroup write.
    struct VgCatalog {
        bool valid = false;
        QStringList groupArgs; // the -G choices (SongRegistry::voicegroupArgs)
        QStringList directSound;
        QStringList progWave;
        QList<QPair<QString, QString>> keysplits;
        QStringList drumkits;
        VgSynthCatalog synths;
        VgAdsrDefaults typicalAdsr;
    };
    const VgCatalog &vgCatalog();
    void invalidateVgCatalog();
    // The committed data behind the picker's rows (loop badges and browse
    // audition): one voicegroup_load_samples batch over the whole catalog —
    // DirectSound samples, programmable waves, and keysplit instruments —
    // loaded on first use and freed with the catalog.
    void ensureSampleSet();
    const WaveData *sampleWaveFor(const QString &symbol);
    void auditionKeysplit(const QString &symbol);
    LoadedSampleSet *m_sampleSet = nullptr;
    QHash<QString, const WaveData *> m_sampleWaves;
    QHash<QString, const uint32_t *> m_progWaves;
    QHash<QString, LoadedKeysplit> m_keysplits;
    // Minted-but-unsaved Golden Sun synth definitions (symbol -> descriptor),
    // project-wide. Param edits point voice lines at these; they reach disk
    // (and the browser's dropdown) only when a voicegroup referencing them
    // saves. Cleared on project switch.
    QHash<QString, VgSynthDesc> m_pendingSynths;

    AudioEngine m_audio;
    bool m_audioOk = false;
    // False during harness runs so they don't overwrite the session.
    bool m_persistSession = true;
    EngineSettings m_engineSettings;
    DecompProject m_project;
    std::vector<std::unique_ptr<SongSession>> m_sessions;
    SongSession *m_active = nullptr;
    // Suppress currentChanged handling (and tab persistence) while tabs are
    // being torn down or bulk-restored; the caller activates once at the end.
    bool m_tearingDown = false;
    bool m_restoringSession = false;
    VgCatalog m_vgCatalog;

    SongListPanel *m_songList = nullptr;
    QTabWidget *m_tabs = nullptr;
    QUndoGroup *m_undoGroup = nullptr;
    VoicegroupBrowser *m_vgBrowser = nullptr;
    QDockWidget *m_vgDock = nullptr;
    PolyphonyPanel *m_polyPanel = nullptr;
    QDockWidget *m_polyDock = nullptr;
    QToolBar *m_transportToolbar = nullptr;
    std::unique_ptr<QSettings> m_themeSettings;
    std::unique_ptr<themes::ThemeController> m_themeController;
    std::unique_ptr<SettingsDialog> m_settingsDialog;
    QAction *m_newSongAction = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_importSampleAction = nullptr;
    QAction *m_registerAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_goToStartAction = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_playPauseAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_loopAction = nullptr;
    QAction *m_followPlayheadAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_exportWavAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_eventListAction = nullptr;
    QAction *m_velocityColorsAction = nullptr;
    QAction *m_noteNamesAction = nullptr;
    QAction *m_velocityLaneAction = nullptr;
    QAction *m_automationLanesAction = nullptr;
    QLabel *m_masterVolCaption = nullptr;
    QSpinBox *m_masterVolSpin = nullptr;
    QLabel *m_tempoCaption = nullptr;
    QSpinBox *m_tempoSpin = nullptr;
    QToolButton *m_tempoWarning = nullptr;
    QAction *m_tempoWarningAction = nullptr; // the toolbar's wrapper; owns visibility
    QLabel *m_timeLabel = nullptr;
    QLabel *m_songLabel = nullptr;
    QWidget *m_polyMeter = nullptr;
    QLabel *m_pcmValueLabel = nullptr;
    QLabel *m_cgbValueLabel = nullptr;
    QLabel *m_polyLostSeparator = nullptr;
    QLabel *m_polyLostCaption = nullptr;
    QLabel *m_polyLostLabel = nullptr;
    QTimer *m_uiTimer = nullptr;
    QTimer *m_playheadTimer = nullptr;
    QTimer *m_engineApplyTimer = nullptr; // coalesces Settings → engine restarts
    // Last values applied to the status widgets (uiTick runs at 2 Hz idle,
    // 10 Hz during playback; unchanged values skip the label writes).
    struct PolyStatusSnapshot {
        bool loaded = false;
        int activePcm = 0;
        int maxPcm = 0;
        int activeCgb = 0;
        uint64_t lostTotal = 0;

        bool operator==(const PolyStatusSnapshot &other) const
        {
            return loaded == other.loaded && activePcm == other.activePcm &&
                   maxPcm == other.maxPcm && activeCgb == other.activeCgb &&
                   lostTotal == other.lostTotal;
        }
    };
    QString m_lastTimeText;
    std::optional<PolyStatusSnapshot> m_lastPolyStatus;
};
