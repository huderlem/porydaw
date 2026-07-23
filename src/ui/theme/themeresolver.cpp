#include "themeresolver.h"
#include "color_math.h"
#include "presetcolors.h"
#include <QStringView>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace themes {
namespace {

// These thresholds are Custom-theme policy, not widget paint constants. Keeping
// them here makes every generated interaction and readability rule auditable.
constexpr auto kMinimumTouchableLightness = 0.20;
constexpr auto kMaximumTouchableLightness = 0.80;
constexpr auto kDefaultToHoverLightness = 0.18;
constexpr auto kHoverToActiveLightness = 0.21;
constexpr auto kDefaultToActiveLightness =
    kDefaultToHoverLightness + kHoverToActiveLightness;
constexpr auto kTextContrastThreshold = 4.5;
constexpr auto kChromeTextContrastThreshold = 3.0;
constexpr auto kActiveDeltaLightness = -0.12;
constexpr auto kActiveMinimumRatio = 1.4;
constexpr auto kTrackHeaderSelectionLightness = 0.15;
constexpr auto kExtendStep = 0.02;
constexpr auto kMaximumExtendAttempts = 100;
constexpr auto kLightnessMatchAttempts = 8;
constexpr auto kLightnessBoundAttempts = 32;
constexpr auto kLightnessBoundStep = 0.001;
// One control family receives one ramp so default, hover, and active states
// move together instead of accumulating independent color tweaks.
struct TouchableRamp {
  QColor defaultColor;
  QColor hover;
  QColor active;
  double separation;
};

double clamp(double value, double minimum, double maximum) {
  return std::max(minimum, std::min(maximum, value));
}

QColor mixColors(const QColor &first, const QColor &second,
                 double firstWeight) {
  const auto secondWeight = 1.0 - firstWeight;
  const auto channel = [firstWeight, secondWeight](int firstValue,
                                                   int secondValue) {
    const auto value = firstValue * firstWeight + secondValue * secondWeight;
    return static_cast<int>(std::floor(value + 0.5));
  };
  return QColor::fromRgb(channel(first.red(), second.red()),
                         channel(first.green(), second.green()),
                         channel(first.blue(), second.blue()));
}

QColor colorFromHex(preset_colors::HexColor hex) {
  return QColor(QLatin1String(hex.data(), static_cast<int>(hex.size())));
}

QColor blackOrWhiteByWorstContrast(const std::initializer_list<QColor> &fills) {
  const auto black = QColor::fromRgb(0, 0, 0);
  const auto white = QColor::fromRgb(255, 255, 255);
  auto blackScore = std::numeric_limits<double>::max();
  auto whiteScore = std::numeric_limits<double>::max();
  for (const auto &fill : fills) {
    blackScore = std::min(blackScore, contrastRatio(black, fill));
    whiteScore = std::min(whiteScore, contrastRatio(white, fill));
  }
  return whiteScore > blackScore ? white : black;
}

QColor lightenToContrast(const QColor &foreground, const QColor &background,
                         double minimumContrast) {
  const auto channel = [](int foregroundChannel, int amount) {
    return (255 * amount + foregroundChannel * (255 - amount) + 127) / 255;
  };
  // Scan one 8-bit blend step at a time so Custom uses the smallest visible
  // adjustment that reaches its contrast requirement.
  for (auto amount = 0; amount <= 255; ++amount) {
    const auto candidate = QColor::fromRgb(channel(foreground.red(), amount),
                                           channel(foreground.green(), amount),
                                           channel(foreground.blue(), amount));
    if (contrastRatio(candidate, background) >= minimumContrast)
      return candidate;
  }
  return QColor::fromRgb(255, 255, 255);
}

// Keep Accent as selected text when readable; otherwise use the safer neutral.
QColor selectedTextColor(const QColor &accent,
                         const std::initializer_list<QColor> &fills) {
  auto accentReadable = true;
  for (const auto &fill : fills) {
    if (contrastRatio(accent, fill) < kTextContrastThreshold) {
      accentReadable = false;
      break;
    }
  }
  return accentReadable ? accent : blackOrWhiteByWorstContrast(fills);
}

QColor touchableBackground(const QColor &color);

// Gamut clipping can move a requested OKLab lightness. Iterate against the
// emitted QColor so the bounded result matches the policy rather than the
// input.
QColor colorAtLightness(double targetLightness, double a, double b) {
  auto inputLightness = targetLightness;
  auto color = colorFromOklab({inputLightness, a, b});
  for (auto attempt = 0; attempt < kLightnessMatchAttempts; ++attempt) {
    const auto actualLightness = oklabLightness(color);
    inputLightness =
        clamp(inputLightness + targetLightness - actualLightness, 0.0, 1.0);
    color = colorFromOklab({inputLightness, a, b});
  }
  for (auto attempt = 0; attempt < kLightnessBoundAttempts; ++attempt) {
    const auto actualLightness = oklabLightness(color);
    if (actualLightness >= kMinimumTouchableLightness &&
        actualLightness <= kMaximumTouchableLightness)
      break;
    inputLightness += actualLightness < kMinimumTouchableLightness
                          ? kLightnessBoundStep
                          : -kLightnessBoundStep;
    color = colorFromOklab({inputLightness, a, b});
  }
  return color;
}

// Clamp only interactive surfaces; very dark or light themes keep their overall
// character while controls retain enough room for visible state changes.
QColor touchableBackground(const QColor &color) {
  const auto lab = oklabFromColor(color);
  if (lab.lightness >= kMinimumTouchableLightness &&
      lab.lightness <= kMaximumTouchableLightness)
    return color;
  return colorAtLightness(clamp(lab.lightness, kMinimumTouchableLightness,
                                kMaximumTouchableLightness),
                          lab.a, lab.b);
}

TouchableRamp rampInDirection(const QColor &defaultColor, double multiplier) {
  const auto lab = oklabFromColor(defaultColor);
  const auto stateColor = [lab, multiplier](double distance) {
    return colorAtLightness(clamp(lab.lightness + multiplier * distance,
                                  kMinimumTouchableLightness,
                                  kMaximumTouchableLightness),
                            lab.a, lab.b);
  };
  const auto hover = stateColor(kDefaultToHoverLightness);
  const auto active = stateColor(kDefaultToActiveLightness);
  return {defaultColor, hover, active,
          std::abs(oklabLightness(active) - lab.lightness)};
}

// Move toward whichever lightness bound has more room, avoiding collapsed
// hover and active states near black or white.
TouchableRamp touchableRamp(const QColor &color) {
  const auto defaultColor = touchableBackground(color);
  const auto darker = rampInDirection(defaultColor, -1.0);
  const auto lighter = rampInDirection(defaultColor, 1.0);
  return lighter.separation > darker.separation ? lighter : darker;
}

// An active-state shift must be distinguishable by contrast, not merely
// numerically different; extend in either direction until it reaches that
// threshold.
QColor activeColor(const QColor &color) {
  const auto lab = oklabFromColor(color);
  for (const auto direction : {1.0, -1.0}) {
    auto target = lab.lightness + kActiveDeltaLightness * direction;
    const auto step =
        kActiveDeltaLightness * direction > 0.0 ? kExtendStep : -kExtendStep;
    for (auto attempt = 0; attempt < kMaximumExtendAttempts; ++attempt) {
      const auto candidate = colorFromOklab({target, lab.a, lab.b});
      if (contrastRatio(candidate, color) >= kActiveMinimumRatio)
        return candidate;
      if (target <= 0.0 || target >= 1.0)
        break;
      target += step;
    }
  }
  return colorFromOklab({lab.lightness + kActiveDeltaLightness, lab.a, lab.b});
}

// Pull a derived surface back toward its safe fallback before sacrificing text
// readability.
QColor keepSurfaceReadable(const QColor &candidate, const QColor &fallback,
                           const QColor &text) {
  for (auto step = 20; step >= 0; --step) {
    const auto candidateWeight = step * 0.05;
    const auto color = mixColors(candidate, fallback, candidateWeight);
    if (contrastRatio(text, color) >= kTextContrastThreshold)
      return color;
  }
  return fallback;
}

// Chrome must keep the same text readable both at rest and after its active
// state shift.
QColor keepChromeReadable(const QColor &candidate, const QColor &neutralBase,
                          const QColor &text) {
  const auto contrastEndpoint =
      text.red() == 0 && text.green() == 0 && text.blue() == 0
          ? QColor::fromRgb(255, 255, 255)
          : QColor::fromRgb(0, 0, 0);
  for (auto step = 0; step <= 20; ++step) {
    const auto endpointWeight = step * 0.05;
    const auto color = mixColors(contrastEndpoint, candidate, endpointWeight);
    if (contrastRatio(text, color) >= kTextContrastThreshold &&
        contrastRatio(text, activeColor(color)) >= kTextContrastThreshold)
      return color;
  }
  return neutralBase;
}

// Disabled text is shared by many Qt controls, so validate it against every
// surface where the platform style may place it.
QColor deriveDisabledText(const QColor &globalText,
                          const QColor &windowBackground,
                          const std::initializer_list<QColor> &fills) {
  for (auto step = 19; step >= 0; --step) {
    const auto textWeight = step * 0.05;
    const auto candidate = mixColors(globalText, windowBackground, textWeight);
    auto readable = true;
    for (const auto &fill : fills) {
      if (contrastRatio(candidate, fill) < kTextContrastThreshold)
        readable = false;
    }
    if (readable)
      return candidate;
  }
  return blackOrWhiteByWorstContrast(fills);
}

// Prefer theme colors before black or white. sharedFills keeps reused text
// roles readable on every component that shares them, not just their primary
// fill.
QColor stateTextColor(const QColor &preferred, const QColor &accent,
                      const std::initializer_list<QColor> &fills,
                      const std::initializer_list<QColor> &sharedFills) {
  const std::array<QColor, 4> candidates{preferred, accent,
                                         QColor::fromRgb(0, 0, 0),
                                         QColor::fromRgb(255, 255, 255)};
  for (const auto &candidate : candidates) {
    auto readable = true;
    for (const auto &fill : sharedFills) {
      if (contrastRatio(candidate, fill) < kTextContrastThreshold) {
        readable = false;
        break;
      }
    }
    if (readable)
      return candidate;
  }
  for (const auto &candidate : candidates) {
    auto readable = true;
    for (const auto &fill : fills) {
      if (contrastRatio(candidate, fill) < kTextContrastThreshold) {
        readable = false;
        break;
      }
    }
    if (readable)
      return candidate;
  }
  return blackOrWhiteByWorstContrast(fills);
}

} // namespace

bool isValidColorPair(const QColor &primary, const QColor &accent) {
  return primary.isValid() && accent.isValid() && primary.alpha() == 255 &&
         accent.alpha() == 255 && contrastRatio(primary, accent) >= 3.0;
}

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

// Custom themes derive a small set of design colors, then assign each public
// role beside its source so a human can trace a UI element back to its policy.
Theme derive(const QColor &primary, const QColor &accent) {
  Theme theme;

  // Neutral surfaces use a 95/5 channel mix, so Accent gently tints the UI.
  const auto neutralBase = mixColors(primary, accent, 0.95);
  const auto windowText = blackOrWhiteByWorstContrast({neutralBase});
  const auto direction = oklabLightness(neutralBase) < 0.55 ? 1.0 : -1.0;
  const auto surface = [&neutralBase, direction](double distance) {
    return shiftOklabLightness(neutralBase, direction * distance);
  };

  const auto windowBackground = neutralBase;
  theme.color(Role::window_background) = windowBackground;
  theme.color(Role::window_text) = windowText;
  theme.color(Role::link_text) = accent;
  theme.color(Role::tab_pane_background) = windowBackground;
  theme.color(Role::scrollbar_page_background) = windowBackground;
  theme.color(Role::splitter_background) = windowBackground;

  // Chrome shares one quiet surface; text is lightened only as far as 3:1
  // requires so the requested Window Text color remains recognizable.
  const auto chromeBackground =
      keepChromeReadable(surface(0.04), neutralBase, windowText);
  const auto chromeText = lightenToContrast(windowText, chromeBackground,
                                            kChromeTextContrastThreshold);
  theme.color(Role::toolbar_background) = chromeBackground;
  theme.color(Role::toolbar_text) = chromeText;
  theme.color(Role::transport_text) = chromeText;
  theme.color(Role::menu_bar_background) = chromeBackground;
  theme.color(Role::menu_bar_text) = chromeText;
  theme.color(Role::header_background) = chromeBackground;
  theme.color(Role::header_text) = chromeText;
  theme.color(Role::scrollbar_background) = chromeBackground;

  // Buttons, tabs, combo lanes, spin buttons, and headers intentionally share
  // one interaction ramp so equivalent states look equivalent.
  const auto buttonRamp = touchableRamp(touchableBackground(surface(0.09)));
  const auto buttonBackground = buttonRamp.defaultColor;
  const auto buttonHoverBackground = buttonRamp.hover;
  const auto buttonActiveBackground = buttonRamp.active;
  theme.color(Role::button_background) = buttonBackground;
  theme.color(Role::button_hover_background) = buttonHoverBackground;
  theme.color(Role::button_pressed_background) = buttonActiveBackground;
  theme.color(Role::tab_background) = buttonBackground;
  theme.color(Role::tab_hover_background) = buttonHoverBackground;
  theme.color(Role::combo_background) = buttonHoverBackground;
  theme.color(Role::combo_drop_down_background) = buttonBackground;
  theme.color(Role::combo_drop_down_hover_background) = buttonRamp.hover;
  theme.color(Role::combo_drop_down_pressed_background) = buttonRamp.active;
  theme.color(Role::spin_box_background) = buttonBackground;
  theme.color(Role::spin_button_background) = buttonBackground;
  theme.color(Role::spin_button_hover_background) = buttonRamp.hover;
  theme.color(Role::spin_button_pressed_background) = buttonRamp.active;
  theme.color(Role::header_hover_background) = buttonHoverBackground;
  theme.color(Role::header_checked_background) = buttonActiveBackground;

  // Recess editable and popup surfaces opposite the main surface direction,
  // falling back toward neutral if Window Text would become unreadable.
  const auto inputBackground =
      keepSurfaceReadable(shiftOklabLightness(neutralBase, direction * -0.04),
                          neutralBase, windowText);
  theme.color(Role::input_background) = inputBackground;
  theme.color(Role::input_text) = windowText;
  theme.color(Role::input_highlight_outline) = buttonHoverBackground;
  theme.color(Role::focus_outline) = accent;
  theme.color(Role::indicator_text) = windowText;
  theme.color(Role::indicator_background) = inputBackground;
  theme.color(Role::indicator_hover_background) = buttonHoverBackground;
  theme.color(Role::menu_background) = inputBackground;
  theme.color(Role::menu_text) = windowText;
  theme.color(Role::tooltip_background) = inputBackground;
  theme.color(Role::tooltip_text) = windowText;

  // Lists, track headers, and polyphony values share the item surface; their
  // separate roles keep that relationship explicit and easy to change locally.
  const auto itemAlternateBackground = windowBackground;
  theme.color(Role::item_alternate_background) = itemAlternateBackground;
  theme.color(Role::indicator_disabled_background) = itemAlternateBackground;
  const auto viewRamp = touchableRamp(
      touchableBackground(shiftOklabLightness(neutralBase, direction * -0.02)));
  const auto viewBackground = viewRamp.defaultColor;
  const auto itemBackground = oklabLightness(primary) < 0.55
                                  ? shiftOklabLightness(primary, 0.12)
                                  : primary;
  theme.color(Role::item_background) = itemBackground;
  theme.color(Role::track_header_panel_background) = itemBackground;
  theme.color(Role::polyphony_value_background) = itemBackground;
  const auto viewHoverBackground = viewRamp.hover;
  theme.color(Role::item_hover_background) = viewHoverBackground;
  const auto viewSelectedBackground = viewRamp.active;
  theme.color(Role::item_selected_background) = viewSelectedBackground;
  theme.color(Role::menu_item_hover_background) = viewSelectedBackground;

  // Outline roles stay component-specific even while they share this swatch.
  // A later border change can therefore affect one widget family only.
  const auto outline = surface(0.16);
  theme.color(Role::palette_outline) = outline;
  theme.color(Role::track_header_panel_outline) = outline;
  theme.color(Role::toolbar_outline) = outline;
  theme.color(Role::tab_outline) = outline;
  theme.color(Role::button_outline) = outline;
  theme.color(Role::input_outline) = outline;
  theme.color(Role::spin_box_outline) = outline;
  theme.color(Role::combo_outline) = outline;
  theme.color(Role::indicator_outline) = outline;
  theme.color(Role::menu_bar_outline) = outline;
  theme.color(Role::menu_outline) = outline;
  theme.color(Role::tooltip_outline) = outline;
  theme.color(Role::group_box_outline) = outline;
  theme.color(Role::item_outline) = outline;
  theme.color(Role::header_outline) = outline;
  theme.color(Role::scrollbar_outline) = outline;
  theme.color(Role::splitter_outline) = outline;
  theme.color(Role::toolbar_separator) = outline;
  theme.color(Role::menu_separator) = outline;
  theme.color(Role::splitter_handle) = outline;
  theme.color(Role::splitter_handle_hover_background) = accent;
  const auto gridLines = surface(0.12);

  // Accent becomes a selection fill only after entering the touchable range;
  // its paired text is then chosen against the actual resulting fill.
  const auto selectionBackground = touchableBackground(accent);
  theme.color(Role::selection_background) = selectionBackground;
  const auto selectionText = selectedTextColor(accent, {selectionBackground});
  theme.color(Role::selection_text) = selectionText;

  const auto scrollbarRamp = touchableRamp(touchableBackground(surface(0.20)));
  theme.color(Role::scrollbar_handle) = scrollbarRamp.defaultColor;
  theme.color(Role::scrollbar_handle_hover_background) = scrollbarRamp.hover;

  const auto tabSelectedBackground = touchableBackground(surface(0.14));
  theme.color(Role::tab_selected_background) = tabSelectedBackground;
  const auto tabSelectedText =
      selectedTextColor(accent, {tabSelectedBackground});
  theme.color(Role::tab_selected_text) = tabSelectedText;

  theme.color(Role::indicator_checked_background) = accent;
  theme.color(Role::indicator_checked_outline) = accent;
  theme.color(Role::indicator_check_mark) = blackOrWhiteByWorstContrast({accent});

  // Track selection follows the item surface, while Mute and Solo retain their
  // domain colors; each state gets text chosen against its own fill.
  const auto trackSelectionDirection =
      oklabLightness(itemBackground) > 0.55 ? 1.0 : -1.0;
  const auto trackSelected = shiftOklabLightness(
      itemBackground, trackSelectionDirection * kTrackHeaderSelectionLightness);
  const auto trackSelectedText = selectedTextColor(accent, {trackSelected});
  const auto trackMuteChecked =
      touchableBackground(shiftOklabLightness("FFAD56", -0.12));
  theme.color(Role::track_mute_checked_background) = trackMuteChecked;
  const auto trackMuteCheckedText =
      selectedTextColor(accent, {trackMuteChecked});
  theme.color(Role::track_mute_checked_text) = trackMuteCheckedText;
  const auto trackSoloChecked =
      touchableBackground(shiftOklabLightness("3B6AB7", 0.12));
  theme.color(Role::track_solo_checked_background) = trackSoloChecked;
  const auto trackSoloCheckedText =
      selectedTextColor(accent, {trackSoloChecked});
  theme.color(Role::track_solo_checked_text) = trackSoloCheckedText;

  // Text roles follow the same sharing boundaries as their background ramps.
  // A shared foreground must remain readable on every surface that consumes it.
  const auto buttonText =
      stateTextColor(windowText, accent, {buttonBackground},
                     {buttonBackground, windowBackground, itemBackground});
  theme.color(Role::button_text) = buttonText;
  theme.color(Role::tab_text) = buttonText;
  theme.color(Role::spin_box_text) = buttonText;
  theme.color(Role::spin_button_text) = buttonText;
  const auto buttonHoverText = stateTextColor(
      windowText, accent, {buttonHoverBackground},
      {buttonBackground, buttonHoverBackground, buttonActiveBackground});
  theme.color(Role::button_hover_text) = buttonHoverText;
  theme.color(Role::tab_hover_text) = buttonHoverText;
  theme.color(Role::combo_text) = buttonHoverText;
  theme.color(Role::header_hover_text) = buttonHoverText;
  const auto buttonActiveText =
      selectedTextColor(accent, {buttonActiveBackground});
  theme.color(Role::button_pressed_text) = buttonActiveText;
  theme.color(Role::menu_item_pressed_background) = buttonActiveBackground;
  theme.color(Role::menu_item_pressed_text) = buttonActiveText;
  theme.color(Role::header_checked_text) = buttonActiveText;

  // Item text is resolved separately from control text because lists and track
  // headers use a different set of backgrounds.
  const auto itemText =
      stateTextColor(windowText, accent, {itemBackground}, {itemBackground});
  theme.color(Role::item_text) = itemText;
  theme.color(Role::track_header_panel_text) = itemText;
  theme.color(Role::polyphony_value_text) = itemText;
  const auto viewHoverText = stateTextColor(
      windowText, accent, {viewHoverBackground},
      {viewBackground, viewHoverBackground, viewSelectedBackground});
  theme.color(Role::item_hover_text) = viewHoverText;
  const auto viewSelectedText =
      selectedTextColor(accent, {viewSelectedBackground});
  theme.color(Role::item_selected_text) = viewSelectedText;
  theme.color(Role::menu_item_hover_text) = viewSelectedText;

  const auto disabledText = deriveDisabledText(
      windowText, windowBackground,
      {windowBackground, buttonBackground, inputBackground, itemBackground});
  theme.color(Role::disabled_text) = disabledText;
  theme.color(Role::secondary_text) = disabledText;

  // SongView roles are direct assignments rather than an override bundle. Each
  // paint element is visible here beside the color policy that supplies it.
  theme.color(Role::song_view_timeline_chrome_background) = chromeBackground;
  theme.color(Role::song_view_separator) = outline;
  theme.color(Role::song_view_primary_text) = windowText;
  theme.color(Role::song_view_secondary_text) = disabledText;
  theme.color(Role::song_view_selection_fill) = selectionBackground;
  theme.color(Role::song_view_selection_edge) = selectionBackground;
  theme.color(Role::song_view_selected_note_inner_border) = selectionText;
  theme.color(Role::song_view_loop_marker) = selectionBackground;
  theme.color(Role::song_view_edit_cursor) = windowText;
  theme.color(Role::song_view_edit_preview_outline) = windowText;
  theme.color(Role::song_view_unterminated_note_outline) = accent;
  theme.color(Role::song_view_grid) = gridLines;
  theme.color(Role::song_view_piano_roll_background) = windowBackground;
  theme.color(Role::song_view_piano_roll_accidental_lane) =
      shiftOklabLightness(windowBackground, -0.05);
  // The playhead keeps its identity red in every theme, like the Mute and
  // Solo domain colors; its glow keeps it visible on any surface.
  theme.color(Role::song_view_playhead) = QColor::fromRgb(226, 66, 66);
  theme.color(Role::song_view_piano_keyboard_natural_key) =
      QColor::fromRgb(0xE8, 0xE8, 0xE8);
  theme.color(Role::song_view_piano_keyboard_black_key) =
      QColor::fromRgb(0x20, 0x22, 0x24);
  theme.color(Role::song_view_piano_keyboard_active_key) = selectionBackground;
  theme.color(Role::song_view_piano_keyboard_separator) =
      QColor::fromRgb(0x9A, 0x9A, 0x9A);
  theme.color(Role::song_view_piano_keyboard_label) =
      QColor::fromRgb(0x1A, 0x1A, 0x1A);
  theme.color(Role::song_view_track_header_selection) = trackSelected;
  theme.color(Role::song_view_track_header_selection_text) = trackSelectedText;
  theme.color(Role::song_view_automation_default_curve) = selectionBackground;
  theme.color(Role::song_view_automation_tempo_curve) = accent;
  theme.color(Role::song_view_file_event_marker) = outline;
  theme.color(Role::song_view_add_automation_lane_action) = accent;

  return theme;
}

Theme withGridLineContrast(Theme theme, int contrast) {
  Q_ASSERT(contrast >= 0 && contrast <= 100);
  if (contrast == 50)
    return theme;
  const auto grid = theme.color(Role::song_view_grid);
  const auto background = theme.color(Role::song_view_piano_roll_background);
  if (contrast < 50) {
    theme.color(Role::song_view_grid) =
        mixColors(grid, background, static_cast<double>(contrast) / 50.0);
    return theme;
  }
  const auto endpoint = relativeLuminance(grid) <= relativeLuminance(background)
                            ? QColor::fromRgb(0, 0, 0)
                            : QColor::fromRgb(255, 255, 255);
  const auto endpointWeight = static_cast<double>(contrast - 50) / 50.0;
  theme.color(Role::song_view_grid) = mixColors(endpoint, grid, endpointWeight);
  return theme;
}

Theme vanilla() { return resolvePreset(preset_colors::vanilla); }

Theme darkNeutralHigh() {
  return resolveDarkPreset(preset_colors::darkNeutralHigh);
}

Theme immaterial() { return resolveDarkPreset(preset_colors::immaterial); }

} // namespace themes
