#include "mainwindow.h"

#include <QApplication>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include "m4a_engine.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("porydaw");
    resize(1024, 640);

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open Project..."), QKeySequence::Open, this, &MainWindow::openProject);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    auto *placeholder = new QLabel(tr("Open a decomp project directory to get started."), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

    // Smoke test: prove the poryaaaa engine core is linked and functional.
    auto *engine = new M4AEngine();
    m4a_engine_init(engine, 48000.0f);
    m4a_engine_destroy(engine);
    delete engine;
    statusBar()->showMessage(tr("poryaaaa engine initialized (48000 Hz)"));
}

void MainWindow::openProject()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Decomp Project"));
    if (dir.isEmpty())
        return;

    projectDir = dir;
    statusBar()->showMessage(tr("Project: %1").arg(projectDir));
}
