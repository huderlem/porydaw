#include "themeruntime.h"
#include "ui/layout.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPolygonF>

#include <QSet>
#include <QWidget>
#include <optional>

namespace themes {
namespace {

// Qt has one application palette, so runtime theme state is intentionally
// process-wide and captured before the first application-level override.
std::optional<QPalette> capturedStartupPalette;
QSet<QWidget *> gridLineRepaintTargets;
std::optional<Theme> currentTheme;

// QPalette covers colors Qt paints natively; the component QSS fragments below
// handle states and subcontrols that one global palette cannot distinguish.
void applyPaletteGroup(QPalette &palette, QPalette::ColorGroup group,
                       const Theme &theme) {
  const auto text =
      group == QPalette::Disabled ? Role::disabled_text : Role::window_text;
  const auto buttonText =
      group == QPalette::Disabled ? Role::disabled_text : Role::button_text;
  const auto set = [&](QPalette::ColorRole target, Role source) {
    palette.setColor(group, target, theme.color(source));
  };
  set(QPalette::Window, Role::window_background);
  set(QPalette::Dark, Role::toolbar_background);
  set(QPalette::WindowText, text);
  set(QPalette::Text, text);
  set(QPalette::PlaceholderText, Role::disabled_text);
  set(QPalette::Mid, Role::palette_outline);
  set(QPalette::Highlight, Role::selection_background);
  set(QPalette::HighlightedText, Role::selection_text);
  set(QPalette::Link, Role::link_text);
  set(QPalette::Button, Role::button_background);
  set(QPalette::ButtonText, buttonText);
  // Item-view viewports are painted from Base by the platform style.
  set(QPalette::Base, Role::item_background);
  set(QPalette::AlternateBase, Role::item_alternate_background);
  set(QPalette::Light, Role::scrollbar_handle);
}

// Keep one QSS fragment per widget family. Its local placeholders sit beside
// the matching roles, so editing one family cannot renumber another's colors.
QString colorName(const Theme &theme, Role role) {
  return theme.color(role).name(QColor::HexRgb).toUpper();
}

QString windowStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QMainWindow,QDialog,QDockWidget,QScrollArea,QStackedWidget{"
             "background-color:%1;color:%2;}"
             "QLabel#voicegroupEditorNotice{color:%3;}")
      .arg(colorName(theme, Role::window_background))
      .arg(colorName(theme, Role::window_text))
      .arg(colorName(theme, Role::secondary_text));
}

QString trackHeaderStyleSheet(const Theme &theme) {
  return QStringLiteral("QWidget#trackHeaderPanel{background-color:%1;color:%2;"
                        "border-color:%3;}"
                        "QLabel#polyphonyPcmValue,QLabel#polyphonyCgbValue,"
                        "QLabel#polyphonyLostValue{"
                        "background-color:%4;color:%5;}")
      .arg(colorName(theme, Role::track_header_panel_background))
      .arg(colorName(theme, Role::track_header_panel_text))
      .arg(colorName(theme, Role::track_header_panel_outline))
      .arg(colorName(theme, Role::polyphony_value_background))
      .arg(colorName(theme, Role::polyphony_value_text));
}

QString toolbarStyleSheet(const Theme &theme) {
  // Transport buttons rest flat on the toolbar chrome but answer the mouse
  // with the shared button interaction ramp; a press adds the focus outline
  // so the click lands visibly without changing the icon's surface (the
  // hovered icon tint stays readable). Checked state, ordered last, wins
  // over hover and press so an engaged Loop never loses its fill.
  return QStringLiteral(
             "QToolBar{background-color:%1;color:%2;border-color:%3;}"
             "QToolBar#transportToolbar "
             "QToolButton{background-color:transparent;"
             "border-color:transparent;color:%4;}"
             "QToolBar#transportToolbar QToolButton:hover{"
             "background-color:%6;border-color:transparent;color:%7;}"
             "QToolBar#transportToolbar QToolButton:pressed{"
             "background-color:%6;border-color:%8;color:%7;}"
             "QToolBar#transportToolbar QToolButton:checked{"
             "background-color:%9;border-color:transparent;color:%10;}"
             "QToolBar#transportToolbar QLabel{background-color:transparent;"
             "color:%4;}"
             "QToolBar::separator{background-color:%5;}")
      .arg(colorName(theme, Role::toolbar_background))
      .arg(colorName(theme, Role::toolbar_text))
      .arg(colorName(theme, Role::toolbar_outline))
      .arg(colorName(theme, Role::transport_text))
      .arg(colorName(theme, Role::toolbar_separator))
      .arg(colorName(theme, Role::button_hover_background))
      .arg(colorName(theme, Role::button_hover_text))
      .arg(colorName(theme, Role::focus_outline))
      .arg(colorName(theme, Role::button_pressed_background))
      .arg(colorName(theme, Role::button_pressed_text));
}

QString tabStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QTabWidget::pane{background-color:%1;border-color:%2;}"
             "QTabBar::tab{background-color:%3;color:%4;border-color:%2;}"
             "QTabBar::tab:hover{background-color:%5;color:%6;"
             "border-color:%2;}"
             "QTabBar::tab:selected{background-color:%7;color:%8;"
             "border-color:%2;}")
      .arg(colorName(theme, Role::tab_pane_background))
      .arg(colorName(theme, Role::tab_outline))
      .arg(colorName(theme, Role::tab_background))
      .arg(colorName(theme, Role::tab_text))
      .arg(colorName(theme, Role::tab_hover_background))
      .arg(colorName(theme, Role::tab_hover_text))
      .arg(colorName(theme, Role::tab_selected_background))
      .arg(colorName(theme, Role::tab_selected_text));
}

QString buttonStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QPushButton,QToolButton{background-color:%1;color:%2;"
             "border-color:%3;}"
             "QPushButton:hover,QToolButton:hover{background-color:%4;"
             "color:%5;border-color:%3;}"
             "QPushButton:focus,QToolButton:focus{border-color:%6;}"
             "QPushButton:pressed,QPushButton:checked,QToolButton:pressed,"
             "QToolButton:checked{background-color:%7;color:%8;"
             "border-color:%3;}"
             "QPushButton:disabled,QToolButton:disabled{background-color:%1;"
             "color:%9;border-color:%3;}")
      .arg(colorName(theme, Role::button_background))
      .arg(colorName(theme, Role::button_text))
      .arg(colorName(theme, Role::button_outline))
      .arg(colorName(theme, Role::button_hover_background))
      .arg(colorName(theme, Role::button_hover_text))
      .arg(colorName(theme, Role::focus_outline))
      .arg(colorName(theme, Role::button_pressed_background))
      .arg(colorName(theme, Role::button_pressed_text))
      .arg(colorName(theme, Role::disabled_text));
}

QString trackControlStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QToolButton#trackSoloButton:pressed{background-color:%1;"
             "color:%2;border-color:%5;}"
             "QToolButton#trackMuteButton:checked{background-color:%3;"
             "color:%4;border-color:%5;}"
             "QToolButton#trackSoloButton:checked{background-color:%1;"
             "color:%2;border-color:%5;}"
             "QToolButton#trackMuteButton:disabled,"
             "QToolButton#trackSoloButton:disabled{background-color:%6;"
             "color:%7;border-color:%5;}")
      .arg(colorName(theme, Role::track_solo_checked_background))
      .arg(colorName(theme, Role::track_solo_checked_text))
      .arg(colorName(theme, Role::track_mute_checked_background))
      .arg(colorName(theme, Role::track_mute_checked_text))
      .arg(colorName(theme, Role::button_outline))
      .arg(colorName(theme, Role::button_background))
      .arg(colorName(theme, Role::disabled_text));
}

QString inputStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QLineEdit,QTextEdit,QPlainTextEdit{background-color:%1;"
             "color:%2;border-top-color:%3;border-left-color:%3;"
             "border-right-color:%4;border-bottom-color:%4;"
             "selection-background-color:%5;selection-color:%6;}"
             "QLineEdit:focus,QTextEdit:focus,QPlainTextEdit:focus{"
             "border-color:%7;}"
             "QLineEdit:disabled,QTextEdit:disabled,QPlainTextEdit:disabled{"
             "color:%8;}")
      .arg(colorName(theme, Role::input_background))
      .arg(colorName(theme, Role::input_text))
      .arg(colorName(theme, Role::input_highlight_outline))
      .arg(colorName(theme, Role::input_outline))
      .arg(colorName(theme, Role::selection_background))
      .arg(colorName(theme, Role::selection_text))
      .arg(colorName(theme, Role::focus_outline))
      .arg(colorName(theme, Role::disabled_text));
}

// The stylesheet renderer draws none of the base style's glyph art once a
// subcontrol is styled (and its border-triangle emulation fills the whole
// border box), so spin arrows and check marks are rendered to per-color
// image files the stylesheet can reference. The color-keyed name makes
// regeneration a cheap existence check and keeps a live preview from ever
// reading a half-written previous file.
enum class Glyph { SpinUp, SpinDown, CheckMark };

void paintGlyph(QPainter &painter, Glyph glyph, const QColor &color, int width,
                int height) {
  painter.setRenderHint(QPainter::Antialiasing);
  switch (glyph) {
  case Glyph::SpinUp:
  case Glyph::SpinDown: {
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const auto peakX = width / 2.0;
    painter.drawPolygon(glyph == Glyph::SpinUp
                            ? QPolygonF{{0.0, qreal(height)},
                                        {qreal(width), qreal(height)},
                                        {peakX, 0.0}}
                            : QPolygonF{{0.0, 0.0},
                                        {qreal(width), 0.0},
                                        {peakX, qreal(height)}});
    return;
  }
  case Glyph::CheckMark: {
    auto pen = QPen(color, qMax(2.0, height * 0.18));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QPointF points[] = {{width * 0.16, height * 0.52},
                              {width * 0.42, height * 0.76},
                              {width * 0.84, height * 0.24}};
    painter.drawPolyline(points, 3);
    return;
  }
  }
}

QSize glyphSize(Glyph glyph) {
  switch (glyph) {
  case Glyph::SpinUp:
  case Glyph::SpinDown:
    // An odd width keeps the triangle's peak on a whole pixel column.
    return {layout::fontPx(0.45) | 1, qMax(2, layout::fontPx(0.28))};
  case Glyph::CheckMark: {
    const auto side = qMax(6, layout::fontPx(0.62));
    return {side, side};
  }
  }
  Q_UNREACHABLE();
}

QString glyphName(Glyph glyph) {
  switch (glyph) {
  case Glyph::SpinUp:
    return QStringLiteral("spin-up");
  case Glyph::SpinDown:
    return QStringLiteral("spin-down");
  case Glyph::CheckMark:
    return QStringLiteral("check");
  }
  Q_UNREACHABLE();
}

QString glyphImagePath(Glyph glyph, const QColor &color,
                       int devicePixelRatio) {
  const auto directory = QDir::temp().filePath(QStringLiteral("porydaw-theme"));
  if (!QDir().mkpath(directory))
    return {};
  const auto path =
      QStringLiteral("%1/%2-%3%4.png")
          .arg(directory, glyphName(glyph), color.name(QColor::HexRgb).mid(1),
               devicePixelRatio > 1
                   ? QStringLiteral("@%1x").arg(devicePixelRatio)
                   : QString());
  if (QFile::exists(path))
    return path;
  const auto size = glyphSize(glyph);
  QImage image(size * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  paintGlyph(painter, glyph, color, size.width() * devicePixelRatio,
             size.height() * devicePixelRatio);
  painter.end();
  return image.save(path) ? path : QString();
}

// Returns the 1x path after also materializing the @2x high-DPI variant.
QString glyphImage(Glyph glyph, const QColor &color) {
  glyphImagePath(glyph, color, 2);
  return glyphImagePath(glyph, color, 1);
}

QString spinArrowStyleSheet(const Theme &theme) {
  const auto color = theme.color(Role::spin_button_text);
  const auto up = glyphImage(Glyph::SpinUp, color);
  const auto down = glyphImage(Glyph::SpinDown, color);
  if (up.isEmpty() || down.isEmpty())
    return {};
  return QStringLiteral("QAbstractSpinBox::up-arrow{image:url(%1);}"
                        "QAbstractSpinBox::down-arrow{image:url(%2);}")
      .arg(up, down);
}

QString checkMarkStyleSheet(const Theme &theme) {
  const auto check =
      glyphImage(Glyph::CheckMark, theme.color(Role::indicator_check_mark));
  if (check.isEmpty())
    return {};
  return QStringLiteral("QCheckBox::indicator:checked,"
                        "QMenu::indicator:checked{image:url(%1);}")
      .arg(check);
}

QString spinBoxStyleSheet(const Theme &theme) {
  return spinArrowStyleSheet(theme) +
         QStringLiteral(
             "QAbstractSpinBox{background-color:%1;color:%2;"
             "border-top-color:%3;border-left-color:%3;"
             "border-right-color:%4;border-bottom-color:%4;}"
             "QAbstractSpinBox:focus{border-color:%5;}"
             "QAbstractSpinBox:disabled{color:%6;}"
             "QAbstractSpinBox::up-button,QAbstractSpinBox::down-button{"
             "background-color:%7;color:%8;border-color:%4;}"
             "QAbstractSpinBox::up-button:hover,"
             "QAbstractSpinBox::down-button:hover{background-color:%9;"
             "border-color:%4;}"
             "QAbstractSpinBox::up-button:pressed,"
             "QAbstractSpinBox::down-button:pressed{background-color:%10;"
             "border-color:%4;}")
      .arg(colorName(theme, Role::spin_box_background))
      .arg(colorName(theme, Role::spin_box_text))
      .arg(colorName(theme, Role::input_highlight_outline))
      .arg(colorName(theme, Role::spin_box_outline))
      .arg(colorName(theme, Role::focus_outline))
      .arg(colorName(theme, Role::disabled_text))
      .arg(colorName(theme, Role::spin_button_background))
      .arg(colorName(theme, Role::spin_button_text))
      .arg(colorName(theme, Role::spin_button_hover_background))
      .arg(colorName(theme, Role::spin_button_pressed_background));
}

QString comboStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QComboBox{background-color:%1;color:%2;border-color:%3;}"
             "QComboBox:focus{border-color:%4;}"
             "QComboBox:disabled{color:%5;border-color:%3;}"
             "QComboBox::drop-down{background-color:%6;border-color:%3;}"
             "QComboBox:focus::drop-down{border-color:%4;}"
             "QComboBox::drop-down:hover{background-color:%7;}"
             "QComboBox::drop-down:pressed,QComboBox::drop-down:on{"
             "background-color:%8;}")
      .arg(colorName(theme, Role::combo_background))
      .arg(colorName(theme, Role::combo_text))
      .arg(colorName(theme, Role::combo_outline))
      .arg(colorName(theme, Role::focus_outline))
      .arg(colorName(theme, Role::disabled_text))
      .arg(colorName(theme, Role::combo_drop_down_background))
      .arg(colorName(theme, Role::combo_drop_down_hover_background))
      .arg(colorName(theme, Role::combo_drop_down_pressed_background));
}

QString indicatorStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QCheckBox,QRadioButton{background-color:transparent;color:%1;"
             "border-color:transparent;}"
             "QCheckBox:disabled,QRadioButton:disabled{color:%2;}"
             "QCheckBox::indicator,QRadioButton::indicator,QMenu::indicator{"
             "background-color:%3;border-color:%4;}"
             "QCheckBox::indicator:hover,QRadioButton::indicator:hover{"
             "background-color:%5;border-color:%4;}"
             "QCheckBox::indicator:checked,QRadioButton::indicator:checked,"
             "QMenu::indicator:checked,"
             "QCheckBox::indicator:indeterminate{background-color:%6;"
             "border-color:%7;}"
             "QCheckBox::indicator:disabled,QRadioButton::indicator:disabled{"
             "background-color:%8;border-color:%4;}")
             .arg(colorName(theme, Role::indicator_text))
             .arg(colorName(theme, Role::disabled_text))
             .arg(colorName(theme, Role::indicator_background))
             .arg(colorName(theme, Role::indicator_outline))
             .arg(colorName(theme, Role::indicator_hover_background))
             .arg(colorName(theme, Role::indicator_checked_background))
             .arg(colorName(theme, Role::indicator_checked_outline))
             .arg(colorName(theme, Role::indicator_disabled_background)) +
         checkMarkStyleSheet(theme);
}

QString menuBarStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QMenuBar{background-color:%1;color:%2;border-color:%3;}"
             "QMenuBar::item{background-color:%1;color:%2;border-color:%3;}"
             "QMenuBar::item:selected{background-color:%4;color:%5;}"
             "QMenuBar::item:pressed,QMenuBar::item:checked{"
             "background-color:%6;color:%7;}")
      .arg(colorName(theme, Role::menu_bar_background))
      .arg(colorName(theme, Role::menu_bar_text))
      .arg(colorName(theme, Role::menu_bar_outline))
      .arg(colorName(theme, Role::menu_item_hover_background))
      .arg(colorName(theme, Role::menu_item_hover_text))
      .arg(colorName(theme, Role::menu_item_pressed_background))
      .arg(colorName(theme, Role::menu_item_pressed_text));
}

QString menuStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QMenu{background-color:%1;color:%2;border-color:%3;}"
             "QMenu::item{background-color:%1;color:%2;border-color:%3;}"
             "QMenu::item:selected{background-color:%4;color:%5;}"
             // Checked items keep the plain item background: the indicator
             // square carries the state, as native menus do.
             "QMenu::item:pressed{"
             "background-color:%6;color:%7;}"
             "QMenu::item:disabled{color:%8;}"
             "QMenu::separator{background-color:%9;}")
      .arg(colorName(theme, Role::menu_background))
      .arg(colorName(theme, Role::menu_text))
      .arg(colorName(theme, Role::menu_outline))
      .arg(colorName(theme, Role::menu_item_hover_background))
      .arg(colorName(theme, Role::menu_item_hover_text))
      .arg(colorName(theme, Role::menu_item_pressed_background))
      .arg(colorName(theme, Role::menu_item_pressed_text))
      .arg(colorName(theme, Role::disabled_text))
      .arg(colorName(theme, Role::menu_separator));
}

QString tooltipStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QToolTip{background-color:%1;color:%2;border-color:%3;}")
      .arg(colorName(theme, Role::tooltip_background))
      .arg(colorName(theme, Role::tooltip_text))
      .arg(colorName(theme, Role::tooltip_outline));
}

QString groupBoxStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QGroupBox{background-color:transparent;color:%1;"
             "border-color:%2;}"
             "QGroupBox::title{background-color:%3;color:%1;}"
             "QDialogButtonBox{background-color:transparent;color:%1;"
             "border-color:transparent;}")
      .arg(colorName(theme, Role::window_text))
      .arg(colorName(theme, Role::group_box_outline))
      .arg(colorName(theme, Role::window_background));
}

QString itemViewStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QAbstractScrollArea{background-color:%1;color:%2;"
             "border-color:%3;}"
             "QAbstractItemView{background-color:%1;color:%2;"
             "border-color:%3;alternate-background-color:%4;"
             "selection-background-color:%5;selection-color:%6;}"
             "QAbstractItemView::item{background-color:%1;color:%2;"
             "border-color:transparent;}"
             "QAbstractItemView::item:alternate{background-color:%4;"
             "color:%7;}"
             "QAbstractItemView::item:hover{background-color:%8;color:%9;}"
             "QAbstractItemView::item:selected{background-color:%5;"
             "color:%6;}")
      .arg(colorName(theme, Role::item_background))
      .arg(colorName(theme, Role::item_text))
      .arg(colorName(theme, Role::item_outline))
      .arg(colorName(theme, Role::item_alternate_background))
      .arg(colorName(theme, Role::item_selected_background))
      .arg(colorName(theme, Role::item_selected_text))
      .arg(colorName(theme, Role::item_text))
      .arg(colorName(theme, Role::item_hover_background))
      .arg(colorName(theme, Role::item_hover_text));
}

QString headerStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QHeaderView{background-color:%1;color:%2;border-color:%3;}"
             "QHeaderView::section{background-color:%1;color:%2;}"
             "QHeaderView::section:hover{background-color:%4;color:%5;"
             "border-color:%3;}"
             "QHeaderView::section:checked,QHeaderView::section:selected{"
             "background-color:%6;color:%7;border-color:%3;}")
      .arg(colorName(theme, Role::header_background))
      .arg(colorName(theme, Role::header_text))
      .arg(colorName(theme, Role::header_outline))
      .arg(colorName(theme, Role::header_hover_background))
      .arg(colorName(theme, Role::header_hover_text))
      .arg(colorName(theme, Role::header_checked_background))
      .arg(colorName(theme, Role::header_checked_text));
}

QString scrollbarStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QScrollBar{background-color:%1;border-color:%2;}"
             "QScrollBar::handle{background-color:%3;border-color:%2;}"
             "QScrollBar::handle:hover{background-color:%4;border-color:%2;}"
             "QScrollBar::add-line,QScrollBar::sub-line{"
             "background-color:%1;border-color:%2;}"
             "QScrollBar::add-page,QScrollBar::sub-page{"
             "background-color:%5;border-color:%2;}"
             "QScrollBar#listPositionIndicator:vertical,"
             "QScrollBar#listPositionIndicator::add-line:vertical,"
             "QScrollBar#listPositionIndicator::sub-line:vertical,"
             "QScrollBar#listPositionIndicator::add-page:vertical,"
             "QScrollBar#listPositionIndicator::sub-page:vertical{"
             "background-color:transparent;border-color:transparent;}"
             "QScrollBar#listPositionIndicator::handle:vertical{"
             "background-color:%3;border-color:transparent;}"
             "QScrollBar#listPositionIndicator::handle:vertical:hover{"
             "background-color:%4;border-color:transparent;}")
      .arg(colorName(theme, Role::scrollbar_background))
      .arg(colorName(theme, Role::scrollbar_outline))
      .arg(colorName(theme, Role::scrollbar_handle))
      .arg(colorName(theme, Role::scrollbar_handle_hover_background))
      .arg(colorName(theme, Role::scrollbar_page_background));
}

QString splitterStyleSheet(const Theme &theme) {
  return QStringLiteral(
             "QSplitter{background-color:%1;}"
             "QSplitter::handle{background-color:%2;border-color:%3;}"
             "QSplitter::handle:hover{background-color:%4;border-color:%3;}")
      .arg(colorName(theme, Role::splitter_background))
      .arg(colorName(theme, Role::splitter_handle))
      .arg(colorName(theme, Role::splitter_outline))
      .arg(colorName(theme, Role::splitter_handle_hover_background));
}

// Theme colors are role-driven; geometry is cached independently by layout.
QString colorStyleSheet(const Theme &theme) {
  return windowStyleSheet(theme) + trackHeaderStyleSheet(theme) +
         toolbarStyleSheet(theme) + tabStyleSheet(theme) +
         buttonStyleSheet(theme) + trackControlStyleSheet(theme) +
         inputStyleSheet(theme) + spinBoxStyleSheet(theme) +
         comboStyleSheet(theme) + indicatorStyleSheet(theme) +
         menuBarStyleSheet(theme) + menuStyleSheet(theme) +
         tooltipStyleSheet(theme) + groupBoxStyleSheet(theme) +
         itemViewStyleSheet(theme) + headerStyleSheet(theme) +
         scrollbarStyleSheet(theme) + splitterStyleSheet(theme);
}

bool onlyGridLineColorChanged(const Theme &before, const Theme &after) {
  auto gridChanged = false;
  for (std::size_t index = 0; index < roleCount; ++index) {
    if (before.colors[index] == after.colors[index])
      continue;
    if (static_cast<Role>(index) != Role::song_view_grid)
      return false;
    gridChanged = true;
  }
  return gridChanged;
}

} // namespace

void initialize(QApplication &application) {
  if (!capturedStartupPalette)
    capturedStartupPalette = application.palette();
}

void registerGridLineRepaintTarget(QWidget &widget) {
  gridLineRepaintTargets.insert(&widget);
  auto *const target = &widget;
  QObject::connect(&widget, &QObject::destroyed,
                   [target] { gridLineRepaintTargets.remove(target); });
}

void apply(QApplication &application, const Theme &theme) {
  initialize(application);
  // Grid contrast affects only custom-painted Song Views. Reinstalling the
  // application palette and stylesheet here needlessly restyles native tab
  // bars while the slider is dragged.
  if (currentTheme && onlyGridLineColorChanged(*currentTheme, theme)) {
    currentTheme = theme;
    for (auto *widget : gridLineRepaintTargets)
      widget->update();
    return;
  }
  // Always resolve from the platform baseline, never from the previous theme;
  // otherwise roles not mapped into QPalette would accumulate stale values.
  auto palette = *capturedStartupPalette;
  applyPaletteGroup(palette, QPalette::Active, theme);
  applyPaletteGroup(palette, QPalette::Inactive, theme);
  applyPaletteGroup(palette, QPalette::Disabled, theme);
  currentTheme = theme;
  application.setPalette(palette);
  application.setStyleSheet(layout::composeStyleSheet(colorStyleSheet(theme)));
  // Installing the stylesheet rebuilds Qt's platform class-font table on
  // macOS. Reapply Body to clear those overrides from item views and headers.
  application.setFont(application.font());
}

const QColor &color(Role role) {
  Q_ASSERT(currentTheme);
  return currentTheme->color(role);
}

} // namespace themes
