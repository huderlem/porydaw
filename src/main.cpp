#include <QApplication>

#include "mainwindow.h"

// viewcheck.cpp; the optional song label + path save one song's rendered view.
int runViewCheck(const QString &projectRoot, const QString &screenshotSong = QString(),
                 const QString &screenshotPath = QString());

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
