#pragma once

#include "enginesettings.h"

#include <QDialog>

class AppearanceSettingsPage;
class AudioSettingsPage;
class KeyboardShortcutsPage;
class QListWidget;
class QStackedWidget;

namespace themes {
class ThemeController;
}

// Edit → Settings…: every app-wide preference in one modeless window, a
// section list on the left and the selected section's page on the right.
// Nothing here is staged — each page applies its controls as they change
// and reports through the signals below; the main window owns landing and
// persisting them. The window remembers which section was open last.
class SettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    enum class Page { Audio, Appearance, Shortcuts };

    SettingsDialog(themes::ThemeController &themes, int outputLevel, const EngineSettings &engine,
                   bool useSystemFont, QWidget *parent = nullptr);

    // Shows (or raises) the window on the remembered section.
    void present();
    void showPage(Page page);
    Page currentPage() const;

    AudioSettingsPage *audioPage() const { return m_audio; }
    AppearanceSettingsPage *appearancePage() const { return m_appearance; }
    KeyboardShortcutsPage *shortcutsPage() const { return m_shortcuts; }

    // Esc, the Close button and the title-bar X (QDialog::closeEvent routes
    // there too); public like QDialog's own.
    void reject() override;

  signals:
    void outputLevelChanged(int percent);
    void engineSettingsChanged(const EngineSettings &settings);
    void useSystemFontChanged(bool on);

  private:
    void addPage(Page page, const QString &title, QWidget *widget);
    void pageChanged(int index);
    void leave();

    QListWidget *m_sections = nullptr;
    QStackedWidget *m_stack = nullptr;
    AudioSettingsPage *m_audio = nullptr;
    AppearanceSettingsPage *m_appearance = nullptr;
    KeyboardShortcutsPage *m_shortcuts = nullptr;
    // Off until construction has restored the remembered section, so
    // merely building the window never writes QSettings.
    bool m_remember = false;
};
