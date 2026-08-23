#pragma once

#include <QColor>

#include <optional>

class QApplication;
class QSettings;

namespace themes {

struct Theme;

enum class ThemeMode { Vanilla, DarkNeutralHigh, Immaterial, Custom };

struct ColorPair {
    QColor primary;
    QColor accent;
};

inline constexpr auto defaultGridLineContrast = 50;

struct ThemeSelection {
    ThemeMode mode = ThemeMode::Vanilla;
    std::optional<ColorPair> customColors;
    int gridLineContrast = defaultGridLineContrast;
};

/// Owns the application's current theme.
///
/// The Appearance page sends the active mode and any valid Custom colors
/// here as they are chosen. It does not apply colors or write settings
/// itself, and nothing is previewed: the committed theme is the only theme
/// ever applied (a repolish while playback paints is unsafe on Windows).
class ThemeController
{
  public:
    ThemeController(QApplication &application, QSettings &settings);

    void restore();
    bool commit(const ThemeSelection &candidate);
    const ThemeSelection &committedSelection() const;

  private:
    void writeStoredSelection(const ThemeSelection &selection);
    Theme resolve(const ThemeSelection &selection) const;
    ThemeSelection readStoredSelection() const;

    QApplication &m_application;
    QSettings &m_settings;
    ThemeSelection m_selection;
};

} // namespace themes
