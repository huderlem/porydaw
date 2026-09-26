#pragma once

#include <QMainWindow>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "audio/audioengine.h"
#include "project/decompproject.h"
#include "project/songregistry.h"
#include "project/voicegroupsource.h"
#include "songsession.h"
#include "ui/enginesettings.h"

class QAction;
class QChildEvent;
class QDockWidget;
class QFileInfo;
struct WavExportOptions;
class QLabel;
class QTabWidget;
class QTemporaryDir;
class QSettings;
class QSpinBox;
class QToolBar;
class QToolButton;
class CompanionWidget;
class QTimer;
class QWidget;
class QUndoGroup;
class PolyphonyPanel;
class NewSongWizard;
class SmfFile;
class SongListPanel;
class SongView;
class VoicegroupBrowser;

namespace themes {
class ThemeController;
} // namespace themes
namespace scripting {
class ScriptHost;
} // namespace scripting
namespace SongBundle {
struct ImportOptions;
struct ImportPlan;
} // namespace SongBundle
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
#ifdef PORYDAW_SCRIPTING
    // The plugin host (docs/scripting/PLAN.md). Plugins are loaded by
    // loadPlugins(), called on interactive launches only, so harnesses
    // never run the user's plugins (they point the host at a fixture dir).
    scripting::ScriptHost *scriptHost() const { return m_scriptHost.get(); }
    void loadPlugins();
    // HostBindings::addDock: a plugin dock joins the window in `area` (or
    // wherever windowState last saw it) and View → Plugin Panels.
    void addPluginDock(QDockWidget *dock, Qt::DockWidgetArea area);
    // Plugin host check (--scriptcheck; scriptcheck.cpp): fixture plugins
    // in pluginsDir are loaded through the real host — actions reach the
    // keymap and the window, errors/watchdog/hot reload/disable behave,
    // the console evaluates — and, with a project, the read API sees the
    // song. QSettings must be redirected. Empty projectRoot skips the
    // song half.
    bool runScriptHostCheck(const QString &pluginsDir, const QString &projectRoot,
                            const QString &songLabel);
    // HostBindings::renderWav: the active song through the Export WAV
    // path, without the dialogs. False with *error on failure.
    bool renderActiveSongWav(const QString &path, const WavExportOptions &opts, double *seconds,
                             QString *error);
#endif

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

    // Opens a song bundle (a .porysong file, or a folder holding a
    // porysong.json) in a read-only tab — with or without a project open
    // (docs/song-bundle/PLAN.md §3.6). A bundle that is already open is
    // focused instead. Failures show a message box, or land in *error when
    // one is given (harnesses, batch opens).
    bool openBundle(const QString &path, QString *error = nullptr);
    // The command line's positional arguments: every one that names a song
    // bundle is opened (after restoreSession, so it ends up in front).
    // Returns how many opened.
    int openCommandLinePaths(const QStringList &arguments);
    // Whether path is something openBundle takes: a *.porysong file or a
    // bundle folder.
    static bool isBundlePath(const QString &path);

    // Read-only bundle tab check (part of --bundlecheck; bundletabcheck.cpp):
    // opens bundleZip with no project open (plays, renders non-silent, the
    // document is locked, write actions are gated), through the drop-event
    // and command-line paths, then opens projectRoot and checks the bundle
    // tab survives with Import enabled. QSettings must be redirected.
    // Returns the number of failed expectations.
    int runBundleTabCheck(const QString &bundleZip, const QString &projectRoot,
                          const QString &scratchDir);
    // Import check (part of --bundlecheck; bundleimportcheck.cpp): opens
    // bundleZip, renders it, imports it into projectRoot through
    // applyBundleImport, and compares the imported song's tab and render
    // with the bundle's. QSettings must be redirected.
    int runBundleImportTabCheck(const QString &bundleZip, const QString &projectRoot,
                                const QString &scratchDir);

    // Import MIDI / New Song's finish (docs/draft-songs/PLAN.md): opens smf
    // as a draft tab named label — editable and playable, but nothing is
    // written to the project until its first save (commitDraft). A non-empty
    // newVoicegroup (cfg's -G then names "_<newVoicegroup>") is created in
    // memory and loaded from the draft's own folder under .porydaw/drafts/.
    // False with *error when it can't open.
    bool openDraftSong(const SmfFile &smf, const QString &label, const QString &constant,
                       const QString &player, const SongCfg &cfg, const QString &newVoicegroup,
                       QString *error);

    // Draft-song check (--draftcheck; draftcheck.cpp): opening, editing and
    // discarding a draft leave the project untouched; saving one commits
    // it (.mid, flags, registration) in place, undo history included.
    // Writes into the project: scratch copy only, QSettings must be
    // redirected. Returns the number of failed expectations.
    int runDraftCheck(const QString &projectRoot);

    // Reopens the last session's project and open song tabs, if they still
    // exist. Called after show() on interactive launches only, so the
    // harnesses never inherit (or overwrite) the user's session.
    void restoreSession();

  protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void childEvent(QChildEvent *event) override;
    // Song bundles dropped on the window open like File → Open Song Bundle.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
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
    void exportBundle();
    void openBundleDialog();
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
    void loadSong(const SongInfo &requested, bool newTab = false);
    void loadSongByLabel(const QString &label, bool newTab = false);
    // The shared body of loadSong and openDraftSong: loads the song's
    // voicegroup, reads the document through readDocument into session
    // (a fresh session, added as a new tab, when null; replaced in place
    // otherwise), installs draft (null = an ordinary song), and binds the
    // timeline, voicegroup and view. Null with *error on failure; a
    // replaced session is then left as it was.
    SongSession *populateSession(SongSession *session, const SongInfo &song,
                                 std::unique_ptr<SongDraft> draft,
                                 const std::function<bool(SongDocument &, QString *)> &readDocument,
                                 QString *error);
    // New Song / Import finish: a draft tab (openDraftSong), then a status
    // message saying so. imported picks the wording and a failure box's
    // title.
    void createSongFromWizard(const NewSongWizard &wizard, bool imported);
    // A draft's first save: writes its new voicegroup (when the cfg still
    // names it), the .mid and its flags, registers the song (a failed
    // registration still commits, as an unregistered song), and turns the
    // session into an ordinary project song in place — its document, undo
    // history and view survive. Safe to retry after a failure. False with
    // *error when nothing was committed. The caller (saveSession) has
    // already settled the names (resolveDraftNameConflicts).
    bool commitDraft(SongSession &session, QString *error);
    // commitDraft's voicegroup step: the draft's new voicegroup, edits
    // included, through its source's save() plus the hub's include line —
    // only while the cfg names it (a draft switched to another voicegroup
    // writes none). Then loads resolve to the project copy and the draft
    // folder goes. Skips what an earlier attempt already did.
    bool commitDraftVoicegroup(SongSession &session, QString *error);
    // The commit found the cfg switched away from the draft's new
    // voicegroup: drop it from the draft and rewrite its -G in the undo
    // history to the committed one.
    void abandonDraftVoicegroup(SongSession &session);
    // The Rename dialog's voicegroup half: the draft's unwritten voicegroup
    // takes newName — its file in the draft folder, its voice_group/label
    // symbol, the source, the voice edits' target in the undo history and
    // the cfg's -G arg. False with *error (nothing changed) on failure.
    bool renameDraftVoicegroup(SongSession &session, const QString &newName, QString *error);
    // Creates draft's folder under <root>/.porydaw/drafts/ and its lock.
    bool createDraftFolder(SongDraft &draft, QString *error);
    // Rewrites the draft folder's copy of the session's unwritten voicegroup
    // from its (edited) source, so the next load hears the edits. No-op for
    // any other session.
    void syncDraftVoicegroupFile(SongSession &session);
    // At project open: removes .porydaw/drafts/ folders no live Porydaw
    // holds the lock of (left by a crash); locked ones are skipped.
    static void sweepStaleDraftFolders(const QString &root);
    // A draft's Save starts here (saveSession, before any write — the
    // dirty-voicegroup save included): when a name the draft holds was taken since
    // the wizard (the .mid created, the label or constant registered, the
    // voicegroup file made), explains the conflict in the Rename dialog and,
    // on OK, renames the draft in place. False when the user cancels; true
    // when the names are free (at once or after the rename).
    bool resolveDraftNameConflicts(SongSession &session);
    // After a session's label or .mid path changed in place (a draft's
    // rename, its commit): the tab title and tooltip and, for the active
    // session, the window title, the transport's song label and the script
    // host's session label — what activateSession sets, without rebinding
    // the engine.
    void refreshSessionIdentity(SongSession &session);
    // The dialog-less half of deleteSongById (also the harness entry): closes
    // the song's tab discarding its edits, moves the .mid to .porydaw/trash/,
    // removes the flag line, unregisters, drops the sidecar, deletes the
    // named voicegroup (empty = keep), and reloads the project. Best-effort:
    // every step runs; false collects what failed into *error.
    bool performSongDeletion(const SongInfo &song, const QString &deleteVoicegroupName,
                             QString *error);
    // Re-reads the project's music data after a registration change.
    // Without *error a failure shows a message box.
    bool reloadProject(QString *error = nullptr);
    // Project adapter writes shared with the plugin host (scripting::
    // HostBindings): the window's actions minus their dialogs, false with
    // *error on failure. Empty constant/player take the song's own (or the
    // label-derived / BGM defaults), as Register Song does.
    bool registerSongByLabel(const QString &label, const QString &constant, const QString &player,
                             int *songId, QString *error);
    bool unregisterSongByLabel(const QString &label, QString *error);
    // sound/voicegroups/<name>.inc as a copy of the voicegroup copyFromArg
    // names (dummy template when empty), hub line included.
    bool createVoicegroupNamed(const QString &name, const QString &copyFromArg, QString *error);
    // A voice edit on the session's open voicegroup source, through its
    // document's undo stack (an open edit group takes it). A no-change
    // edit is a success that pushes nothing.
    bool pushVoiceEdit(SongSession &session, int slot, const VgVoice &voice, QString *error);
    // reloadProject with a failure shown in the status bar (after a write
    // that succeeded regardless).
    void reloadProjectOrWarn();
    // Register Song is enabled while the active song's registration has gaps.
    void refreshRegisterAction();

    // --- tab/session plumbing ---
    SongSession *activeSession() const { return m_active; }
    SongSession *sessionForWidget(QWidget *widget) const;
    // Project-song tabs only; a bundle tab is found by its file instead.
    SongSession *sessionForLabel(const QString &label) const;
    SongSession *sessionForBundlePath(const QString &canonicalPath) const;
    // The names open tabs hold that the project's files may not know yet
    // (docs/draft-songs/PLAN.md step 2): every project tab's label, plus
    // each draft's constant and new voicegroup — the New Song wizard's and
    // the Rename dialog's reserved list. except is left out (the draft
    // being renamed doesn't conflict with itself).
    ReservedSongNames reservedSongNames(const SongSession *except = nullptr) const;
    static QString bundleTabTitle(const QString &label);
    // A draft tab's title (docs/draft-songs/PLAN.md step 4), marked like a
    // bundle tab's; the tab and window titles add the unsaved marker.
    static QString draftTabTitle(const QString &label);
    // The tab's tooltip: the song's .mid, or for a draft (whose .mid doesn't
    // exist yet) what Save and closing will do.
    QString sessionTabToolTip(const SongSession &session) const;
    // The strip above a bundle tab's ruler: what it is, and the Import
    // button (SongSession::bundleImportButton).
    QWidget *createBundleBanner(SongSession &session);
    // Import needs a project to import into: every bundle tab's button
    // follows m_project.isOpen().
    void refreshBundleBanners();
    // The strip above a draft tab's ruler (SongSession::draftBanner): what
    // Save and closing do, and a Save to project button. Created while the
    // session is a draft, its text following the label; removed once it
    // isn't (committed, or replaced in place by another song).
    void updateDraftBanner(SongSession &session);
    void importBundle(SongSession &session);
    // Applies an accepted import plan to the open project, reloads it, and
    // opens the imported song in a new editable tab. bundleTab, the tab the
    // import came from (none for a plugin's import), closes once it has.
    bool applyBundleImport(const SongBundle::ImportPlan &plan, QString *error,
                           SongSession *bundleTab = nullptr);
    // The folder a bundle is read from: a bundle folder itself, or a fresh
    // extraction of a .porysong under *tempDir (allocated only then), which
    // the caller keeps alive for as long as it reads the root.
    bool resolveBundleRoot(const QFileInfo &info, std::unique_ptr<QTemporaryDir> *tempDir,
                           QString *root, QString *error);
    // The dialog-free halves shared with the plugin host. Export: a project
    // song by label — an open tab as it is in memory (unsaved edits
    // included), any other song from disk. Import: plans the bundle at path
    // (.porysong file or bundle folder) against the open project with the
    // given overrides and applies it; a refusing plan fails with its
    // refusals. *applied receives the plan that ran (its warnings included).
    bool exportBundleByLabel(const QString &label, const QString &path, int *samples,
                             QString *error);
    bool importBundleFile(const QString &path, const SongBundle::ImportOptions &options,
                          SongBundle::ImportPlan *applied, QString *error);
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
    const VgSynthDesc *synthDescForSymbol(const QString &root, const QString &symbol);
    void cleanupVgPreview();
    void updateVgDockTitle();
    void newVoicegroup();
    // Sidecar view state (SPEC §4.4): written whenever a session is let go
    // (tab close, project switch, app close). Cosmetic; silent on failure.
    void saveViewState(SongSession &session);
    // Loads cfg's voicegroup under root. With a draft whose new voicegroup
    // is unwritten, the draft folder is searched before the project's own
    // voicegroups (its copy is the only one).
    LoadedVoiceGroup *loadVoicegroupFor(const QString &root, const SongCfg &cfg, QString *tried,
                                        const SongDraft *draft = nullptr);
    // Every load of a session's voicegroup: under its root, draft-aware.
    LoadedVoiceGroup *loadVoicegroupForSession(const SongSession &session, const SongCfg &cfg,
                                               QString *tried);
    // The -G args a session can choose from: the project catalog's, plus
    // its draft's unwritten new voicegroup.
    QStringList voicegroupChoices(const SongSession &session);
    // The pending (in-memory) synth definitions the session's voicegroup
    // source references — what must be written before that source is saved.
    QList<QPair<QString, VgSynthDesc>> pendingSynthsReferencedBy(const SongSession &session) const;
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
    void syncCompanion(uint64_t playhead, bool playing);
    void chooseCompanionImage();
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
        QStringList cries;           // compressed voices' sample choices
        QStringList voiceMacroWords; // VgDirectSoundScan::voiceMacroWords
        QStringList progWave;
        QList<QPair<QString, QString>> keysplits;
        QStringList drumkits;
        VgSynthCatalog synths;
        VgAdsrDefaults typicalAdsr;
        QString root; // what `valid` data was scanned from
    };
    // Root-scoped reads take the owning session's root (SongSession::root),
    // never m_project's: a tab need not live under the open project.
    const VgCatalog &vgCatalog(const QString &root);
    QString activeRoot() const; // the active tab's root, else the project's
    void invalidateVgCatalog();
    // The committed data behind the picker's rows (loop badges and browse
    // audition): one voicegroup_load_samples batch over the whole catalog —
    // DirectSound samples, programmable waves, and keysplit instruments —
    // loaded on first use and freed with the catalog.
    void ensureSampleSet(const QString &root);
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
    QAction *m_exportBundleAction = nullptr;
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
    CompanionWidget *m_companion = nullptr;
    QAction *m_companionAction = nullptr;
    uint64_t m_companionLastSample = UINT64_MAX;
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
#ifdef PORYDAW_SCRIPTING
    // Declared after m_sessions on purpose: the host is destroyed first,
    // while the session its facades still point at is alive for a
    // plugin's deactivate().
    std::unique_ptr<scripting::ScriptHost> m_scriptHost;
    QDockWidget *m_consoleDock = nullptr;
    // View → Plugin Panels: one toggle per plugin dock (hidden while empty).
    QMenu *m_pluginPanelsMenu = nullptr;
    QMenu *m_pluginsMenu = nullptr; // menu-bar Plugins menu; shown while a plugin fills it
#endif
};
