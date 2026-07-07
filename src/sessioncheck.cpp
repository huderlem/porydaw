#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <cstdio>

#include "mainwindow.h"

// --sessioncheck <projectRoot> <song>: session-persistence check. Verifies
// that restoreSession() reopens the remembered project (and song), is a
// no-op when nothing (or a vanished directory) is remembered, that closing
// a window records the session — window geometry included — and that a
// fresh window comes back at the saved geometry. QSettings is redirected
// into a temp dir first, so the user's real session is never read or
// written. Run against a scratch copy — closing writes view sidecars into
// the project.

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

    // 3. Project remembered but no song: the project opens, nothing loads.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("lastProjectDir"), projectRoot);
        settings.remove(QStringLiteral("lastSongLabel"));
        settings.sync();
        MainWindow window;
        window.restoreSession();
        check(window.statusBar()->currentMessage().startsWith(
                  QStringLiteral("Opened")),
              "remembered project did not open");
        check(window.windowTitle() == QStringLiteral("porydaw"),
              "a song loaded though none was remembered");
    }

    // 4. Project + song remembered: both come back; closing at a distinctive
    // size records the geometry and re-records the session.
    {
        QSettings().setValue(QStringLiteral("lastSongLabel"), songLabel);
        MainWindow window;
        window.restoreSession();
        check(window.windowTitle().startsWith(songLabel),
              "remembered song did not load");
        window.resize(999, 555);
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
    }

    // 5. Relaunch: geometry and session both come back.
    {
        MainWindow window;
        check(window.size() == QSize(999, 555),
              "new window did not restore the saved geometry");
        window.restoreSession();
        check(window.windowTitle().startsWith(songLabel),
              "relaunch did not restore project and song");
    }

    if (failures == 0)
        std::printf("sessioncheck: PASS\n");
    return failures == 0 ? 0 : 1;
}
