#include "themecontroller.h"

#include "theme.h"
#include "themeresolver.h"
#include "themeruntime.h"

#include <QApplication>
#include <QSettings>
#include <QtGlobal>
#include <algorithm>

namespace themes {
namespace {

const auto modeKey = QStringLiteral("theme/mode");
const auto primaryKey = QStringLiteral("theme/primary");
const auto accentKey = QStringLiteral("theme/accent");
const auto gridLineContrastKey = QStringLiteral("theme/grid-line-contrast");

bool isValid(const ThemeSelection &selection) {
  if (selection.customColors &&
      !isValidColorPair(selection.customColors->primary,
                        selection.customColors->accent))
    return false;
  return selection.gridLineContrast >= 0 && selection.gridLineContrast <= 100 &&
         (selection.mode != ThemeMode::Custom || selection.customColors);
}

QString canonicalColor(const QColor &color) {
  return color.name(QColor::HexRgb).toUpper();
}

} // namespace

ThemeController::ThemeController(QApplication &application, QSettings &settings)
    : m_application(application), m_settings(settings) {}

void ThemeController::restore() {
  // Writing the value we just read is intentional startup repair: it removes
  // stale keys and canonicalizes malformed, partial, or lowercase settings.
  m_selection = readStoredSelection();
  writeStoredSelection(m_selection);
  apply(m_application, resolve(m_selection));
}

void ThemeController::preview(const ThemeSelection &candidate) {
  if (isValid(candidate))
    apply(m_application, resolve(candidate));
}

bool ThemeController::commit(const ThemeSelection &candidate) {
  auto selection = candidate;
  // Presets do not use custom colors, but retain the last pair so returning to
  // Custom restores the user's work instead of the preset replacing it.
  if (selection.mode != ThemeMode::Custom && !selection.customColors)
    selection.customColors = m_selection.customColors;
  if (!isValid(selection))
    return false;
  writeStoredSelection(selection);
  apply(m_application, resolve(selection));
  m_selection = selection;
  return true;
}

void ThemeController::discardPreview() {
  apply(m_application, resolve(m_selection));
}

const ThemeSelection &ThemeController::committedSelection() const {
  return m_selection;
}

Theme ThemeController::resolve(const ThemeSelection &selection) const {
  switch (selection.mode) {
  case ThemeMode::Vanilla:
    return withGridLineContrast(vanilla(), selection.gridLineContrast);
  case ThemeMode::DarkNeutralHigh:
    return withGridLineContrast(darkNeutralHigh(), selection.gridLineContrast);
  case ThemeMode::Immaterial:
    return withGridLineContrast(immaterial(), selection.gridLineContrast);
  case ThemeMode::Custom:
    return withGridLineContrast(
        derive(selection.customColors->primary, selection.customColors->accent),
        selection.gridLineContrast);
  }
  Q_UNREACHABLE();
}

ThemeSelection ThemeController::readStoredSelection() const {
  const auto hasPrimary = m_settings.contains(primaryKey);
  const auto hasAccent = m_settings.contains(accentKey);
  if (hasPrimary != hasAccent)
    return {};
  auto customColors = std::optional<ColorPair>{};
  if (hasPrimary) {
    const auto colors =
        ColorPair{QColor(m_settings.value(primaryKey).toString()),
                  QColor(m_settings.value(accentKey).toString())};
    if (!isValidColorPair(colors.primary, colors.accent))
      return {};
    customColors = colors;
  }
  auto contrastValid = false;
  const auto storedContrast =
      m_settings.value(gridLineContrastKey, defaultGridLineContrast)
          .toInt(&contrastValid);
  const auto gridLineContrast = contrastValid
                                    ? std::clamp(storedContrast, 0, 100)
                                    : defaultGridLineContrast;
  const auto mode = m_settings.value(modeKey).toString();
  if (mode == QStringLiteral("vanilla"))
    return ThemeSelection{ThemeMode::Vanilla, customColors, gridLineContrast};
  if (mode == QStringLiteral("dark-neutral-high"))
    return ThemeSelection{ThemeMode::DarkNeutralHigh, customColors,
                          gridLineContrast};
  if (mode == QStringLiteral("immaterial"))
    return ThemeSelection{ThemeMode::Immaterial, customColors,
                          gridLineContrast};
  if (mode == QStringLiteral("custom") && customColors)
    return ThemeSelection{ThemeMode::Custom, customColors, gridLineContrast};
  return ThemeSelection{ThemeMode::Vanilla, customColors, gridLineContrast};
}

void ThemeController::writeStoredSelection(const ThemeSelection &selection) {
  if (selection.customColors) {
    m_settings.setValue(primaryKey,
                        canonicalColor(selection.customColors->primary));
    m_settings.setValue(accentKey,
                        canonicalColor(selection.customColors->accent));
  } else {
    m_settings.remove(primaryKey);
    m_settings.remove(accentKey);
  }
  m_settings.setValue(gridLineContrastKey, selection.gridLineContrast);
  switch (selection.mode) {
  case ThemeMode::Vanilla:
    m_settings.setValue(modeKey, QStringLiteral("vanilla"));
    return;
  case ThemeMode::DarkNeutralHigh:
    m_settings.setValue(modeKey, QStringLiteral("dark-neutral-high"));
    return;
  case ThemeMode::Immaterial:
    m_settings.setValue(modeKey, QStringLiteral("immaterial"));
    return;
  case ThemeMode::Custom:
    m_settings.setValue(modeKey, QStringLiteral("custom"));
    return;
  }
  Q_UNREACHABLE();
}

} // namespace themes
