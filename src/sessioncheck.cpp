#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <cstdio>

#include "mainwindow.h"

// --sessioncheck <projectRoot> <song>: session-persistence check. Verifies
// that restoreSession() reopens the remembered project (and song), is a
// no-op when nothing (or a vanished directory) is remembered, that closing
// a window records the session — window geometry and the song list's
// filter state (search text, sort, category) included — and that a fresh
// window comes back at the saved geometry with the filters reapplied.
// QSettings is redirected into a temp dir first, so the user's real
// session is never read or written. Run against a scratch copy — closing
// writes view sidecars into the project.

int runSessionCheck(const QString &projectRoot, const QString &songLabel)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "sessioncheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());

    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "sessioncheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    // 1. Nothing remembered: restore is a no-op.
    {
        MainWindow window;
        window.restoreSession();
        check(window.windowTitle() == QStringLiteral("porydaw"),
              "restore with no remembered project opened something");
    }

    // 2. The remembered project directory vanished: still a no-op.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("lastProjectDir"),
                          settingsDir.path() + QStringLiteral("/gone"));
        settings.setValue(QStringLiteral("lastSongLabel"), songLabel);
        settings.sync();
        MainWindow window;
        window.restoreSession();
        check(window.windowTitle() == QStringLiteral("porydaw"),
              "restore with a vanished project dir opened something");
    }

    // 3. Project remembered but no song: the project opens (titled after its
    // directory), nothing loads.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("lastProjectDir"), projectRoot);
        settings.remove(QStringLiteral("lastSongLabel"));
        settings.remove(QStringLiteral("lastOpenSongs"));
        settings.sync();
        MainWindow window;
        window.restoreSession();
        check(window.statusBar()->currentMessage().startsWith(
                  QStringLiteral("Opened")),
              "remembered project did not open");
        check(window.windowTitle()
                  == QStringLiteral("%1 — porydaw").arg(QDir(projectRoot).dirName()),
              "title is not the project name (or a song loaded unasked)");
    }

    // 4. Project + song remembered: both come back; closing at a distinctive
    // size records the geometry, the song list's filter state, and
    // re-records the session. Only lastSongLabel is set here — the pre-tabs
    // session format — so this also proves the single-label fallback
    // restores as one tab; the close then records the tab-list format that
    // block 5 restores from.
    QString filterCategory;
    {
        QSettings().setValue(QStringLiteral("lastSongLabel"), songLabel);
        MainWindow window;
        window.restoreSession();
        check(window.windowTitle().startsWith(songLabel),
              "remembered song did not load");
        // The song list (the app's only QListWidget) tracks the loaded song.
        auto *list = window.findChild<QListWidget *>();
        check(list && list->currentItem()
                  && list->currentItem()->text().startsWith(songLabel),
              "restored song is not selected in the song list");
        // Distinctive filter state — search text, A–Z sort, a real
        // category — for block 5 to find again after the relaunch.
        auto *search = window.findChild<QLineEdit *>(QStringLiteral("songListSearch"));
        auto *category =
            window.findChild<QComboBox *>(QStringLiteral("songListCategory"));
        auto *sort = window.findChild<QComboBox *>(QStringLiteral("songListSort"));
        if (check(search && category && sort, "song list filter widgets not found")) {
            if (category->count() > 1)
                category->setCurrentIndex(1);
            filterCategory = category->currentData().toString();
            sort->setCurrentIndex(1);
            search->setText(QStringLiteral("filterme"));
        }
        // Must fit the offscreen platform's 800x600 virtual screen: newer Qt
        // clamps restoreGeometry() to the available screen, so an oversized
        // window would come back shrunk and block 5 would fail.
        window.resize(777, 505);
        check(window.close(), "close was refused");
        QSettings settings;
        check(!settings.value(QStringLiteral("windowGeometry"))
                   .toByteArray()
                   .isEmpty(),
              "close did not save window geometry");
        check(settings.value(QStringLiteral("lastProjectDir")).toString()
                  == projectRoot,
              "close lost the remembered project");
        check(settings.value(QStringLiteral("lastSongLabel")).toString()
                  == songLabel,
              "close lost the remembered song");
        check(settings.value(QStringLiteral("lastOpenSongs")).toStringList()
                  == QStringList(songLabel),
              "close did not record the open tab list");
        check(settings.value(QStringLiteral("songFilterText")).toString()
                  == QStringLiteral("filterme"),
              "close did not save the song filter text");
    }

    // 5. Relaunch: geometry, session, and song-list filters all come back.
    // The category can only reapply once the project's songs populate the
    // combo, so it's checked after restoreSession().
    {
        MainWindow window;
        check(window.size() == QSize(777, 505),
              "new window did not restore the saved geometry");
        window.restoreSession();
        check(window.windowTitle().startsWith(songLabel),
              "relaunch did not restore project and song");
        auto *search = window.findChild<QLineEdit *>(QStringLiteral("songListSearch"));
        check(search && search->text() == QStringLiteral("filterme"),
              "relaunch did not restore the song filter text");
        auto *sort = window.findChild<QComboBox *>(QStringLiteral("songListSort"));
        check(sort && sort->currentIndex() == 1,
              "relaunch did not restore the song sort order");
        auto *category =
            window.findChild<QComboBox *>(QStringLiteral("songListCategory"));
        check(category && category->currentData().toString() == filterCategory,
              "relaunch did not restore the song category filter");
    }

    if (failures == 0)
        std::printf("sessioncheck: PASS\n");
    return failures == 0 ? 0 : 1;
}
