#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoGroup>
#include <algorithm>
#include <cstdio>

#include "mainwindow.h"

// --tabcheck <projectRoot> <songA> <songB>: multi-tab check. Two songs open
// in tabs with fully separate documents and undo stacks; switching tabs
// stops playback and rebinds the audio engine to the active tab's timeline
// and voicegroup; closing and replacing tabs behave; the open-tab set
// round-trips through QSettings (the second half, in runTabCheck's caller,
// restores it into a fresh window). A clean background tab whose voicegroup
// file changed on disk reloads it on activation. QSettings is redirected
// into a temp dir; view sidecars are written into the project on tab
// close — run against a scratch copy.

bool MainWindow::runTabCheck(const QString &projectRoot, const QString &songA,
                             const QString &songB)
{
    // m_persistSession stays true (the caller redirected QSettings) so the
    // tab persistence written for restoreSession is exercised for real.
    if (!m_audioOk) {
        std::fprintf(stderr, "tabcheck: no audio device available\n");
        return false;
    }
    if (!openProjectDir(projectRoot, /*interactive=*/false)) {
        std::fprintf(stderr, "tabcheck: project failed to open\n");
        return false;
    }

    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "tabcheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };
    QEventLoop loop;
    const auto wait = [&loop](int ms) {
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };

    // 1. First song loads into the first tab; the engine borrows its data.
    loadSongByLabel(songA);
    SongSession *tabA = m_active;
    if (!tabA || tabA->doc.label() != songA) {
        std::fprintf(stderr, "tabcheck: song '%s' did not load\n",
                     qUtf8Printable(songA));
        return false;
    }
    check(m_tabs->count() == 1, "first song did not open exactly one tab");
    check(m_audio.timeline() == tabA->timeline.get()
              && m_audio.voicegroup() == tabA->voicegroup,
          "engine is not borrowing the first tab's data");

    // 2. Second song in a new tab becomes the active one.
    loadSongByLabel(songB, /*newTab=*/true);
    SongSession *tabB = m_active;
    if (!tabB || tabB == tabA || tabB->doc.label() != songB) {
        std::fprintf(stderr, "tabcheck: song '%s' did not open in a new tab\n",
                     qUtf8Printable(songB));
        return false;
    }
    check(m_tabs->count() == 2, "second song did not open a second tab");
    check(m_audio.timeline() == tabB->timeline.get(),
          "engine did not rebind to the new tab");
    check(m_undoGroup->activeStack() == tabB->doc.undoStack(),
          "undo group is not on the new tab's stack");
    check(sessionForLabel(songA) == tabA && !tabA->doc.isDirty(),
          "first tab did not survive the second one opening");

    // 3. Separate documents and undo stacks: an edit in one tab dirties
    // only that tab.
    m_tabs->setCurrentWidget(tabA->view);
    check(m_active == tabA && m_audio.timeline() == tabA->timeline.get(),
          "switching tabs did not rebind the engine to the first tab");
    check(m_undoGroup->activeStack() == tabA->doc.undoStack(),
          "undo group did not follow the tab switch");
    if (tabA->doc.engineTrackCount() == 0) {
        std::fprintf(stderr, "tabcheck: song '%s' has no tracks\n",
                     qUtf8Printable(songA));
        return false;
    }
    uint64_t base = 0;
    for (const SmfTrack &tr : tabA->doc.smf().tracks)
        base = std::max(base, tr.endTick);
    tabA->doc.addNote(0, base + 96, 72, 24, 93);
    check(tabA->doc.isDirty() && !tabB->doc.isDirty(),
          "edit in one tab did not stay in that tab");
    check(m_tabs->tabText(m_tabs->indexOf(tabA->view)).endsWith(QLatin1Char('*')),
          "dirty tab title has no asterisk");
    check(!m_tabs->tabText(m_tabs->indexOf(tabB->view)).endsWith(QLatin1Char('*')),
          "clean tab title grew an asterisk");

    // 4. Switching tabs stops playback in the tab being left.
    m_audio.play();
    wait(200);
    check(m_audio.transport() == Transport::Playing, "playback did not start");
    m_tabs->setCurrentWidget(tabB->view);
    check(m_audio.transport() == Transport::Stopped,
          "switching tabs did not stop playback");
    check(m_audio.timeline() == tabB->timeline.get(),
          "engine timeline is not the newly active tab's");

    // 5. The dirty edit survives the round trip; each stack undoes its own.
    m_tabs->setCurrentWidget(tabA->view);
    check(tabA->doc.isDirty(), "first tab's edit vanished across the switch");
    m_undoGroup->activeStack()->undo();
    check(!tabA->doc.isDirty() && !tabB->doc.isDirty(),
          "undo through the group did not clean the active tab");

    // 6. Re-opening an already open song focuses its tab, no duplicates.
    loadSongByLabel(songB, /*newTab=*/true);
    check(m_tabs->count() == 2 && m_active == tabB,
          "re-opening an open song did not just focus its tab");

    // 7. Closing a tab hands the engine to the survivor.
    closeTab(m_tabs->indexOf(tabB->view));
    check(m_tabs->count() == 1 && m_active == tabA
              && m_audio.timeline() == tabA->timeline.get(),
          "closing the active tab did not fall back to the other tab");
    check(sessionForLabel(songB) == nullptr, "closed tab's session lingered");

    // 8. Plain activation replaces the current tab's song (tab count
    // unchanged) — the pre-tabs behavior.
    loadSongByLabel(songB);
    tabB = m_active;
    check(m_tabs->count() == 1 && tabB && tabB->doc.label() == songB
              && sessionForLabel(songA) == nullptr,
          "activating a song did not replace the current tab's");
    check(m_audio.timeline() == tabB->timeline.get(),
          "engine did not rebind after the in-place replace");

    // 9. The open-tab set is recorded for restoreSession.
    loadSongByLabel(songA, /*newTab=*/true);
    tabA = m_active;
    {
        QSettings settings;
        const QStringList open =
            settings.value(QStringLiteral("lastOpenSongs")).toStringList();
        check(open == QStringList({songB, songA}),
              "lastOpenSongs does not list the open tabs in order");
        check(settings.value(QStringLiteral("lastSongLabel")).toString() == songA,
              "lastSongLabel is not the active tab");
    }

    // 10. A clean background tab follows its voicegroup file when the file
    // changes on disk (as after a save from another tab).
    if (tabB->vgSource) {
        const QString vgPath = tabB->vgSource->filePath();
        QFile f(vgPath);
        if (f.open(QIODevice::ReadWrite)) {
            // Same bytes, definitely-new mtime.
            f.setFileTime(QDateTime::currentDateTime().addSecs(2),
                          QFileDevice::FileModificationTime);
            f.close();
            const LoadedVoiceGroup *before = tabB->voicegroup;
            m_tabs->setCurrentWidget(tabB->view);
            check(tabB->voicegroup != nullptr && tabB->voicegroup != before,
                  "clean tab did not reload its changed voicegroup file");
            check(tabB->vgSource && !tabB->vgSource->dirty(),
                  "voicegroup auto-refresh left the source dirty");
        } else {
            std::printf("tabcheck: note: voicegroup file not writable, "
                        "auto-refresh check skipped\n");
            m_tabs->setCurrentWidget(tabB->view);
        }
    } else {
        std::printf("tabcheck: note: no editable voicegroup source, "
                    "auto-refresh check skipped\n");
        m_tabs->setCurrentWidget(tabB->view);
    }

    std::printf("tabcheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures == 0;
}

int runTabCheck(const QString &projectRoot, const QString &songA, const QString &songB)
{
    // Redirected settings: the user's real session is never touched.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "tabcheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());

    {
        MainWindow window;
        if (!window.runTabCheck(projectRoot, songA, songB))
            return 1;
    } // the first window's audio device is gone before the second opens

    // Relaunch: the whole tab set comes back, with the same active tab
    // (songB was active at the end of runTabCheck).
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "tabcheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };
    {
        MainWindow window;
        window.restoreSession();
        auto *tabs = window.findChild<QTabWidget *>();
        check(tabs && tabs->count() == 2, "relaunch did not restore both tabs");
        if (tabs && tabs->count() == 2) {
            check(tabs->tabText(0) == songB && tabs->tabText(1) == songA,
                  "restored tabs are not in the saved order");
        }
        check(window.windowTitle().startsWith(songB),
              "relaunch did not re-activate the last active tab");
    }
    if (failures == 0)
        std::printf("tabcheck: restore PASS\n");
    return failures == 0 ? 0 : 1;
}
