#include <QApplication>
#include <QStyleHints>

#include "mainwindow.h"

// viewcheck.cpp; the optional song label + path save one song's rendered view.
int runViewCheck(const QString &projectRoot, const QString &screenshotSong = QString(),
                 const QString &screenshotPath = QString());
// roundtrip.cpp; M2 save-fidelity check through the project's real mid2agb.
int runRoundTrip(const QString &projectRoot, const QString &mid2agbPath = QString());
// editcheck.cpp; M2 undo-integrity check over every edit-operation type.
int runEditCheck(const QString &projectRoot);
// savecheck.cpp; M2 edited-save check (writes into the project: use a copy).
int runSaveCheck(const QString &projectRoot, const QString &songLabel,
                 const QString &mid2agbPath = QString());
// onboardcheck.cpp; M3 New Song + import check (writes into the project: use a copy).
int runOnboardCheck(const QString &projectRoot, const QString &mid2agbPath = QString());
// vgcheck.cpp; voicegroup edit/save/create check (writes into the project: use a copy).
int runVgCheck(const QString &projectRoot, const QString &songLabel);
// vgsavecheck.cpp; unified song+voicegroup undo/save check through MainWindow,
// against redirected QSettings (writes into the project: use a copy).
int runVgSaveCheck(const QString &projectRoot, const QString &songLabel);
// exportcheck.cpp; WAV export check (writes a .wav into the project: use a copy).
int runExportCheck(const QString &projectRoot, const QString &songLabel);
// mkcheck.cpp; songs.mk-fallback parse/write check for projects with no
// midi.cfg (writes into the project: use a copy).
int runMkCheck(const QString &projectRoot, const QString &songLabel);
// sessioncheck.cpp; session restore/persistence check against redirected
// QSettings (writes view sidecars into the project: use a copy).
int runSessionCheck(const QString &projectRoot, const QString &songLabel);
// tabcheck.cpp; multi-tab check against redirected QSettings (writes view
// sidecars into the project: use a copy).
int runTabCheck(const QString &projectRoot, const QString &songA,
                const QString &songB);
// rollcheck.cpp; piano-roll gesture check (pencil draw + velocity latch +
// header-drag track reorder); the optional path saves the rendered view
// after the gestures.
int runRollCheck(const QString &projectRoot, const QString &songLabel,
                 const QString &screenshotPath = QString());
// loopcheck.cpp; loop-wrap playback check (self-contained, no project needed).
int runLoopCheck();
// primecheck.cpp; audition voice-priming check (self-contained, no project needed).
int runPrimeCheck();
// transportcheck.cpp; playback-start halts ringing auditions (self-contained,
// no project needed; SKIPs without an audio device).
int runTransportCheck();
// eventviewcheck.cpp; raw MIDI event list check (model API + offscreen UI);
// the optional song label + path save that song's rendered event list.
int runEventViewCheck(const QString &projectRoot,
                      const QString &screenshotSong = QString(),
                      const QString &screenshotPath = QString());

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // The OS dark theme renders badly (notably on Windows); force light until
    // the widgets are audited for dark palettes.
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    QApplication::setApplicationName("porydaw");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("huderlem");

    const QStringList args = app.arguments();
    const int selfTest = args.indexOf(QStringLiteral("--selftest"));
    if (selfTest >= 0 && selfTest + 2 < args.size()) {
        MainWindow window;
        return window.runSelfTest(args[selfTest + 1], args[selfTest + 2]) ? 0 : 1;
    }
    const int saveCheck = args.indexOf(QStringLiteral("--savecheck"));
    if (saveCheck >= 0 && saveCheck + 2 < args.size()) {
        const QString mid2agb = saveCheck + 3 < args.size() ? args[saveCheck + 3] : QString();
        return runSaveCheck(args[saveCheck + 1], args[saveCheck + 2], mid2agb);
    }
    const int onboardCheck = args.indexOf(QStringLiteral("--onboardcheck"));
    if (onboardCheck >= 0 && onboardCheck + 1 < args.size()) {
        const QString mid2agb =
            onboardCheck + 2 < args.size() ? args[onboardCheck + 2] : QString();
        return runOnboardCheck(args[onboardCheck + 1], mid2agb);
    }
    const int vgCheck = args.indexOf(QStringLiteral("--vgcheck"));
    if (vgCheck >= 0 && vgCheck + 2 < args.size())
        return runVgCheck(args[vgCheck + 1], args[vgCheck + 2]);
    const int vgSaveCheck = args.indexOf(QStringLiteral("--vgsavecheck"));
    if (vgSaveCheck >= 0 && vgSaveCheck + 2 < args.size())
        return runVgSaveCheck(args[vgSaveCheck + 1], args[vgSaveCheck + 2]);
    const int exportCheck = args.indexOf(QStringLiteral("--exportcheck"));
    if (exportCheck >= 0 && exportCheck + 2 < args.size())
        return runExportCheck(args[exportCheck + 1], args[exportCheck + 2]);
    const int mkCheck = args.indexOf(QStringLiteral("--mkcheck"));
    if (mkCheck >= 0 && mkCheck + 2 < args.size())
        return runMkCheck(args[mkCheck + 1], args[mkCheck + 2]);
    const int sessionCheck = args.indexOf(QStringLiteral("--sessioncheck"));
    if (sessionCheck >= 0 && sessionCheck + 2 < args.size())
        return runSessionCheck(args[sessionCheck + 1], args[sessionCheck + 2]);
    const int tabCheck = args.indexOf(QStringLiteral("--tabcheck"));
    if (tabCheck >= 0 && tabCheck + 3 < args.size())
        return runTabCheck(args[tabCheck + 1], args[tabCheck + 2],
                           args[tabCheck + 3]);
    if (args.contains(QStringLiteral("--loopcheck")))
        return runLoopCheck();
    if (args.contains(QStringLiteral("--primecheck")))
        return runPrimeCheck();
    if (args.contains(QStringLiteral("--transportcheck")))
        return runTransportCheck();
    const int editCheck = args.indexOf(QStringLiteral("--editcheck"));
    if (editCheck >= 0 && editCheck + 1 < args.size())
        return runEditCheck(args[editCheck + 1]);
    const int eventViewCheck = args.indexOf(QStringLiteral("--eventviewcheck"));
    if (eventViewCheck >= 0 && eventViewCheck + 1 < args.size()) {
        const QString song =
            eventViewCheck + 2 < args.size() ? args[eventViewCheck + 2] : QString();
        const QString path =
            eventViewCheck + 3 < args.size() ? args[eventViewCheck + 3] : QString();
        return runEventViewCheck(args[eventViewCheck + 1], song, path);
    }
    const int rollCheck = args.indexOf(QStringLiteral("--rollcheck"));
    if (rollCheck >= 0 && rollCheck + 2 < args.size()) {
        const QString path =
            rollCheck + 3 < args.size() ? args[rollCheck + 3] : QString();
        return runRollCheck(args[rollCheck + 1], args[rollCheck + 2], path);
    }
    const int roundTrip = args.indexOf(QStringLiteral("--roundtrip"));
    if (roundTrip >= 0 && roundTrip + 1 < args.size()) {
        const QString mid2agb = roundTrip + 2 < args.size() ? args[roundTrip + 2] : QString();
        return runRoundTrip(args[roundTrip + 1], mid2agb);
    }
    const int viewCheck = args.indexOf(QStringLiteral("--viewcheck"));
    if (viewCheck >= 0 && viewCheck + 1 < args.size()) {
        const QString song = viewCheck + 2 < args.size() ? args[viewCheck + 2] : QString();
        const QString path = viewCheck + 3 < args.size() ? args[viewCheck + 3] : QString();
        return runViewCheck(args[viewCheck + 1], song, path);
    }

    MainWindow window;
    window.show();
    window.restoreSession();
    return app.exec();
}
