#include <QApplication>

#include "mainwindow.h"

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

    MainWindow window;
    window.show();
    return app.exec();
}
