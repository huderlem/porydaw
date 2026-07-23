#pragma once

#include <cstddef>

namespace themes {

// Fully resolved UI color roles. Source swatches stay private to the resolver;
// each public role names the component or paint path that consumes it.
enum class Role {
  // QApplication palette and window-level defaults.
  window_background,
  window_text,
  disabled_text,
  palette_outline,
  selection_background,
  selection_text,
  link_text,

  // QToolBar chrome and icon-only transport controls.
  toolbar_background,
  toolbar_text,
  toolbar_outline,
  transport_text,

  // QTabWidget pane and open-song tabs.
  tab_pane_background,
  tab_background,
  tab_text,
  tab_hover_background,
  tab_hover_text,
  tab_selected_background,
  tab_selected_text,
  tab_outline,

  // QPushButton and ordinary QToolButton states.
  button_background,
  button_text,
  button_hover_background,
  button_hover_text,
  button_pressed_background,
  button_pressed_text,
  button_outline,
  focus_outline,

  // Line and text editor surfaces.
  input_background,
  input_text,
  input_outline,
  input_highlight_outline,

  // QComboBox field and drop-down lane.
  combo_background,
  combo_text,
  combo_drop_down_background,
  combo_drop_down_hover_background,
  combo_drop_down_pressed_background,
  combo_outline,

  // QAbstractSpinBox field and arrow buttons.
  spin_box_background,
  spin_box_text,
  spin_box_outline,
  spin_button_background,
  spin_button_text,
  spin_button_hover_background,
  spin_button_pressed_background,

  // QCheckBox and QRadioButton labels and indicators.
  indicator_text,
  indicator_background,
  indicator_hover_background,
  indicator_checked_background,
  indicator_outline,
  indicator_checked_outline,
  indicator_disabled_background,
  indicator_check_mark,

  // QMenuBar, popup menus, and their items.
  menu_bar_background,
  menu_bar_text,
  menu_bar_outline,
  menu_background,
  menu_text,
  menu_outline,
  menu_item_hover_background,
  menu_item_hover_text,
  menu_item_pressed_background,
  menu_item_pressed_text,
  menu_separator,

  // Tooltips and group-box framing.
  tooltip_background,
  tooltip_text,
  tooltip_outline,
  group_box_outline,

  // Item-view surfaces, rows, and selection states.
  item_background,
  item_text,
  item_outline,
  item_alternate_background,
  secondary_text,
  item_hover_background,
  item_hover_text,
  item_selected_background,
  item_selected_text,

  // QHeaderView sections.
  header_background,
  header_text,
  header_outline,
  header_hover_background,
  header_hover_text,
  header_checked_background,
  header_checked_text,

  // Scrollbar track, handle, and page areas.
  scrollbar_background,
  scrollbar_page_background,
  scrollbar_handle,
  scrollbar_handle_hover_background,
  scrollbar_outline,

  // Splitter surface and draggable handle.
  splitter_background,
  splitter_handle,
  splitter_handle_hover_background,
  splitter_outline,

  // SongView Mute and Solo controls.
  track_mute_checked_background,
  track_mute_checked_text,
  track_solo_checked_background,
  track_solo_checked_text,

  // SongView track-header panel and polyphony readouts.
  track_header_panel_background,
  track_header_panel_text,
  track_header_panel_outline,
  polyphony_value_background,
  polyphony_value_text,

  // Canvas colors consumed directly by SongView paint paths.
  song_view_timeline_chrome_background,
  song_view_separator,
  song_view_primary_text,
  song_view_secondary_text,
  song_view_selection_fill,
  song_view_selection_edge,
  song_view_selected_note_inner_border,
  song_view_loop_marker,
  song_view_edit_cursor,
  song_view_edit_preview_outline,
  song_view_playhead,
  song_view_unterminated_note_outline,
  song_view_grid,
  song_view_piano_roll_background,
  song_view_piano_roll_accidental_lane,
  song_view_piano_keyboard_natural_key,
  song_view_piano_keyboard_black_key,
  song_view_piano_keyboard_active_key,
  song_view_piano_keyboard_separator,
  song_view_piano_keyboard_label,
  song_view_track_header_selection,
  song_view_track_header_selection_text,
  song_view_automation_default_curve,
  song_view_automation_tempo_curve,
  song_view_file_event_marker,
  song_view_add_automation_lane_action,

  count,
};

inline constexpr auto roleCount = static_cast<std::size_t>(Role::count);

} // namespace themes
