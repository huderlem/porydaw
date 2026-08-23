#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontInfo>
#include <QKeyEvent>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>

#include "mainwindow.h"
#include "ui/appearancesettingspage.h"
#include "ui/audiosettingspage.h"
#include "ui/keyboardshortcutspage.h"
#include "ui/settingsdialog.h"
#include "ui/typography.h"

// --settingscheck: the Settings window (Edit → Settings…). Self-contained —
// no project needed; QSettings is redirected into a temp dir first so the
// user's real preferences are never read or written. Verifies that the
// menu action opens the window, the section list drives the page stack and
// the last section is remembered across a close/reopen and a relaunch,
// that the Audio page's controls apply immediately (engine knobs persist
// as they change; the output level reaches the engine) and Restore
// Defaults returns them all, that the Appearance page's font choice lands
// in typography and persists, and that the Shortcuts page is the real
// editor. With a shot path, each page is also saved as <shot>-<page>.png.

int runSettingsCheck(const QString &shotPath)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "settingscheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "settingscheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    {
        MainWindow window;
        SettingsDialog *dialog = window.settingsDialog();
        if (!check(dialog != nullptr, "main window has no Settings window"))
            return 1;
        auto *action = window.findChild<QAction *>(QStringLiteral("editSettingsAction"));
        auto *sections = dialog->findChild<QListWidget *>(QStringLiteral("settingsSections"));
        auto *stack = dialog->findChild<QStackedWidget *>(QStringLiteral("settingsStack"));
        auto *closeButton = dialog->findChild<QPushButton *>(QStringLiteral("settingsCloseButton"));
        if (!check(action && sections && stack && closeButton, "Settings window chrome not found"))
            return 1;

        // 1. Merely building the window never writes QSettings.
        check(!QSettings().contains(QStringLiteral("settings/lastPage")),
              "constructing the Settings window wrote the remembered section");

        // 2. The menu action shows it, on Audio the first time.
        check(!dialog->isVisible(), "Settings window was visible before being opened");
        action->trigger();
        QApplication::processEvents();
        check(dialog->isVisible(), "Edit → Settings did not open the window");
        check(sections->count() == 3, "Settings window does not have three sections");
        check(dialog->currentPage() == SettingsDialog::Page::Audio &&
                  stack->currentWidget() == dialog->audioPage(),
              "Settings window did not open on Audio first");

        const auto shoot = [&](const char *page) {
            if (shotPath.isEmpty())
                return;
            QApplication::processEvents();
            QString path = shotPath;
            const int dot = path.lastIndexOf(QLatin1Char('.'));
            const QString suffix = QStringLiteral("-%1").arg(QLatin1String(page));
            path = dot > 0 ? path.left(dot) + suffix + path.mid(dot) : path + suffix;
            dialog->grab().save(path);
        };
        shoot("audio");

        // 3. The section list drives the stack; the choice is remembered
        // across close/reopen.
        sections->setCurrentRow(1);
        check(dialog->currentPage() == SettingsDialog::Page::Appearance &&
                  stack->currentWidget() == dialog->appearancePage(),
              "selecting Appearance did not switch the page");
        shoot("appearance");
        sections->setCurrentRow(2);
        check(stack->currentWidget() == dialog->shortcutsPage() &&
                  dialog->shortcutsPage()->findChild<QTreeWidget *>() != nullptr,
              "selecting Keyboard Shortcuts did not show the shortcuts editor");
        shoot("shortcuts");
        sections->setCurrentRow(1);
        closeButton->click();
        QApplication::processEvents();
        check(!dialog->isVisible(), "Close did not hide the Settings window");
        check(QSettings().value(QStringLiteral("settings/lastPage")).toString() ==
                  QStringLiteral("appearance"),
              "the selected section was not remembered");
        action->trigger();
        check(dialog->isVisible() && dialog->currentPage() == SettingsDialog::Page::Appearance,
              "reopening did not return to the last section");

        // 4. Audio page: every control applies as it changes.
        dialog->showPage(SettingsDialog::Page::Audio);
        auto *output = dialog->findChild<QSlider *>(QStringLiteral("settingsOutputLevel"));
        auto *polyphony = dialog->findChild<QSpinBox *>(QStringLiteral("settingsPcmPolyphony"));
        auto *mixRate = dialog->findChild<QComboBox *>(QStringLiteral("settingsPcmMixRate"));
        auto *analog = dialog->findChild<QCheckBox *>(QStringLiteral("settingsAnalogFilter"));
        auto *defaults =
            dialog->findChild<QPushButton *>(QStringLiteral("settingsAudioRestoreDefaults"));
        if (check(output && polyphony && mixRate && analog && defaults,
                  "Audio page controls not found")) {
            check(output->value() == AudioSettingsPage::kOutputLevelDefault &&
                      window.audio().outputGain() == 1.0f,
                  "output level did not start at unity");
            output->setValue(150);
            check(window.audio().outputGain() == 1.5f, "output level did not reach the engine");
            check(QSettings().value(QStringLiteral("outputLevel")).toInt() == 150,
                  "output level was not persisted as it changed");

            polyphony->setValue(3);
            check(QSettings().value(QStringLiteral("engine/maxPcmChannels")).toInt() == 3,
                  "PCM polyphony was not persisted as it changed");
            mixRate->setCurrentIndex(mixRate->findData(0));
            check(QSettings().value(QStringLiteral("engine/pcmMixRate")).toDouble() == 0.0,
                  "PCM mix rate was not persisted as it changed");
            analog->setChecked(true);
            check(QSettings().value(QStringLiteral("engine/analogFilter")).toBool(),
                  "analog filter was not persisted as it changed");
            check(dialog->audioPage()->engineSettings().maxPcmChannels == 3 &&
                      dialog->audioPage()->engineSettings().pcmMixRate == 0.0f &&
                      dialog->audioPage()->engineSettings().analogFilter,
                  "Audio page does not report what its controls show");

            defaults->click();
            const EngineSettings factory;
            check(output->value() == AudioSettingsPage::kOutputLevelDefault &&
                      window.audio().outputGain() == 1.0f,
                  "Restore Defaults did not reset the output level");
            check(polyphony->value() == factory.maxPcmChannels &&
                      mixRate->currentData().toInt() == int(factory.pcmMixRate) &&
                      analog->isChecked() == factory.analogFilter,
                  "Restore Defaults did not reset the engine knobs");
            check(QSettings().value(QStringLiteral("engine/maxPcmChannels")).toInt() ==
                          factory.maxPcmChannels &&
                      QSettings().value(QStringLiteral("engine/pcmMixRate")).toDouble() ==
                          double(factory.pcmMixRate) &&
                      !QSettings().value(QStringLiteral("engine/analogFilter")).toBool(),
                  "Restore Defaults did not persist the factory engine settings");
            // Restore Defaults on factory knobs is a no-op: it must not
            // report (the owner restarts the audio device on every report).
            int reports = 0;
            QObject::connect(dialog, &SettingsDialog::engineSettingsChanged, dialog,
                             [&reports](const EngineSettings &) { ++reports; });
            defaults->click();
            check(reports == 0, "Restore Defaults reported unchanged engine settings");

            // Enter in the polyphony spinbox commits the typed value; it
            // must not also fire Restore Defaults (the window's only push
            // button would otherwise be its default button).
            polyphony->setValue(3);
            output->setValue(150);
            polyphony->setFocus();
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(polyphony, &press);
            QApplication::sendEvent(polyphony, &release);
            QApplication::processEvents();
            check(polyphony->value() == 3 && output->value() == 150,
                  "Enter in the polyphony spinbox triggered Restore Defaults");
            // Leaves polyphony 3 behind for the relaunch check.
        }

        // 5. Appearance page: the font choice lands and persists.
        auto *systemFont = dialog->findChild<QCheckBox *>(QStringLiteral("settingsSystemFont"));
        if (check(systemFont != nullptr, "system-font checkbox not found")) {
            check(!systemFont->isChecked(), "system font did not start off");
            systemFont->setChecked(true);
            check(QFontInfo(QApplication::font()).family() == typography::systemFontFamily(),
                  "system-font checkbox did not install the platform face");
            check(QSettings().value(QStringLiteral("systemFont")).toBool(),
                  "system font choice was not persisted");
            systemFont->setChecked(false);
        }

        // 6. Escape closes too (it is a dialog), and the window survives.
        action->trigger();
        dialog->reject();
        check(!dialog->isVisible(), "Escape did not close the Settings window");
    }

    // 7. A fresh window comes back with the persisted preferences loaded
    // into the page, and the remembered section selected.
    {
        MainWindow window;
        SettingsDialog *dialog = window.settingsDialog();
        auto *polyphony = dialog->findChild<QSpinBox *>(QStringLiteral("settingsPcmPolyphony"));
        check(polyphony && polyphony->value() == 3,
              "relaunch did not load the persisted PCM polyphony into the Audio page");
        check(dialog->currentPage() == SettingsDialog::Page::Audio,
              "relaunch did not remember the last section");
    }

    if (failures == 0)
        std::printf("settingscheck: OK\n");
    return failures ? 1 : 0;
}
