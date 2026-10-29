#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>

#include "mainwindow.h"
#include "ui/appearancesettingspage.h"
#include "ui/audiosettingspage.h"
#include "ui/keyboardshortcutspage.h"
#include "ui/midisettingspage.h"
#include "ui/settingsdialog.h"
#include "ui/typography.h"
#ifdef PORYDAW_SCRIPTING
#include <QLineEdit>

#include "scripting/scripthost.h"
#endif

// --settingscheck: the Settings window (Edit → Settings…). Self-contained —
// no project needed; QSettings is redirected into a temp dir first so the
// user's real preferences are never read or written. Verifies that the
// menu action opens the window, the section list drives the page stack and
// the last section is remembered across a close/reopen and a relaunch,
// that the Audio page's controls apply immediately (engine knobs persist
// as they change; the output level reaches the engine) and Restore
// Defaults returns them all, that the Appearance page's font choice lands
// in typography and persists, and that the Shortcuts page is the real
// editor. The CPU budget is in cycles (60,000 on a fresh store) with a live
// percent-of-VBlank caption, and a legacy engine/cpuBudgetPercent converts
// once to engine/cpuBudgetCycles. With a shot path, each page is also saved
// as <shot>-<page>.png.

int runSettingsCheck(const QString &shotPath)
{
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "settingscheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    // The plugins-folder check returns to the *default* folder, which must
    // not be the user's real app data (their plugins would load and run).
    QStandardPaths::setTestModeEnabled(true);

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
#ifdef PORYDAW_SCRIPTING
        check(sections->count() == 5, "Settings window does not have five sections");
#else
        check(sections->count() == 4, "Settings window does not have four sections");
#endif
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
        check(dialog->currentPage() == SettingsDialog::Page::Midi &&
                  stack->currentWidget() == dialog->midiPage(),
              "selecting MIDI did not switch the page");
        shoot("midi");
        sections->setCurrentRow(2);
        check(dialog->currentPage() == SettingsDialog::Page::Appearance &&
                  stack->currentWidget() == dialog->appearancePage(),
              "selecting Appearance did not switch the page");
        shoot("appearance");
        sections->setCurrentRow(3);
        check(stack->currentWidget() == dialog->shortcutsPage() &&
                  dialog->shortcutsPage()->findChild<QTreeWidget *>() != nullptr,
              "selecting Keyboard Shortcuts did not show the shortcuts editor");
        shoot("shortcuts");
#ifdef PORYDAW_SCRIPTING
        sections->setCurrentRow(4);
        check(dialog->currentPage() == SettingsDialog::Page::Plugins &&
                  stack->currentWidget() == dialog->pluginsPage() &&
                  dialog->pluginsPage()->findChild<QTreeWidget *>() != nullptr,
              "selecting Plugins did not show the plugins page");
        shoot("plugins");

        // The plugins folder is a setting: the row shows the folder in
        // use, a changed folder persists and reloads the host at once, Use
        // Default clears it, and PORYDAW_PLUGINS_DIR beats the setting for
        // a run without touching it.
        {
            QTemporaryDir customDir;
            scripting::ScriptHost *host = window.scriptHost();
            auto *folder = dialog->pluginsPage()->findChild<QLineEdit *>(
                QStringLiteral("settingsPluginsFolder"));
            auto *change = dialog->pluginsPage()->findChild<QPushButton *>(
                QStringLiteral("settingsPluginsChangeFolder"));
            auto *useDefault = dialog->pluginsPage()->findChild<QPushButton *>(
                QStringLiteral("settingsPluginsDefaultFolder"));
            const bool envSet = !scripting::ScriptHost::environmentPluginsDir().isEmpty();
            if (envSet)
                std::fprintf(stderr, "settingscheck: PORYDAW_PLUGINS_DIR is set; skipping the "
                                     "plugins-folder setting check\n");
            if (check(host && folder && change && useDefault && customDir.isValid(),
                      "Plugins page folder row not found") &&
                !envSet) {
                const QString defaultDir = scripting::ScriptHost::defaultPluginsDir();
                check(QDir::cleanPath(host->pluginsDir()) == QDir::cleanPath(defaultDir) &&
                          folder->text() == QDir::toNativeSeparators(defaultDir) &&
                          !useDefault->isEnabled() && change->isEnabled(),
                      "Plugins page did not start on the default folder");
                host->setPluginsDirSetting(customDir.path());
                host->loadAll();
                QApplication::processEvents();
                check(QSettings().value(QStringLiteral("pluginsDir")).toString() ==
                          customDir.path(),
                      "changed plugins folder was not persisted");
                check(host->pluginsDir() == customDir.path() &&
                          folder->text() == QDir::toNativeSeparators(customDir.path()) &&
                          useDefault->isEnabled(),
                      "changed plugins folder did not reach the host and the page");
                check(scripting::ScriptHost::resolvePluginsDir() == customDir.path(),
                      "a fresh host would not start on the saved folder");
                QSettings().setValue(QStringLiteral("pluginsDir"), QStringLiteral("plugins"));
                check(scripting::ScriptHost::configuredPluginsDir() == defaultDir,
                      "a relative saved folder must fall back to the default");
                QSettings().setValue(QStringLiteral("pluginsDir"), customDir.path() + "/./");
                check(scripting::ScriptHost::configuredPluginsDir() == customDir.path(),
                      "the saved folder was not normalised");
                host->setPluginsDirSetting(customDir.path());
                qputenv("PORYDAW_PLUGINS_DIR", "/nonexistent/env-plugins");
                check(scripting::ScriptHost::resolvePluginsDir() ==
                              QStringLiteral("/nonexistent/env-plugins") &&
                          scripting::ScriptHost::configuredPluginsDir() == customDir.path(),
                      "PORYDAW_PLUGINS_DIR did not override the saved folder");
                host->setPluginsDirSetting(defaultDir);
                check(host->pluginsDir() == customDir.path() &&
                          !QSettings().contains(QStringLiteral("pluginsDir")),
                      "with the environment override, the setting must persist without moving "
                      "the host");
                qunsetenv("PORYDAW_PLUGINS_DIR");
                host->setPluginsDirSetting(customDir.path());
                host->loadAll();
                QApplication::processEvents();
                check(useDefault->isEnabled(),
                      "Use Default was not re-enabled after the folder changed");
                useDefault->click();
                QApplication::processEvents();
                check(QDir::cleanPath(host->pluginsDir()) == QDir::cleanPath(defaultDir) &&
                          !QSettings().contains(QStringLiteral("pluginsDir")) &&
                          !useDefault->isEnabled() &&
                          folder->text() == QDir::toNativeSeparators(defaultDir),
                      "Use Default did not return to the default folder");
            }
        }
#endif
        sections->setCurrentRow(2);
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
        auto *hq = dialog->findChild<QCheckBox *>(QStringLiteral("settingsHqMixer"));
        auto *cpuBudget = dialog->findChild<QSpinBox *>(QStringLiteral("settingsCpuBudget"));
        auto *cpuBudgetPercent =
            dialog->findChild<QLabel *>(QStringLiteral("settingsCpuBudgetPercent"));
        auto *defaults =
            dialog->findChild<QPushButton *>(QStringLiteral("settingsAudioRestoreDefaults"));
        if (check(output && polyphony && mixRate && hq && cpuBudget && cpuBudgetPercent && defaults,
                  "Audio page controls not found")) {
            // The CPU budget (docs/cpu-meter/PLAN.md D1, amended
            // 2026-10-29): GBA cycles per frame, 1,000 to the whole frame
            // (280,896), default 60,000 on a fresh store, in steps of 1,000,
            // its percent of the VBlank window beside it. It reports,
            // persists, and never restarts the engine.
            check(cpuBudget->value() == AudioSettingsPage::kCpuBudgetDefault &&
                      cpuBudget->value() == 60000 && cpuBudget->minimum() == 1000 &&
                      cpuBudget->maximum() == 280896 && cpuBudget->singleStep() == 1000 &&
                      cpuBudget->suffix() == QStringLiteral(" cycles"),
                  "CPU budget's default, range, step or suffix is not 60000 cycles within "
                  "1000..280896 by 1000");
            check(window.cpuBudgetCycles() == 60000,
                  "a fresh settings store did not give the window a 60000-cycle budget");
            check(cpuBudgetPercent->text() == QStringLiteral("72% of the VBlank window"),
                  "CPU budget caption does not read 72% at the default");
            {
                int engineReports = 0;
                const auto connection =
                    QObject::connect(dialog, &SettingsDialog::engineSettingsChanged, dialog,
                                     [&engineReports](const EngineSettings &) { ++engineReports; });
                cpuBudget->setValue(41888);
                QObject::disconnect(connection);
                check(QSettings().value(QStringLiteral("engine/cpuBudgetCycles")).toInt() ==
                              41888 &&
                          dialog->audioPage()->cpuBudget() == 41888 &&
                          window.cpuBudgetCycles() == 41888,
                      "CPU budget was not persisted as it changed");
                check(!QSettings().contains(QStringLiteral("engine/cpuBudgetPercent")),
                      "changing the CPU budget wrote the legacy percent key");
                check(cpuBudgetPercent->text() == QStringLiteral("50% of the VBlank window"),
                      "CPU budget caption did not follow the spin box to 50%");
                check(engineReports == 0, "changing the CPU budget reported engine settings");
                cpuBudget->stepUp();
                check(cpuBudget->value() == 42888, "CPU budget does not step by 1000 cycles");
                dialog->audioPage()->setCpuBudget(83776);
                check(cpuBudgetPercent->text() == QStringLiteral("100% of the VBlank window"),
                      "CPU budget caption does not read 100% at the VBlank window");
                dialog->audioPage()->setCpuBudget(1000000);
                check(cpuBudget->value() == 280896, "CPU budget was not clamped to the frame");
                check(cpuBudgetPercent->text() == QStringLiteral("335% of the VBlank window"),
                      "CPU budget caption does not read 335% at the whole frame");
                dialog->audioPage()->setCpuBudget(10);
                check(cpuBudget->value() == 1000, "CPU budget was not clamped to 1000 cycles");
                cpuBudget->setValue(80000);
            }

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
            mixRate->setCurrentIndex(mixRate->findData(21024));
            check(QSettings().value(QStringLiteral("engine/pcmMixRate")).toDouble() == 21024.0,
                  "PCM mix rate was not persisted as it changed");
            check(mixRate->findData(0) < 0, "the retired host-rate item is still offered");
            hq->setChecked(true);
            check(QSettings().value(QStringLiteral("engine/hqMixer")).toBool(),
                  "HQ mixer was not persisted as it changed");
            check(dialog->audioPage()->engineSettings().maxPcmChannels == 3 &&
                      dialog->audioPage()->engineSettings().pcmMixRate == 21024.0f &&
                      dialog->audioPage()->engineSettings().hqMixer,
                  "Audio page does not report what its controls show");
            // The HQ mixer has no 7884 Hz: the item greys out, and choosing
            // it anyway lands on the GBA default (what the engine would play).
            {
                auto *model = qobject_cast<QStandardItemModel *>(mixRate->model());
                const int idx7884 = mixRate->findData(kGbaMixRateHqUnsupported);
                check(model && idx7884 >= 0 && !model->item(idx7884)->isEnabled(),
                      "7884 Hz stayed selectable under the HQ mixer");
                hq->setChecked(false);
                check(model && idx7884 >= 0 && model->item(idx7884)->isEnabled(),
                      "7884 Hz did not come back without the HQ mixer");
                mixRate->setCurrentIndex(idx7884);
                hq->setChecked(true);
                check(mixRate->currentData().toInt() == 13379 &&
                          dialog->audioPage()->engineSettings().pcmMixRate == 13379.0f &&
                          QSettings().value(QStringLiteral("engine/pcmMixRate")).toDouble() ==
                              13379.0,
                      "HQ mixer over 7884 Hz did not snap the rate to 13379");
            }

            defaults->click();
            const EngineSettings factory;
            check(output->value() == AudioSettingsPage::kOutputLevelDefault &&
                      window.audio().outputGain() == 1.0f,
                  "Restore Defaults did not reset the output level");
            check(polyphony->value() == factory.maxPcmChannels &&
                      mixRate->currentData().toInt() == int(factory.pcmMixRate) &&
                      hq->isChecked() == factory.hqMixer,
                  "Restore Defaults did not reset the engine knobs");
            check(cpuBudget->value() == AudioSettingsPage::kCpuBudgetDefault &&
                      QSettings().value(QStringLiteral("engine/cpuBudgetCycles")).toInt() ==
                          AudioSettingsPage::kCpuBudgetDefault &&
                      cpuBudgetPercent->text() == QStringLiteral("72% of the VBlank window"),
                  "Restore Defaults did not reset and persist the CPU budget");
            check(QSettings().value(QStringLiteral("engine/maxPcmChannels")).toInt() ==
                          factory.maxPcmChannels &&
                      QSettings().value(QStringLiteral("engine/pcmMixRate")).toDouble() ==
                          double(factory.pcmMixRate) &&
                      !QSettings().value(QStringLiteral("engine/hqMixer")).toBool(),
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
            cpuBudget->setValue(70000);
            // Leaves polyphony 3 and a 70,000-cycle CPU budget behind for
            // the relaunch check.
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
        auto *cpuBudget = dialog->findChild<QSpinBox *>(QStringLiteral("settingsCpuBudget"));
        check(cpuBudget && cpuBudget->value() == 70000 && window.cpuBudgetCycles() == 70000,
              "relaunch did not load the persisted CPU budget into the Audio page");
        check(dialog->currentPage() == SettingsDialog::Page::Audio,
              "relaunch did not remember the last section");
    }

    // 8. The one migration (docs/cpu-meter/PLAN.md D1 amendment): a legacy
    // percent-of-VBlank budget with no cycles key converts once on load
    // (50 % → 41,888 cycles), the cycles stored and the old key removed; a
    // cycles key present wins and leaves a stray legacy key alone; the old
    // default 100 and an unreadable value are dropped, the 60000 default
    // standing with nothing written.
    {
        QSettings().remove(QStringLiteral("engine/cpuBudgetCycles"));
        QSettings().setValue(QStringLiteral("engine/cpuBudgetPercent"), 50);
        {
            MainWindow window;
            auto *cpuBudget =
                window.settingsDialog()->findChild<QSpinBox *>(QStringLiteral("settingsCpuBudget"));
            auto *cpuBudgetPercent = window.settingsDialog()->findChild<QLabel *>(
                QStringLiteral("settingsCpuBudgetPercent"));
            check(cpuBudget && cpuBudget->value() == 41888 && window.cpuBudgetCycles() == 41888,
                  "a legacy 50% CPU budget did not load as 41888 cycles");
            check(cpuBudgetPercent &&
                      cpuBudgetPercent->text() == QStringLiteral("50% of the VBlank window"),
                  "the migrated CPU budget's caption does not read 50%");
            check(QSettings().value(QStringLiteral("engine/cpuBudgetCycles")).toInt() == 41888 &&
                      !QSettings().contains(QStringLiteral("engine/cpuBudgetPercent")),
                  "the CPU budget migration did not store the cycles and remove the legacy key");
        }
        QSettings().setValue(QStringLiteral("engine/cpuBudgetPercent"), 100);
        check(MainWindow::loadCpuBudgetCycles() == 41888 &&
                  QSettings().value(QStringLiteral("engine/cpuBudgetPercent")).toInt() == 100,
              "a legacy CPU budget key overrode the stored cycles");
        // The old default (100, what its Restore Defaults wrote) is not a
        // choice: the old key goes, nothing is written, 60000 stands.
        QSettings().remove(QStringLiteral("engine/cpuBudgetCycles"));
        QSettings().setValue(QStringLiteral("engine/cpuBudgetPercent"), 100);
        {
            MainWindow window;
            auto *cpuBudget =
                window.settingsDialog()->findChild<QSpinBox *>(QStringLiteral("settingsCpuBudget"));
            check(cpuBudget && cpuBudget->value() == 60000 && window.cpuBudgetCycles() == 60000,
                  "a legacy 100% CPU budget did not leave the 60000-cycle default");
            check(!QSettings().contains(QStringLiteral("engine/cpuBudgetPercent")) &&
                      !QSettings().contains(QStringLiteral("engine/cpuBudgetCycles")),
                  "a legacy 100% CPU budget was not dropped without writing the cycles key");
        }
        // An unreadable legacy value is dropped the same way.
        QSettings().setValue(QStringLiteral("engine/cpuBudgetPercent"), QStringLiteral("lots"));
        check(MainWindow::loadCpuBudgetCycles() == 60000 &&
                  !QSettings().contains(QStringLiteral("engine/cpuBudgetPercent")) &&
                  !QSettings().contains(QStringLiteral("engine/cpuBudgetCycles")),
              "an unreadable legacy CPU budget did not leave the 60000-cycle default");
        QSettings().setValue(QStringLiteral("engine/cpuBudgetCycles"), 5);
        check(MainWindow::loadCpuBudgetCycles() == AudioSettingsPage::kCpuBudgetMin,
              "a stored CPU budget under the minimum was not clamped on load");
    }

    if (failures == 0)
        std::printf("settingscheck: OK\n");
    return failures ? 1 : 0;
}
