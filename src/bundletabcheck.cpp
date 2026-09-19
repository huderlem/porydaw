#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>
#include <cstdio>
#include <functional>

#include "audio/wavexport.h"
#include "mainwindow.h"
#include "project/bundlearchive.h"
#include "project/songbundle.h"
#include "ui/voicegroupbrowser.h"

// The read-only bundle tab sections of --bundlecheck (docs/song-bundle/
// PLAN.md Phase 2), run by runBundleCheck against the bundle the export
// sections wrote (export_a.porysong) and their macro fixture project:
//  - SongBundle::readSong refuses bundles that reach outside their root
//    (label, -G value, .incbin path) and openBundle leaves no tab behind;
//  - with NO project open the bundle opens in a tab rooted in a temp dir,
//    plays (the transport advances and an offline render is non-silent),
//    its document is locked (edits, cfg, voice edits and saves all no-ops),
//    and Save / Song Settings / Register / Export Bundle / Import are off
//    while Export WAV stays on and the voicegroup dock is view-only;
//  - re-opening focuses the same tab; closing removes the temp dir; a
//    folder bundle opens in place and nothing is written under it;
//  - the drop-event and command-line paths open bundles;
//  - opening a project keeps the bundle tab (decided: bundle tabs are
//    project-independent), enables Import, never persists the bundle tab,
//    and a project song with the bundle's label gets its own tab.

namespace {

QStringList treeFiles(const QString &root)
{
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << QDir(root).relativeFilePath(it.next());
    files.sort();
    return files;
}

bool copyTree(const QString &from, const QString &to)
{
    for (const QString &rel : treeFiles(from)) {
        const QString dest = to + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(dest).path());
        if (!QFile::copy(from + QLatin1Char('/') + rel, dest))
            return false;
    }
    return true;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile in(path);
    return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

// Peak absolute 16-bit sample of a PCM WAV's data chunk (0 when unreadable).
int wavPeak(const QString &path)
{
    const QByteArray bytes = readFile(path);
    const int data = bytes.indexOf("data");
    if (data < 0)
        return 0;
    int peak = 0;
    for (int i = data + 8; i + 1 < bytes.size(); i += 2) {
        const auto lo = uchar(bytes[i]);
        const auto hi = uchar(bytes[i + 1]);
        const int v = qint16(quint16(lo | (hi << 8)));
        peak = qMax(peak, qAbs(v));
    }
    return peak;
}

void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int MainWindow::runBundleTabCheck(const QString &bundleZip, const QString &fixtureProject,
                                  const QString &scratchDir)
{
    m_persistSession = false;
    int failures = 0;
    const auto check = [&failures](bool ok, const QString &what) {
        if (!ok) {
            std::fprintf(stderr, "bundlecheck: FAIL: tab: %s\n", qUtf8Printable(what));
            failures++;
        }
    };
    if (!m_audioOk) {
        check(false, QStringLiteral("no audio device available"));
        return failures;
    }
    const auto path = [&](const char *name) {
        return scratchDir + QStringLiteral("/tab_") + QLatin1String(name);
    };
    QString error;

    // The folder form of the same bundle, for the refusals and the
    // opened-in-place section.
    const QString folder = path("folder");
    check(BundleArchive::extractBundle(bundleZip, folder, &error),
          QStringLiteral("fixture bundle extracts: ") + error);

    { // ---- hostile bundles are refused, no tab left behind ---------------------
        const auto hostile = [&](const char *name, const std::function<void(const QString &)> &edit,
                                 const QString &expectInError) {
            const QString dir = path(name);
            check(copyTree(folder, dir), QStringLiteral("%1: fixture copies").arg(name));
            edit(dir);
            BundleManifest m;
            SongInfo song;
            QString why;
            check(!SongBundle::readSong(dir, &m, &song, &why) && why.contains(expectInError),
                  QStringLiteral("%1: readSong refuses, got '%2'").arg(QLatin1String(name), why));
            why.clear();
            check(!openBundle(dir, &why) && !why.isEmpty() && m_sessions.empty() &&
                      m_tabs->count() == 0,
                  QStringLiteral("%1: openBundle fails cleanly").arg(QLatin1String(name)));
        };
        const auto patchManifestLabel = [](const QString &dir) {
            const QString p = SongBundle::manifestPath(dir);
            QJsonObject root = QJsonDocument::fromJson(readFile(p)).object();
            QJsonObject song = root.value(QStringLiteral("song")).toObject();
            song.insert(QStringLiteral("label"), QStringLiteral("../../escape"));
            root.insert(QStringLiteral("song"), song);
            writeFile(p, QJsonDocument(root).toJson());
        };
        hostile("hostile_label", patchManifestLabel, QStringLiteral("not a valid song label"));
        hostile(
            "hostile_incbin",
            [](const QString &dir) {
                const QString p = dir + QStringLiteral("/sound/direct_sound_data.inc");
                QByteArray b = readFile(p);
                b.replace("\"sound/direct_sound_samples/flute.bin\"", "\"../../../secret.bin\"");
                writeFile(p, b);
            },
            QStringLiteral("secret.bin"));
        // The loader needs no space before the quote (strstr + first quote).
        hostile(
            "hostile_incbin_tight",
            [](const QString &dir) {
                const QString p = dir + QStringLiteral("/sound/direct_sound_data.inc");
                QByteArray b = readFile(p);
                b.replace(".incbin \"sound/direct_sound_samples/flute.bin\"",
                          ".incbin\"../../../secret.bin\"");
                writeFile(p, b);
            },
            QStringLiteral("secret.bin"));
        hostile(
            "hostile_vg",
            [](const QString &dir) {
                writeFile(dir + QStringLiteral("/sound/songs/midi/midi.cfg"),
                          "mus_bundle.mid: -E -G/../../x -V090\n");
            },
            QStringLiteral("not a valid voicegroup"));
        hostile(
            "no_mid",
            [](const QString &dir) {
                QFile::remove(dir + QStringLiteral("/sound/songs/midi/mus_bundle.mid"));
            },
            QStringLiteral("incomplete"));
        error.clear();
        check(!openBundle(path("does_not_exist.porysong"), &error) && !error.isEmpty(),
              QStringLiteral("a missing file fails with a message"));
        check(isBundlePath(bundleZip) && isBundlePath(folder) && !isBundlePath(scratchDir) &&
                  !isBundlePath(folder + QStringLiteral("/porysong.json")),
              QStringLiteral("isBundlePath: .porysong files and bundle folders only"));
    }

    // ---- open with NO project ------------------------------------------------
    check(!m_project.isOpen(), QStringLiteral("harness starts with no project"));
    error.clear();
    check(openBundle(bundleZip, &error), QStringLiteral("bundle opens with no project: ") + error);
    SongSession *tab = m_active;
    if (!tab || !tab->bundle) {
        check(false, QStringLiteral("the bundle tab is not the active session"));
        return failures;
    }
    const QString tempRoot = tab->root;
    check(m_tabs->count() == 1 && m_sessions.size() == 1, QStringLiteral("exactly one tab"));
    check(tab->bundleDir && tempRoot.startsWith(tab->bundleDir->path()) &&
              SongBundle::isBundleDir(tempRoot) && !tempRoot.startsWith(scratchDir),
          QStringLiteral("the session is rooted in its own temp extraction dir"));
    check(tab->bundlePath == QFileInfo(bundleZip).canonicalFilePath(),
          QStringLiteral("bundlePath is the canonical file"));
    check(tab->manifest.label == QStringLiteral("mus_bundle") &&
              tab->doc.label() == QStringLiteral("mus_bundle") && tab->songId == -1,
          QStringLiteral("manifest + label loaded, no project song id"));
    check(tab->doc.cfg().voicegroupArg == QStringLiteral("_bundle_song") &&
              tab->doc.cfg().masterVolume == 90 && tab->doc.cfg().exactGate,
          QStringLiteral("cfg synthesized from the bundle's midi.cfg line"));
    check(tab->voicegroup && tab->vgSource && tab->vgSource->filePath().startsWith(tempRoot),
          QStringLiteral("voicegroup + its source load from the bundle root"));
    check(m_tabs->tabText(0) == bundleTabTitle(QStringLiteral("mus_bundle")) &&
              m_tabs->tabToolTip(0).contains(QStringLiteral("export_a.porysong")),
          QStringLiteral("tab title marks the bundle, tooltip names the file"));
    check(windowTitle().contains(QStringLiteral("export_a.porysong")) && !isWindowModified(),
          QStringLiteral("window title names the bundle file"));
    {
        auto *banner = tab->view->findChild<QWidget *>(QStringLiteral("bundleBanner"));
        auto *text = tab->view->findChild<QLabel *>(QStringLiteral("bundleBannerText"));
        check(banner && text && text->text().contains(QStringLiteral("read-only")) &&
                  text->text().contains(QStringLiteral("export_a.porysong")),
              QStringLiteral("the read-only banner is in the view"));
        check(tab->bundleImportButton && !tab->bundleImportButton->isEnabled() &&
                  tab->bundleImportButton->toolTip() == tr("Open a decomp project first"),
              QStringLiteral("Import is disabled (with the tooltip) while no project is open"));
    }

    // Visual review: PORYDAW_BUNDLE_SHOT=<png> saves the window with the
    // bundle tab up (banner, gated menus' state, view-only dock).
    const QString shot = qEnvironmentVariable("PORYDAW_BUNDLE_SHOT");
    if (!shot.isEmpty()) {
        resize(1280, 800);
        show();
        QApplication::processEvents();
        grab().save(shot);
    }

    // Action gating: listening stays, writing goes.
    check(!m_saveAction->isEnabled() && !m_settingsAction->isEnabled() &&
              !m_registerAction->isEnabled() && !m_exportBundleAction->isEnabled(),
          QStringLiteral("Save / Song Settings / Register / Export Bundle are disabled"));
    check(m_exportWavAction->isEnabled() && m_closeTabAction->isEnabled() &&
              m_playAction->isEnabled(),
          QStringLiteral("Export WAV, Close Tab and Play stay enabled"));
    check(!m_newSongAction->isEnabled() && !m_importAction->isEnabled() &&
              !m_importSampleAction->isEnabled(),
          QStringLiteral("project-scoped actions stay disabled with no project"));
    check(!m_masterVolSpin->isEnabled() && !m_tempoSpin->isEnabled(),
          QStringLiteral("toolbar volume/tempo spinners are disabled"));
    check(m_vgBrowser->viewOnly(), QStringLiteral("the voicegroup dock is view-only"));

    { // ---- the document lock ---------------------------------------------------
        const uint64_t revision = tab->doc.revision();
        const QByteArray before = tab->doc.smf().write();
        check(tab->doc.isLocked(), QStringLiteral("document is locked"));
        tab->doc.addNote(0, 0, 60, 24, 100);
        tab->doc.addLanePoint(0, 7, 0, 100);
        SongCfg cfg = tab->doc.cfg();
        cfg.masterVolume = 11;
        tab->doc.setCfg(cfg);
        check(!tab->doc.canAddTrack() && tab->doc.addTrack(0) == -1,
              QStringLiteral("addTrack reports failure"));
        check(tab->doc.revision() == revision && tab->doc.smf().write() == before &&
                  tab->doc.cfg().masterVolume == 90 && tab->doc.undoStack()->count() == 0,
              QStringLiteral("pushCommand is a no-op: nothing changed, nothing undoable"));
        check(!tab->doc.isDirty() && !tab->isDirty() && !isWindowModified(),
              QStringLiteral("a locked document is never dirty"));
        // An open edit group (a script transaction's shape) stays empty too.
        tab->doc.beginEditGroup(QStringLiteral("group"));
        tab->doc.addNote(0, 0, 62, 24, 100);
        check(!tab->doc.endEditGroup(false) && tab->doc.undoStack()->count() == 0,
              QStringLiteral("an edit group over a locked document commits nothing"));

        const VgVoice *voice = tab->vgSource->voiceAt(0);
        check(voice != nullptr, QStringLiteral("slot 0 is an editable voice in the source"));
        if (voice) {
            VgVoice edited = *voice;
            edited.release = edited.release == 1 ? 2 : 1;
            QString why;
            check(!pushVoiceEdit(*tab, 0, edited, &why) &&
                      why.contains(QStringLiteral("read-only")),
                  QStringLiteral("pushVoiceEdit refuses with a reason"));
            onVoiceEditRequested(0, edited, false);
            check(!tab->vgSource->dirty() && *tab->vgSource->voiceAt(0) == *voice,
                  QStringLiteral("a dock voice edit never reaches the source"));
        }
        const QStringList filesBefore = treeFiles(tempRoot);
        QString why;
        check(!saveSession(*tab) && !tab->doc.save(&why) && !why.isEmpty(),
              QStringLiteral("saves refuse"));
        saveViewState(*tab);
        check(treeFiles(tempRoot) == filesBefore,
              QStringLiteral("no sidecar or file is written under the bundle root"));
        check(maybeSaveSession(*tab), QStringLiteral("closing never prompts"));
    }

    { // ---- it plays ---------------------------------------------------------------
        check(m_audio.songLoaded() && m_audio.timeline() == tab->timeline.get(),
              QStringLiteral("the engine holds the bundle's timeline"));
        QApplication::processEvents();
        startPlayback();
        waitMs(700);
        const uint64_t played = m_audio.playheadSamples();
        check(m_audio.transport() == Transport::Playing &&
                  played > uint64_t(m_audio.sampleRate() / 8),
              QStringLiteral("transport plays: playhead at %1 samples").arg(played));
        stopPlayback();

        WavExportOptions opts;
        opts.loopCount = 1;
        opts.fadeoutSeconds = 0.5;
        opts.tailSeconds = 0.5;
        auto timeline = tab->doc.buildTimeline(double(opts.sampleRate));
        const QString wav = path("render.wav");
        QString why;
        const bool rendered = ::exportWav(wav, *timeline, tab->voicegroup, songSettingsFor(*tab),
                                          opts, nullptr, &why);
        const int peak = rendered ? wavPeak(wav) : 0;
        check(rendered && peak > 500,
              QStringLiteral("offline render of the bundle is non-silent (peak %1) %2")
                  .arg(peak)
                  .arg(why));
    }

    { // ---- one tab per bundle; closing removes the temp dir ------------------------
        check(openBundle(bundleZip, &error) && m_tabs->count() == 1 && m_active == tab,
              QStringLiteral("opening the same bundle again focuses its tab"));
        closeTab(0);
        check(m_tabs->count() == 0 && m_sessions.empty() && !m_active,
              QStringLiteral("the bundle tab closes"));
        check(!QFileInfo::exists(tempRoot),
              QStringLiteral("closing the tab deletes the extraction dir"));
    }

    { // ---- a folder bundle opens in place, untouched --------------------------------
        const QStringList before = treeFiles(folder);
        check(openBundle(folder, &error), QStringLiteral("folder bundle opens: ") + error);
        check(m_active && m_active->bundle && !m_active->bundleDir &&
                  m_active->root == QFileInfo(folder).canonicalFilePath(),
              QStringLiteral("a folder bundle is rooted at the folder"));
        if (m_active)
            closeTab(m_tabs->indexOf(m_active->view));
        check(treeFiles(folder) == before && QFileInfo::exists(folder),
              QStringLiteral("a folder bundle is neither written to nor deleted"));
    }

    { // ---- drop events ----------------------------------------------------------------
        QMimeData other;
        other.setUrls({QUrl::fromLocalFile(folder + QStringLiteral("/porysong.json"))});
        QDragEnterEvent ignoredEnter(QPoint(10, 10), Qt::CopyAction, &other, Qt::LeftButton,
                                     Qt::NoModifier);
        ignoredEnter.ignore();
        QApplication::sendEvent(this, &ignoredEnter);
        check(!ignoredEnter.isAccepted(), QStringLiteral("a non-bundle drag is not accepted"));

        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(bundleZip)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton,
                              Qt::NoModifier);
        enter.ignore();
        QApplication::sendEvent(this, &enter);
        check(acceptDrops() && enter.isAccepted(), QStringLiteral("a .porysong drag is accepted"));
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        drop.ignore();
        QApplication::sendEvent(this, &drop);
        check(drop.isAccepted(), QStringLiteral("the drop is accepted"));
        waitMs(50); // the open is deferred out of the drop
        check(m_tabs->count() == 1 && m_active && m_active->bundle,
              QStringLiteral("dropping a .porysong opens its tab"));
        if (m_active)
            closeTab(m_tabs->indexOf(m_active->view));
    }

    { // ---- command-line arguments ------------------------------------------------------
        const int opened =
            openCommandLinePaths({QStringLiteral("--some-flag"), path("does_not_exist.porysong"),
                                  bundleZip, folder, folder + QStringLiteral("/porysong.json")});
        check(opened == 2 && m_tabs->count() == 2,
              QStringLiteral("positional bundle paths open (file + folder), got %1").arg(opened));
        check(m_active && m_active->bundle && !m_active->bundleDir,
              QStringLiteral("the last path named is the active tab"));
        while (m_tabs->count() > 1)
            closeTab(m_tabs->count() - 1);
    }

    // ---- then a project opens: the bundle tab stays ---------------------------------------
    tab = m_active;
    check(tab && tab->bundle && tab->bundleDir, QStringLiteral("the zip bundle tab is left open"));
    if (!tab)
        return failures;
    const QString projectRoot = path("project");
    check(copyTree(fixtureProject, projectRoot), QStringLiteral("fixture project copies"));
    writeFile(projectRoot + QStringLiteral("/sound/song_table.inc"),
              "\tsong mus_bundle, MUSIC_PLAYER_BGM, 0\n");
    writeFile(projectRoot + QStringLiteral("/sound/songs/midi/midi.cfg"),
              "mus_bundle.mid: -E -R50 -G_bundle_song -V090\n");

    check(openProjectDir(projectRoot, /*interactive=*/false),
          QStringLiteral("the fixture project opens"));
    check(m_project.isOpen() && m_tabs->count() == 1 && m_sessions.size() == 1 && m_active == tab,
          QStringLiteral("openProjectDir keeps the bundle tab, still active"));
    check(m_audio.songLoaded() && m_audio.timeline() == tab->timeline.get(),
          QStringLiteral("the engine is re-bound to the surviving bundle tab"));
    check(tab->bundleImportButton->isEnabled() &&
              tab->bundleImportButton->toolTip() != tr("Open a decomp project first"),
          QStringLiteral("Import is enabled once a project is open"));
    check(!m_saveAction->isEnabled() && !m_settingsAction->isEnabled() &&
              m_newSongAction->isEnabled(),
          QStringLiteral("the bundle tab stays gated; project actions come alive"));
    check(tab->songId == -1 && QFileInfo::exists(tab->root),
          QStringLiteral("the bundle tab keeps its root and takes no project song id"));

    // The project's own song with the same label is a different song: it
    // opens beside the bundle, never replacing or focusing it.
    const SongInfo *projectSong = nullptr;
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == QStringLiteral("mus_bundle"))
            projectSong = &song;
    }
    check(projectSong && projectSong->isPlayable(),
          QStringLiteral("the project has a mus_bundle of its own"));
    if (projectSong) {
        loadSong(*projectSong); // newTab=false, with the bundle tab active
        SongSession *own = m_active;
        check(m_tabs->count() == 2 && own && own != tab && !own->bundle && !own->doc.isLocked() &&
                  own->root == m_project.root() && own->songId == projectSong->id,
              QStringLiteral("the project song opens in its own editable tab"));
        check(sessionForLabel(QStringLiteral("mus_bundle")) == own,
              QStringLiteral("label lookup finds the project tab, not the bundle"));
        check(m_saveAction->isEnabled() && m_settingsAction->isEnabled() &&
                  m_exportBundleAction->isEnabled() && m_masterVolSpin->isEnabled() &&
                  !m_vgBrowser->viewOnly(),
              QStringLiteral("a project tab is fully editable again"));
        if (own) {
            own->doc.addNote(0, 0, 60, 24, 100);
            check(own->doc.isDirty() && own->doc.undoStack()->count() == 1,
                  QStringLiteral("project documents still take edits"));
            own->doc.undoStack()->undo();
        }

        // Bundle tabs are never persisted for restoreSession.
        m_persistSession = true; // QSettings are redirected by the caller
        persistOpenTabs();
        m_persistSession = false;
        QSettings settings;
        check(settings.value(QStringLiteral("lastOpenSongs")).toStringList() ==
                  QStringList{QStringLiteral("mus_bundle")},
              QStringLiteral("only the project tab is persisted"));

        m_tabs->setCurrentWidget(tab->view);
        check(m_active == tab && !m_saveAction->isEnabled() && m_vgBrowser->viewOnly() &&
                  m_audio.timeline() == tab->timeline.get(),
              QStringLiteral("switching back to the bundle tab re-gates and re-binds"));
        m_persistSession = true;
        persistOpenTabs();
        m_persistSession = false;
        check(settings.value(QStringLiteral("lastSongLabel")).toString() ==
                      QStringLiteral("mus_bundle") &&
                  settings.value(QStringLiteral("lastOpenSongs")).toStringList().size() == 1,
              QStringLiteral("an active bundle tab is not recorded as the last song"));
    }

    // A second project switch still keeps it, and drops the project tab.
    check(openProjectDir(projectRoot, /*interactive=*/false) && m_tabs->count() == 1 &&
              m_active == tab && m_audio.timeline() == tab->timeline.get(),
          QStringLiteral("a second project switch keeps only the bundle tab"));
    const QString lastRoot = tab->root;
    closeTab(0);
    check(m_tabs->count() == 0 && !QFileInfo::exists(lastRoot),
          QStringLiteral("final close removes the extraction dir"));
    return failures;
}

int runBundleTabSections(const QString &bundleZip, const QString &fixtureProject,
                         const QString &scratchDir)
{
    // Redirected settings: the user's real session is never touched.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "bundlecheck: FAIL: tab: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    MainWindow window;
    return window.runBundleTabCheck(bundleZip, fixtureProject, scratchDir);
}
