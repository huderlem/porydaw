#include <QApplication>

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

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
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
    const int editCheck = args.indexOf(QStringLiteral("--editcheck"));
    if (editCheck >= 0 && editCheck + 1 < args.size())
        return runEditCheck(args[editCheck + 1]);
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
    return app.exec();
}
