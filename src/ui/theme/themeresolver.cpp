#include "themeresolver.h"
#include "color_math.h"
#include "presetcolors.h"
#include <QStringView>

#include <array>


namespace themes {
namespace {

constexpr auto kChromeTextContrastThreshold = 3.0;
constexpr auto kPlayheadLightnessThreshold = 0.50;

QColor colorFromHex(preset_colors::HexColor hex) {
  return QColor::fromString(
      QLatin1StringView(hex.data(), static_cast<qsizetype>(hex.size())));
}


QColor playheadColor(const QColor &pianoRollBackground) {
  if (oklabLightness(pianoRollBackground) < kPlayheadLightnessThreshold)
    return QColor::fromRgb(255, 255, 255);
  return QColor::fromRgb(0, 0, 0);
}

} // namespace

namespace {

Theme resolvePreset(const preset_colors::PresetColors &colors) {
  Theme theme;
  for (std::size_t index = 0; index < roleCount; ++index) {
    const auto role = static_cast<Role>(index);
    theme.colors[index] = colorFromHex(colors.color(role));
  }
  return theme;
}

Theme resolveDarkPreset(const preset_colors::PresetColors &colors) {
  auto theme = resolvePreset(colors);
  // The pale foreground is unreadable on the light active fills. Reuse the
  // dark resting-button surface as their foreground.
  const auto activeText = theme.color(Role::button_background);
  constexpr auto activeTextRoles = std::array{
      Role::selection_text,
      Role::tab_selected_text,
      Role::button_pressed_text,
      Role::menu_item_hover_text,
      Role::menu_item_pressed_text,
      Role::item_selected_text,
      Role::header_checked_text,
      Role::track_mute_checked_text,
      Role::song_view_track_header_selection_text,
  };
  for (const auto role : activeTextRoles)
    theme.color(role) = activeText;
  return theme;
}

} // namespace



Theme vanilla() { return resolvePreset(preset_colors::vanilla); }

Theme darkNeutralMedium() {
  return resolveDarkPreset(preset_colors::darkNeutralMedium);
}

Theme darkNeutralHigh() {
  return resolveDarkPreset(preset_colors::darkNeutralHigh);
}

Theme immaterial() {
  return resolveDarkPreset(preset_colors::immaterial);
}


} // namespace themes
