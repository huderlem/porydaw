#pragma once

#include <QWidget>

class QCheckBox;

namespace themes {
class ThemeController;
class ThemePage;
} // namespace themes

// Settings window, Appearance page: the theme editor and the typeface
// choice. Like every Settings page it applies as it changes and only
// reports; the main window lands the font swap and persists the choice.
class AppearanceSettingsPage : public QWidget
{
    Q_OBJECT

  public:
    AppearanceSettingsPage(themes::ThemeController &themes, bool useSystemFont,
                           QWidget *parent = nullptr);

    bool useSystemFont() const;

    // Drops any half-typed Custom theme draft. Called when the Settings
    // window closes.
    void rollback();

  signals:
    void useSystemFontChanged(bool on);

  private:
    themes::ThemePage *m_theme = nullptr;
    QCheckBox *m_systemFont = nullptr;
};
