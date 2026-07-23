#pragma once

#include "theme_roles.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace themes::preset_colors {

/// Compile-time hexadecimal sRGB color string, including the leading '#'.
using HexColor = std::string_view;

/// Fixed-theme color names. Each entry selects one authored color used by one
/// or more visible UI surfaces.
enum class PresetColor {
  /// Main window, scrollbar page, and splitter backgrounds.
  window_background,
  /// Text for labels, controls, tabs, menus, headers, items, selections, and
  /// SongView.
  window_text,
  /// Text shown by disabled widgets.
  disabled_text,
  /// Widget borders, item outlines, separators, hovered scrollbar thumbs, and
  /// SongView event markers.
  outline,
  /// Selected tabs/rows, hovered combo/spin arrow lanes, and SongView
  /// selection, loop, and active-key fills.
  selection_background,
  /// Links, checked indicators, active arrows, and SongView accents.
  accent,
  /// Toolbar, menu-bar, header, scrollbar-track, and timeline-ruler
  /// backgrounds.
  chrome_background,
  /// Toolbar/menu separators, splitter handles, and SongView separator lines.
  separator,
  /// Resting buttons/tabs and combo-box/spin-box field or arrow-lane surfaces.
  control_background,
  /// Hovered buttons/tabs/combo fields, indicators, headers, and input
  /// outlines.
  control_hover_background,
  /// Pressed buttons, menus, headers, checked Mute/Solo controls, and
  /// unterminated-note outlines.
  control_pressed_background,
  /// Text fields, popup menus, tooltips, and checkbox/radio indicator surfaces.
  input_background,
  /// Alternating item rows and disabled checkbox/radio indicator surfaces.
  alternate_background,
  /// List/tree rows, track-header panels, polyphony readouts, and track
  /// selection overlays.
  item_background,
  /// Secondary item text and SongView track-header/automation details.
  secondary_text,
  /// Hovered list/tree rows and item-view surfaces.
  item_hover_background,
  /// Scrollbar thumbs and SongView grid lines.
  scrollbar_handle,
  /// SongView piano-roll note area background; its lightness selects the
  /// authored accidental-lane color.
  piano_roll_background,
  /// SongView piano-roll accidental-note lane background.
  piano_roll_accidental_lane,
  /// SongView playhead: a fixed identity red shared by every theme, like
  /// the Mute and Solo domain colors (rollcheck pixel-scans for it).
  playhead,
  /// Natural (white) keys on the SongView piano keyboard.
  piano_natural_key,
  /// Black (accidental) keys on the SongView piano keyboard.
  piano_black_key,
  /// Octave separator lines on the SongView piano keyboard.
  piano_keyboard_separator,
  /// Note labels on the SongView piano keyboard.
  piano_keyboard_label,
  /// Sentinel for the number of authored preset colors; not rendered.
  count,
};

inline constexpr auto presetColorCount =
    static_cast<std::size_t>(PresetColor::count);
static_assert(presetColorCount == 24);

constexpr PresetColor presetColorFor(Role role);

/// Fixed-theme colors are authored as this compact palette. The role map below
/// adapts the palette to the complete rendering interface.
struct PresetColors {
  std::array<HexColor, presetColorCount> values{};

  constexpr HexColor &color(PresetColor presetColor) {
    return values.at(static_cast<std::size_t>(presetColor));
  }

  constexpr HexColor color(PresetColor presetColor) const {
    return values.at(static_cast<std::size_t>(presetColor));
  }

  constexpr HexColor color(Role role) const {
    return color(presetColorFor(role));
  }
};

// Keep this in Role order. The size assertion makes additions to Role update
// this internal adapter, while fixed-theme authors only edit PresetColor
// values.

inline constexpr auto rolePresetColors = std::array{
    PresetColor::window_background,
    PresetColor::window_text,
    PresetColor::disabled_text,
    PresetColor::outline,
    PresetColor::selection_background,
    PresetColor::window_text,
    PresetColor::accent,

    PresetColor::chrome_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::window_text,

    PresetColor::chrome_background,
    PresetColor::control_background,
    PresetColor::window_text,
    PresetColor::control_hover_background,
    PresetColor::window_text,
    PresetColor::selection_background,
    PresetColor::window_text,
    PresetColor::outline,

    PresetColor::control_background,
    PresetColor::window_text,
    PresetColor::control_hover_background,
    PresetColor::window_text,
    PresetColor::control_pressed_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::outline,

    PresetColor::input_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::control_hover_background,

    PresetColor::control_hover_background,
    PresetColor::window_text,
    PresetColor::control_background,
    PresetColor::control_hover_background,
    PresetColor::accent,
    PresetColor::outline,

    PresetColor::control_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::control_background,
    PresetColor::window_text,
    PresetColor::selection_background,
    PresetColor::accent,

    PresetColor::window_text,
    PresetColor::input_background,
    PresetColor::control_hover_background,
    PresetColor::accent,
    PresetColor::outline,
    PresetColor::accent,
    PresetColor::alternate_background,
    PresetColor::window_text,

    PresetColor::chrome_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::input_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::selection_background,
    PresetColor::window_text,
    PresetColor::control_pressed_background,
    PresetColor::window_text,
    PresetColor::separator,

    PresetColor::input_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::outline,

    PresetColor::item_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::alternate_background,
    PresetColor::secondary_text,
    PresetColor::item_hover_background,
    PresetColor::window_text,
    PresetColor::selection_background,
    PresetColor::window_text,

    PresetColor::chrome_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::control_hover_background,
    PresetColor::window_text,
    PresetColor::control_pressed_background,
    PresetColor::window_text,

    PresetColor::chrome_background,
    PresetColor::window_background,
    PresetColor::scrollbar_handle,
    PresetColor::outline,
    PresetColor::outline,

    PresetColor::window_background,
    PresetColor::separator,
    PresetColor::accent,
    PresetColor::outline,

    PresetColor::control_pressed_background,
    PresetColor::window_text,
    PresetColor::accent,
    PresetColor::window_text,

    PresetColor::item_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::item_background,
    PresetColor::window_text,

    PresetColor::chrome_background,
    PresetColor::separator,
    PresetColor::window_text,
    PresetColor::secondary_text,
    PresetColor::selection_background,
    PresetColor::accent,
    PresetColor::window_text,
    PresetColor::selection_background,
    PresetColor::accent,
    PresetColor::window_text,
    PresetColor::playhead,
    PresetColor::control_pressed_background,
    PresetColor::scrollbar_handle,
    PresetColor::piano_roll_background,
    PresetColor::piano_roll_accidental_lane,
    PresetColor::piano_natural_key,
    PresetColor::piano_black_key,
    PresetColor::selection_background,
    PresetColor::piano_keyboard_separator,
    PresetColor::piano_keyboard_label,
    PresetColor::selection_background,
    PresetColor::window_text,
    PresetColor::selection_background,
    PresetColor::accent,
    PresetColor::outline,
    PresetColor::secondary_text,
};

static_assert(rolePresetColors.size() == roleCount);

constexpr PresetColor presetColorFor(Role role) {
  return rolePresetColors.at(static_cast<std::size_t>(role));
}

constexpr PresetColors makeVanilla() {
  PresetColors colors;
  colors.color(PresetColor::window_background) = "#C9C1BB";
  colors.color(PresetColor::window_text) = "#302C29";
  colors.color(PresetColor::disabled_text) = "#8B847E";
  colors.color(PresetColor::outline) = "#8C857F";
  colors.color(PresetColor::selection_background) = "#B9E8EE";
  colors.color(PresetColor::accent) = "#00CADB";
  colors.color(PresetColor::chrome_background) = "#BDB5AF";
  colors.color(PresetColor::separator) = "#5B5652";
  colors.color(PresetColor::control_background) = "#E1DBD6";
  colors.color(PresetColor::control_hover_background) = "#ECE7E1";
  colors.color(PresetColor::control_pressed_background) = "#F5B61C";
  colors.color(PresetColor::input_background) = "#F3F0ED";
  colors.color(PresetColor::alternate_background) = "#D1CBC5";
  colors.color(PresetColor::item_background) = "#D2D0CA";
  colors.color(PresetColor::secondary_text) = "#57514C";
  colors.color(PresetColor::item_hover_background) = "#E7E2DC";
  colors.color(PresetColor::scrollbar_handle) = "#A49D97";
  colors.color(PresetColor::piano_roll_background) = "#D9D3CF";
  colors.color(PresetColor::piano_roll_accidental_lane) = "#C9C3BF";
  colors.color(PresetColor::playhead) = "#E24242";
  colors.color(PresetColor::piano_natural_key) = "#F4F4F4";
  colors.color(PresetColor::piano_black_key) = "#434040";
  colors.color(PresetColor::piano_keyboard_separator) = "#9A9A9A";
  colors.color(PresetColor::piano_keyboard_label) = "#1A1A1A";
  return colors;
}

// Chromatic colors are gamut-mapped at OKLCh hue 213 degrees.
constexpr PresetColors makeDarkNeutralHigh() {
  auto colors = PresetColors{};
  colors.color(PresetColor::window_background) = "#373737";
  colors.color(PresetColor::window_text) = "#D8D8D8";
  colors.color(PresetColor::disabled_text) = "#C1C1C1";
  colors.color(PresetColor::outline) = "#505050";
  colors.color(PresetColor::selection_background) = "#9FCDD7";
  colors.color(PresetColor::accent) = "#037384";
  colors.color(PresetColor::chrome_background) = "#424242";
  colors.color(PresetColor::separator) = "#262626";
  colors.color(PresetColor::control_background) = "#1A1A1A";
  colors.color(PresetColor::control_hover_background) = "#6C6C6C";
  colors.color(PresetColor::control_pressed_background) = "#00D3F2";
  colors.color(PresetColor::input_background) = "#252525";
  colors.color(PresetColor::alternate_background) = "#575757";
  colors.color(PresetColor::item_background) = "#424242";
  colors.color(PresetColor::secondary_text) = "#BDBDBD";
  colors.color(PresetColor::item_hover_background) = "#5B5B5B";
  colors.color(PresetColor::scrollbar_handle) = "#262626";
  colors.color(PresetColor::piano_roll_background) = "#454545";
  colors.color(PresetColor::piano_roll_accidental_lane) = "#383838";
  colors.color(PresetColor::playhead) = "#E24242";
  colors.color(PresetColor::piano_natural_key) = "#AAAAAA";
  colors.color(PresetColor::piano_black_key) = "#4C4C4C";
  colors.color(PresetColor::piano_keyboard_separator) = "#404040";
  colors.color(PresetColor::piano_keyboard_label) = "#FFFFFF";
  return colors;
}

constexpr PresetColors makeImmaterial() {
  auto colors = PresetColors{};
  colors.color(PresetColor::window_background) = "#2E3138";
  colors.color(PresetColor::window_text) = "#CBCBCD";
  colors.color(PresetColor::disabled_text) = "#A9ACB5";
  colors.color(PresetColor::outline) = "#545559";
  colors.color(PresetColor::selection_background) = "#ABCAD2";
  colors.color(PresetColor::accent) = "#008493";
  colors.color(PresetColor::chrome_background) = "#363941";
  colors.color(PresetColor::separator) = "#292A2E";
  colors.color(PresetColor::control_background) = "#292A2E";
  colors.color(PresetColor::control_hover_background) = "#5F626B";
  colors.color(PresetColor::control_pressed_background) = "#F98CBE";
  colors.color(PresetColor::input_background) = "#25272B";
  colors.color(PresetColor::alternate_background) = "#52545C";
  colors.color(PresetColor::item_background) = "#393C43";
  colors.color(PresetColor::secondary_text) = "#A5A8B0";
  colors.color(PresetColor::item_hover_background) = "#51545C";
  colors.color(PresetColor::scrollbar_handle) = "#212225"; // also grid color
  colors.color(PresetColor::piano_roll_background) = "#3C3F46";
  colors.color(PresetColor::piano_roll_accidental_lane) = "#2F3239";
  colors.color(PresetColor::playhead) = "#E24242";
  colors.color(PresetColor::piano_natural_key) = "#898B92";
  colors.color(PresetColor::piano_black_key) = "#21232B";
  colors.color(PresetColor::piano_keyboard_separator) = "#34373F";
  colors.color(PresetColor::piano_keyboard_label) = "#F3F3F5";
  return colors;
}

constexpr bool isHexDigit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F') ||
         (value >= 'a' && value <= 'f');
}

constexpr bool isCompletePreset(const PresetColors &colors) {
  for (const HexColor color : colors.values) {
    if (color.size() != 7 || color[0] != '#' || !isHexDigit(color[1]) ||
        !isHexDigit(color[2]) || !isHexDigit(color[3]) ||
        !isHexDigit(color[4]) || !isHexDigit(color[5]) ||
        !isHexDigit(color[6])) {
      return false;
    }
  }
  return true;
}

inline constexpr auto vanilla = makeVanilla();
inline constexpr auto darkNeutralHigh = makeDarkNeutralHigh();
inline constexpr auto immaterial = makeImmaterial();

static_assert(isCompletePreset(vanilla));
static_assert(isCompletePreset(darkNeutralHigh));
static_assert(isCompletePreset(immaterial));

} // namespace themes::preset_colors
