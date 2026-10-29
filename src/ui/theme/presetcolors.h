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
    /// Hovered buttons/tabs/indicators/headers, and the shared resting surface
    /// of combo, text, and spin-box fields.
    control_hover_background,
    /// Pressed buttons, menus, headers, and checked Mute/Solo controls.
    control_pressed_background,
    /// Tooltips and checkbox/radio indicator surfaces.
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
    /// Scrollbar thumbs.
    scrollbar_handle,
    /// SongView grid lines.
    grid_line,
    /// SongView piano-roll note area background; its lightness selects the
    /// authored accidental-lane color.
    piano_roll_background,
    /// SongView piano-roll accidental-note lane background.
    piano_roll_accidental_lane,
    /// SongView playhead: a fixed identity red shared by every theme, like
    /// the Mute and Solo domain colors (rollcheck pixel-scans for it).
    playhead,
    /// Natural (white) keys on the SongView piano keyboard: a fixed white
    /// shared by every theme so the keyboard always reads black-and-white.
    piano_natural_key,
    /// Black (accidental) keys on the SongView piano keyboard: a fixed
    /// near-black shared by every theme.
    piano_black_key,
    /// Octave separator lines on the SongView piano keyboard.
    piano_keyboard_separator,
    /// Note labels on the SongView piano keyboard.
    piano_keyboard_label,
    /// Text on active/releasing/shadow polyphony cells: a fixed near-white
    /// shared by every theme because the fills below it are fixed too.
    polyphony_cell_text,
    /// Polyphony cell holding a sounding note: a fixed identity green shared
    /// by every theme, matching the CLAP plugin's channel display.
    polyphony_active_cell,
    /// Polyphony cell fading a released note: a fixed identity amber.
    polyphony_releasing_cell,
    /// Polyphony cell playing a lost sound in solo-overflow mode: a fixed
    /// identity blue.
    polyphony_shadow_cell,
    /// Dropped-note rows in the polyphony event log, and the status bar's CPU
    /// readout at the frame ceiling.
    polyphony_dropped_text,
    /// Cut-off-note rows in the polyphony event log, and the status bar's CPU
    /// readout over its budget.
    polyphony_stolen_text,
    /// Overflow-table row flash on a fresh drop: a fixed identity red faded
    /// by the panel.
    polyphony_flash,
    /// The CPU dock's breakdown segments: fixed identities shared by every
    /// theme, kept apart from the Polyphony cells' green/amber/blue and the
    /// warning amber/red: teal (sequencer), pink (CGB), purple (mixer).
    cpu_sequencer_fill,
    cpu_cgb_fill,
    cpu_mixer_fill,
    /// The CPU profile strip's cheap end: a quiet neutral that still reads
    /// against the timeline chrome it sits on. Its amber and red are the
    /// Polyphony cells' fixed releasing amber and flash red.
    cpu_profile_low,
    /// Sample Editor waveform trace, drawn on the item surface behind it.
    sample_waveform_ink,
    /// Sample Editor crop handles: the amber domain color, kept legible on the
    /// item surface.
    sample_crop_handle,
    /// Sample Editor loop handles, loop tint, and seam-start trace: the blue
    /// domain color, kept legible on the item and alternate surfaces.
    sample_loop_handle,
    /// Sample Editor seam-end trace: the red domain color, kept legible on the
    /// seam inset's alternate surface.
    sample_seam_end_ink,
    /// Sentinel for the number of authored preset colors; not rendered.
    count,
};

inline constexpr auto presetColorCount = static_cast<std::size_t>(PresetColor::count);
static_assert(presetColorCount == 40);

constexpr PresetColor presetColorFor(Role role);

/// Fixed-theme colors are authored as this compact palette. The role map below
/// adapts the palette to the complete rendering interface.
struct PresetColors {
    std::array<HexColor, presetColorCount> values{};

    constexpr HexColor &color(PresetColor presetColor)
    {
        return values.at(static_cast<std::size_t>(presetColor));
    }

    constexpr HexColor color(PresetColor presetColor) const
    {
        return values.at(static_cast<std::size_t>(presetColor));
    }

    constexpr HexColor color(Role role) const { return color(presetColorFor(role)); }
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

    PresetColor::control_hover_background,
    PresetColor::window_text,
    PresetColor::outline,

    PresetColor::control_hover_background,
    PresetColor::window_text,
    PresetColor::control_background,
    PresetColor::control_hover_background,
    PresetColor::control_pressed_background,
    PresetColor::outline,

    PresetColor::control_hover_background,
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
    PresetColor::item_background,
    PresetColor::window_text,
    PresetColor::outline,
    PresetColor::item_hover_background,
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
    PresetColor::control_pressed_background,
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
    // The CPU readout's amber and red: the event log's severity inks, which
    // are authored for the same item surface.
    PresetColor::polyphony_stolen_text,
    PresetColor::polyphony_dropped_text,
    // The CPU dock's breakdown segments: fixed identities, like the cells.
    PresetColor::cpu_sequencer_fill,
    PresetColor::cpu_cgb_fill,
    PresetColor::cpu_mixer_fill,
    // The CPU profile strip: its neutral end, then the fixed amber and red.
    PresetColor::cpu_profile_low,
    PresetColor::polyphony_releasing_cell,
    PresetColor::polyphony_flash,

    PresetColor::control_background,
    PresetColor::disabled_text,
    PresetColor::polyphony_cell_text,
    PresetColor::polyphony_active_cell,
    PresetColor::polyphony_releasing_cell,
    PresetColor::polyphony_shadow_cell,
    PresetColor::polyphony_dropped_text,
    PresetColor::polyphony_stolen_text,
    PresetColor::polyphony_flash,

    PresetColor::chrome_background,
    PresetColor::separator,
    PresetColor::window_text,
    PresetColor::secondary_text,
    PresetColor::selection_background,
    PresetColor::accent,
    PresetColor::disabled_text,
    PresetColor::selection_background,
    PresetColor::window_text,
    PresetColor::window_text,
    PresetColor::playhead,
    PresetColor::grid_line,
    PresetColor::piano_roll_background,
    PresetColor::piano_roll_accidental_lane,
    PresetColor::piano_natural_key,
    PresetColor::piano_black_key,
    PresetColor::selection_background,
    PresetColor::piano_keyboard_separator,
    PresetColor::piano_keyboard_label,
    PresetColor::selection_background,
    PresetColor::window_text,
    // The velocity lane's PSG level lines want exactly what the accidental
    // lane wants — one authored shade under the plot background.
    PresetColor::piano_roll_accidental_lane,
    PresetColor::selection_background,
    PresetColor::accent,
    PresetColor::outline,
    PresetColor::secondary_text,

    PresetColor::sample_waveform_ink,
    PresetColor::sample_crop_handle,
    PresetColor::sample_loop_handle,
    PresetColor::sample_seam_end_ink,
};

static_assert(rolePresetColors.size() == roleCount);

constexpr PresetColor presetColorFor(Role role)
{
    return rolePresetColors.at(static_cast<std::size_t>(role));
}

constexpr PresetColors makeVanilla()
{
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
    colors.color(PresetColor::grid_line) = "#3F040000";
    colors.color(PresetColor::piano_roll_background) = "#D4CCC7";
    colors.color(PresetColor::piano_roll_accidental_lane) = "#B7AFA9";
    colors.color(PresetColor::playhead) = "#E24242";
    colors.color(PresetColor::piano_natural_key) = "#F4F4F4";
    colors.color(PresetColor::piano_black_key) = "#202224";
    colors.color(PresetColor::piano_keyboard_separator) = "#BCB4AF";
    colors.color(PresetColor::piano_keyboard_label) = "#1A1A1A";
    colors.color(PresetColor::polyphony_cell_text) = "#F0F0F4";
    colors.color(PresetColor::polyphony_active_cell) = "#268C38";
    colors.color(PresetColor::polyphony_releasing_cell) = "#B8871A";
    colors.color(PresetColor::polyphony_shadow_cell) = "#2961BF";
    // Severity inks keep their red and amber hues but darken to 4.5:1 on the
    // light item surface that hosts the event log.
    colors.color(PresetColor::polyphony_dropped_text) = "#9C2B2B";
    colors.color(PresetColor::polyphony_stolen_text) = "#6E4A06";
    colors.color(PresetColor::polyphony_flash) = "#D92626";
    colors.color(PresetColor::cpu_sequencer_fill) = "#1A9C8C";
    colors.color(PresetColor::cpu_cgb_fill) = "#C44F8E";
    colors.color(PresetColor::cpu_mixer_fill) = "#7A55C9";
    colors.color(PresetColor::cpu_profile_low) = "#8E9CA8";
    // The accent is too pale for a trace on the light item surface (the raw
    // selection cyan sat 1.16:1 there); this is the accent hue walked toward
    // black by the same policy Custom themes use, landing at 5.09:1.
    colors.color(PresetColor::sample_waveform_ink) = "#005B63";
    // The identity amber/blue/red sat 1.47/1.54/2.29:1 on the light surfaces;
    // each is its hue walked toward black by the Custom-theme policy to the
    // 3:1 UI-component bar (3.22, 3.33 worst-surface, 3.05:1).
    colors.color(PresetColor::sample_crop_handle) = "#92681F";
    colors.color(PresetColor::sample_loop_handle) = "#2A7292";
    colors.color(PresetColor::sample_seam_end_ink) = "#C54444";
    return colors;
}

// Chromatic colors are gamut-mapped at OKLCh hue 213 degrees.
constexpr PresetColors makeDarkNeutralHigh()
{
    auto colors = PresetColors{};
    colors.color(PresetColor::window_background) = "#373737";
    colors.color(PresetColor::window_text) = "#D8D8D8";
    // 4.55:1 on the window surface, 1.83:1 from enabled text; the old
    // #C1C1C1 sat 1.26:1 from enabled and read as live.
    colors.color(PresetColor::disabled_text) = "#A0A0A0";
    colors.color(PresetColor::outline) = "#505050";
    colors.color(PresetColor::selection_background) = "#9FCDD7";
    colors.color(PresetColor::accent) = "#037384";
    colors.color(PresetColor::chrome_background) = "#424242";
    colors.color(PresetColor::separator) = "#262626";
    colors.color(PresetColor::control_background) = "#1A1A1A";
    // 4.69:1 under the shared window text; the old #6C6C6C held only 3.68:1,
    // so hovered controls and menu-bar items failed the AA floor themecheck
    // enforces.
    colors.color(PresetColor::control_hover_background) = "#5C5C5C";
    colors.color(PresetColor::control_pressed_background) = "#00D3F2";
    colors.color(PresetColor::input_background) = "#252525";
    colors.color(PresetColor::alternate_background) = "#575757";
    colors.color(PresetColor::item_background) = "#424242";
    colors.color(PresetColor::secondary_text) = "#BDBDBD";
    colors.color(PresetColor::item_hover_background) = "#5B5B5B";
    colors.color(PresetColor::scrollbar_handle) = "#262626";
    colors.color(PresetColor::grid_line) = "#54030303";
    colors.color(PresetColor::piano_roll_background) = "#454545";
    colors.color(PresetColor::piano_roll_accidental_lane) = "#363636";
    colors.color(PresetColor::playhead) = "#E24242";
    colors.color(PresetColor::piano_natural_key) = "#F4F4F4";
    colors.color(PresetColor::piano_black_key) = "#202224";
    colors.color(PresetColor::piano_keyboard_separator) = "#9A9A9A";
    colors.color(PresetColor::piano_keyboard_label) = "#1A1A1A";
    colors.color(PresetColor::polyphony_cell_text) = "#F0F0F4";
    colors.color(PresetColor::polyphony_active_cell) = "#268C38";
    colors.color(PresetColor::polyphony_releasing_cell) = "#B8871A";
    colors.color(PresetColor::polyphony_shadow_cell) = "#2961BF";
    // Severity inks lighten instead: 4.5:1 on the dark item surface.
    colors.color(PresetColor::polyphony_dropped_text) = "#F09999";
    colors.color(PresetColor::polyphony_stolen_text) = "#E2A854";
    colors.color(PresetColor::polyphony_flash) = "#D92626";
    colors.color(PresetColor::cpu_sequencer_fill) = "#1A9C8C";
    colors.color(PresetColor::cpu_cgb_fill) = "#C44F8E";
    colors.color(PresetColor::cpu_mixer_fill) = "#7A55C9";
    colors.color(PresetColor::cpu_profile_low) = "#56626E";
    // Matches the selection cyan the waveform always rendered here: 5.83:1 on
    // the item surface, so this preset keeps its look.
    colors.color(PresetColor::sample_waveform_ink) = "#9FCDD7";
    // The identity amber already clears the bar here (4.42:1); the blue and
    // red walk one and seven steps toward white for the seam inset's lighter
    // alternate surface (3.07 and 3.06:1 there).
    colors.color(PresetColor::sample_crop_handle) = "#E0A030";
    colors.color(PresetColor::sample_loop_handle) = "#4AB4E2";
    colors.color(PresetColor::sample_seam_end_ink) = "#F08D8D";
    return colors;
}

constexpr PresetColors makeImmaterial()
{
    auto colors = PresetColors{};
    colors.color(PresetColor::window_background) = "#2E3138";
    colors.color(PresetColor::window_text) = "#CBCBCD";
    // 4.63:1 on the window surface, 1.74:1 from enabled text (was 1.40:1).
    colors.color(PresetColor::disabled_text) = "#979AA3";
    colors.color(PresetColor::outline) = "#545559";
    colors.color(PresetColor::selection_background) = "#ABCAD2";
    colors.color(PresetColor::accent) = "#008493";
    colors.color(PresetColor::chrome_background) = "#363941";
    colors.color(PresetColor::separator) = "#292A2E";
    colors.color(PresetColor::control_background) = "#292A2E";
    // 4.60:1 under the shared window text; the old #5F626B held only 3.76:1,
    // so hovered controls and menu-bar items failed the AA floor themecheck
    // enforces.
    colors.color(PresetColor::control_hover_background) = "#52555E";
    colors.color(PresetColor::control_pressed_background) = "#F98CBE";
    colors.color(PresetColor::input_background) = "#25272B";
    colors.color(PresetColor::alternate_background) = "#52545C";
    colors.color(PresetColor::item_background) = "#393C43";
    colors.color(PresetColor::secondary_text) = "#A5A8B0";
    colors.color(PresetColor::item_hover_background) = "#51545C";
    colors.color(PresetColor::scrollbar_handle) = "#212225";
    colors.color(PresetColor::grid_line) = "#54030606";
    colors.color(PresetColor::piano_roll_background) = "#3C3F46";
    colors.color(PresetColor::piano_roll_accidental_lane) = "#2F3239";
    colors.color(PresetColor::playhead) = "#E24242";
    colors.color(PresetColor::piano_natural_key) = "#F4F4F4";
    colors.color(PresetColor::piano_black_key) = "#202224";
    colors.color(PresetColor::piano_keyboard_separator) = "#9A9A9A";
    colors.color(PresetColor::piano_keyboard_label) = "#1A1A1A";
    colors.color(PresetColor::polyphony_cell_text) = "#F0F0F4";
    colors.color(PresetColor::polyphony_active_cell) = "#268C38";
    colors.color(PresetColor::polyphony_releasing_cell) = "#B8871A";
    colors.color(PresetColor::polyphony_shadow_cell) = "#2961BF";
    colors.color(PresetColor::polyphony_dropped_text) = "#F09999";
    colors.color(PresetColor::polyphony_stolen_text) = "#E2A854";
    colors.color(PresetColor::polyphony_flash) = "#D92626";
    colors.color(PresetColor::cpu_sequencer_fill) = "#1A9C8C";
    colors.color(PresetColor::cpu_cgb_fill) = "#C44F8E";
    colors.color(PresetColor::cpu_mixer_fill) = "#7A55C9";
    colors.color(PresetColor::cpu_profile_low) = "#4E5A68";
    // Matches the selection cyan the waveform always rendered here: 6.37:1 on
    // the item surface, so this preset keeps its look.
    colors.color(PresetColor::sample_waveform_ink) = "#ABCAD2";
    // The identity amber and blue already clear the bar here (4.86 and 3.06:1
    // worst-surface); the red walks six steps toward white for the seam
    // inset's alternate surface (3.01:1 there).
    colors.color(PresetColor::sample_crop_handle) = "#E0A030";
    colors.color(PresetColor::sample_loop_handle) = "#40B0E0";
    colors.color(PresetColor::sample_seam_end_ink) = "#EF8585";
    return colors;
}

constexpr bool isHexDigit(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F') ||
           (value >= 'a' && value <= 'f');
}

constexpr bool isCompletePreset(const PresetColors &colors)
{
    for (const HexColor color : colors.values) {
        if ((color.size() != 7 && color.size() != 9) || color[0] != '#')
            return false;
        for (std::size_t index = 1; index < color.size(); ++index) {
            if (!isHexDigit(color[index]))
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
