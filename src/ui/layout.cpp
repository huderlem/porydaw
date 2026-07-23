#include "layout.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QLayout>
#include <QPainter>
#include <QPalette>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <optional>

namespace layout {
namespace {

// Concrete pixel dimensions derived once from typography's captured base font
// pixel size.
// The stylesheet builders consume these values so control geometry and spacing
// remain proportional to the platform-scaled application font.
struct FontScaledGeometry {
  int border;
  int zero;
  int half;
  int one;
  int two;
  int comboDropDownLane;
  int comboDropDownWidth;
  int listPositionIndicatorMinimumLength;
  int indicatorExtent;
  int groupBoxTitleBand;
};

// Space tokens use their enum ordinal as the lookup key:
// token -> font-relative multiplier -> startup-resolved pixel value. Keeping
// the resolved pixels in ResolvedLayout makes space() a hot-path array lookup.
constexpr auto SPACE_MULTIPLIERS =
    std::array{0.0, 0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
using ResolvedSpaces = std::array<int, SPACE_MULTIPLIERS.size()>;
// Pin every current public token to its positional multiplier. These checks
// fail if the existing enum order or table length changes independently.
static_assert(static_cast<std::size_t>(Space::Zero) == 0);
static_assert(static_cast<std::size_t>(Space::Half) == 1);
static_assert(static_cast<std::size_t>(Space::One) == 2);
static_assert(static_cast<std::size_t>(Space::Two) == 3);
static_assert(static_cast<std::size_t>(Space::Three) == 4);
static_assert(static_cast<std::size_t>(Space::Four) == 5);
static_assert(static_cast<std::size_t>(Space::Six) == 6);
static_assert(static_cast<std::size_t>(Space::Eight) + 1 ==
              SPACE_MULTIPLIERS.size());

// Qt has one application stylesheet and one application event-filter chain.
// Retaining the pair makes repeated startup harmless while rejecting a second
// scale or QApplication that would otherwise split process-wide layout state.
struct ResolvedLayout {
  QApplication *application;
  int baseFontPx;
  ResolvedSpaces spaces;
  QString geometry;
};

std::optional<ResolvedLayout> resolvedLayout;

int resolve(double baseFontPx, double multiplier) {
  Q_ASSERT(baseFontPx > 0.0);
  Q_ASSERT(multiplier >= 0.0);
  if (multiplier == 0.0)
    return 0;
  return qMax(1, qRound(baseFontPx * multiplier));
}

QString pixels(int value) {
  return QString::number(value) + QStringLiteral("px");
}

std::size_t spaceIndex(Space token) {
  const auto index = static_cast<std::size_t>(token);
  Q_ASSERT(index < SPACE_MULTIPLIERS.size());
  return index;
}

ResolvedSpaces resolveSpaces(int baseFontPx) {
  // Rounding and minimum-size clamping happen once during initialization, not
  // in the paint and widget-layout paths that repeatedly call space().
  auto spaces = ResolvedSpaces{};
  std::transform(
      SPACE_MULTIPLIERS.cbegin(), SPACE_MULTIPLIERS.cend(), spaces.begin(),
      [baseFontPx](double multiplier) {
        return resolve(baseFontPx, multiplier);
      });
  return spaces;
}

int resolvedSpace(const ResolvedSpaces &spaces, Space token) {
  return spaces[spaceIndex(token)];
}

FontScaledGeometry resolveGeometry(int baseFontPx,
                                   const ResolvedSpaces &spaces) {
  const auto comboDropDownLane = resolve(baseFontPx, 1.25);
  return {
      singlePixel(),
      resolvedSpace(spaces, Space::Zero),
      resolvedSpace(spaces, Space::Half),
      resolvedSpace(spaces, Space::One),
      resolvedSpace(spaces, Space::Two),
      comboDropDownLane,
      qMax(singlePixel(), comboDropDownLane - 2 * singlePixel()),
      resolvedSpace(spaces, Space::Eight),
      resolve(baseFontPx, 0.9),
      resolve(baseFontPx, 1.2),
  };
}

QString commonGeometryStyleSheet(const FontScaledGeometry &geometry) {
  return QStringLiteral(
             "QPushButton,QToolButton,QTabBar::tab,QAbstractSpinBox,"
             "QLineEdit,QTextEdit,QPlainTextEdit,QCheckBox,QRadioButton,"
             "QGroupBox,QMenu,QToolTip,QAbstractItemView,QScrollBar,"
             "QScrollBar::handle,QSplitter::handle{"
             "border:%1 solid transparent;border-radius:%2;}"
             "QHeaderView::section{border:0;}"
             "QPushButton{padding:%5 %6;}"
             "QMenu::item{padding-left:%3;padding-right:%3;}"
             "QMenu::item{padding-top:%4;padding-bottom:%4;}"
             "QCheckBox::indicator,QRadioButton::indicator,"
             "QAbstractSpinBox::up-button,QAbstractSpinBox::down-button{"
             "border:%1 solid transparent;border-radius:%2;}")
      .arg(pixels(geometry.border), pixels(geometry.zero), pixels(geometry.two),
           pixels(geometry.one), pixels(singlePixel()),
           pixels(geometry.two + singlePixel()));
}

QString comboBoxGeometryStyleSheet(const FontScaledGeometry &geometry) {
  // Popup width is measured from content and live Qt style metrics by the
  // private Layout popup filter; do not fix the dropdown width here.
  return QStringLiteral(
             "QComboBox{border:%1 solid transparent;border-radius:%5;"
             "padding:%3 %4 %3 %5;}"
             "QComboBox QLineEdit{background-color:transparent;"
             "border:%2;padding:%2;}"
             "QComboBox::drop-down{subcontrol-origin:border;"
             "subcontrol-position:top right;width:%6;"
             "border:%1 solid transparent;border-radius:%2;}"
             "QComboBox QAbstractItemView{border:%2;border-radius:%2;"
             "padding:%2;}"
             "QComboBox QAbstractItemView::item{padding:%3 %5;}")
      .arg(pixels(geometry.border), pixels(geometry.zero),
           pixels(geometry.half), pixels(geometry.comboDropDownLane),
           pixels(geometry.one), pixels(geometry.comboDropDownWidth));
}

QString tabGeometryStyleSheet(const FontScaledGeometry &geometry) {
  // Vertical tab metrics must stay in step with chromeRowHeight(); only the
  // horizontal padding is free to breathe.
  return QStringLiteral("QTabBar::tab{margin-top:%1;padding:%1 %2;}")
      .arg(pixels(geometry.half), pixels(geometry.two));
}

// The stylesheet renderer drops the native style's built-in breathing room
// and subcontrol art the moment a widget family is themed. These rules give
// every styled family back font-derived padding, correctly shaped check and
// radio indicators, a reserved group-box title band, and a full spin-button
// lane (whose arrow glyphs the theme sheet supplies as generated images).
QString comfortGeometryStyleSheet(const FontScaledGeometry &geometry) {
  auto sheet =
      QStringLiteral(
          "QMenuBar::item{padding:%1 %2;}"
          "QMenu{padding:%3 0px;}"
          "QMenu::separator{height:%4;margin:%3 %2;}"
          "QToolTip{padding:%3 %1;}"
          "QLineEdit,QTextEdit,QPlainTextEdit,QAbstractSpinBox{padding:%3 %1;}"
          "QHeaderView::section{padding:%3 %1;}")
          .arg(pixels(geometry.one), pixels(geometry.two),
               pixels(geometry.half), pixels(geometry.border));
  sheet +=
      QStringLiteral(
          "QCheckBox,QRadioButton{spacing:%1;}"
          "QCheckBox::indicator,QRadioButton::indicator{width:%2;height:%2;}"
          "QRadioButton::indicator{border-radius:%3;}")
          .arg(pixels(geometry.one), pixels(geometry.indicatorExtent),
               pixels((geometry.indicatorExtent + 2 * geometry.border) / 2));
  // Checkable menu items share the checkbox indicator; every item indents
  // past the indicator column so labels align whether checkable or not.
  sheet += QStringLiteral(
               "QMenu::item{padding-left:%1;}"
               "QMenu::indicator{width:%2;height:%2;left:%3;"
               "subcontrol-origin:border;subcontrol-position:left center;"
               "border:%4 solid transparent;}")
               .arg(pixels(geometry.indicatorExtent + 2 * geometry.one +
                           2 * geometry.border),
                    pixels(geometry.indicatorExtent), pixels(geometry.one),
                    pixels(geometry.border));
  sheet += QStringLiteral(
               "QGroupBox{margin-top:%1;padding-top:%2;}"
               "QGroupBox::title{subcontrol-origin:margin;"
               "subcontrol-position:top left;left:%3;padding:0 %2;}")
               .arg(pixels(geometry.groupBoxTitleBand), pixels(geometry.half),
                    pixels(geometry.one));
  // No ::up-arrow/::down-arrow rules on purpose: with none present the
  // stylesheet renderer draws the base style's arrow primitive in the
  // theme's foreground, exactly like the combo drop-down arrow.
  sheet += QStringLiteral(
               "QAbstractSpinBox{padding-right:%1;}"
               "QAbstractSpinBox::up-button{subcontrol-origin:border;"
               "subcontrol-position:top right;width:%1;}"
               "QAbstractSpinBox::down-button{subcontrol-origin:border;"
               "subcontrol-position:bottom right;width:%1;}")
               .arg(pixels(geometry.comboDropDownLane));
  return sheet;
}

QString
listPositionIndicatorGeometryStyleSheet(const FontScaledGeometry &geometry) {
  return QStringLiteral(
             "QScrollBar#listPositionIndicator:vertical{width:%1;margin:%2;"
             "padding:%2;border-width:%2;}"
             "QScrollBar#listPositionIndicator::handle:vertical{"
             "min-height:%3;border-width:%2;}"
             "QScrollBar#listPositionIndicator::add-line:vertical,"
             "QScrollBar#listPositionIndicator::sub-line:vertical{"
             "height:%2;border-width:%2;}"
             "QScrollBar#listPositionIndicator::up-arrow:vertical,"
             "QScrollBar#listPositionIndicator::down-arrow:vertical{"
             "width:%2;height:%2;}")
      .arg(pixels(geometry.two), pixels(geometry.zero),
           pixels(geometry.listPositionIndicatorMinimumLength));
}

QString toolbarGeometryStyleSheet(const FontScaledGeometry &geometry) {
  // A real (transparent at rest) border lets the theme sheet surface the
  // pressed-state outline; the zero radius keeps the buttons flat.
  return QStringLiteral("QToolBar#transportToolbar QToolButton{"
                        "border:%1 solid transparent;border-radius:%2;"
                        "padding:%3;}")
      .arg(pixels(geometry.border), pixels(geometry.zero),
           pixels(geometry.half));
}

QString buildGeometryStyleSheet(int baseFontPx,
                                const ResolvedSpaces &spaces) {
  const auto geometry = resolveGeometry(baseFontPx, spaces);
  return commonGeometryStyleSheet(geometry) +
         comboBoxGeometryStyleSheet(geometry) +
         tabGeometryStyleSheet(geometry) +
         comfortGeometryStyleSheet(geometry) +
         listPositionIndicatorGeometryStyleSheet(geometry) +
         toolbarGeometryStyleSheet(geometry);
}

QComboBox *comboForPopup(QAbstractItemView &view) {
  for (auto *parent = view.parent(); parent; parent = parent->parent()) {
    if (auto *combo = qobject_cast<QComboBox *>(parent))
      return combo;
  }
  return nullptr;
}

// Begin at the closed-control width so opening a popup may only make the popup
// wider. Model and style metrics stay live because either may change at
// runtime.
int popupContentWidth(const QComboBox &combo) {
  const auto *view = combo.view();
  const auto *model = view->model();
  auto width = combo.width();
  for (auto row = 0; row < model->rowCount(view->rootIndex()); ++row) {
    const auto index = model->index(row, 0, view->rootIndex());
    width = std::max(width, view->sizeHintForIndex(index).width());
  }
  const auto *style = combo.style();
  const auto checkmarkWidth =
      style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, &combo);
  const auto focusMargin =
      style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, &combo);
  const auto margins = view->contentsMargins();
  return width + checkmarkWidth + 2 * focusMargin + margins.left() +
         margins.right() + 2 * view->frameWidth();
}

// Qt styles wrap ComboBox views in platform-specific popup frames. Flatten each
// layer here so Layout's item padding is not compounded by native containers.
void flattenPopup(QComboBox &combo) {
  auto *view = combo.view();
  view->setFrameShape(QFrame::NoFrame);
  view->setContentsMargins(0, 0, 0, 0);

  auto *popup = view->window();
  if (!popup || popup == combo.window())
    return;
  if (auto *frame = qobject_cast<QFrame *>(popup)) {
    frame->setFrameShape(QFrame::NoFrame);
    frame->setLineWidth(0);
    frame->setMidLineWidth(0);
  }
  popup->setContentsMargins(0, 0, 0, 0);
  if (auto *layout = popup->layout()) {
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
  }
  auto palette = popup->palette();
  palette.setColor(QPalette::Window, view->palette().color(QPalette::Base));
  popup->setPalette(palette);
  popup->setAutoFillBackground(true);
}

// Set minimum widths only on the view/window; resizing the QComboBox here would
// make opening the popup permanently alter its closed layout.
void resizePopup(QComboBox &combo) {
  auto *view = combo.view();
  const auto contentWidth = popupContentWidth(combo);
  view->setMinimumWidth(contentWidth);
  auto *popup = view->window();
  if (!popup || popup == combo.window())
    return;
  const auto margins = popup->contentsMargins();
  const auto popupWidth = contentWidth + margins.left() + margins.right();
  popup->setMinimumWidth(popupWidth);
  if (popup->width() < popupWidth)
    popup->resize(popupWidth, popup->height());
}

// Some styles add visible popup siblings such as scrollers. Only fill the whole
// container when the item view is the sole visible popup content.
void fillPopupContainer(QComboBox &combo) {
  auto *view = combo.view();
  auto *popup = view->window();
  if (!popup || popup == combo.window())
    return;
  for (auto *child : popup->children()) {
    auto *sibling = qobject_cast<QWidget *>(child);
    if (sibling && sibling != view && sibling->isVisible())
      return;
  }
  if (auto *layout = popup->layout())
    layout->activate();
  view->setGeometry(popup->contentsRect());
}

void paintComboBox(QComboBox &combo) {
  QStyleOptionComboBox option;
  option.initFrom(&combo);
  option.editable = combo.isEditable();
  option.frame = combo.hasFrame();
  option.currentIcon = combo.itemIcon(combo.currentIndex());
  option.currentText = combo.currentText();
  option.iconSize = combo.iconSize();
  option.subControls = QStyle::SC_All;
  const auto arrowRect = combo.style()->subControlRect(
      QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, &combo);
  const auto arrowHovered =
      option.state & QStyle::State_MouseOver &&
      arrowRect.contains(combo.mapFromGlobal(QCursor::pos()));
  if (arrowHovered)
    option.activeSubControls = QStyle::SC_ComboBoxArrow;
  if (combo.view()->isVisible()) {
    option.activeSubControls = QStyle::SC_ComboBoxArrow;
    option.state |= QStyle::State_On;
  }
  QPainter painter(&combo);
  combo.style()->drawComplexControl(QStyle::CC_ComboBox, &option, &painter,
                                    &combo);
  if (!combo.isEditable())
    combo.style()->drawControl(QStyle::CE_ComboBoxLabel, &option, &painter,
                               &combo);
  const auto center = arrowRect.center() + QPoint(1, 0);
  const auto halfWidth = qMax(2, arrowRect.width() / 4);
  const auto halfHeight = qMax(1, halfWidth / 2);
  const QPoint points[] = {
      {center.x() - halfWidth, center.y() - halfHeight},
      {center.x() + halfWidth, center.y() - halfHeight},
      {center.x(), center.y() + halfHeight},
  };
  const auto colorGroup =
      combo.isEnabled() ? QPalette::Active : QPalette::Disabled;
  painter.setPen(Qt::NoPen);
  painter.setBrush(combo.palette().color(colorGroup, QPalette::ButtonText));
  painter.drawPolygon(points, 3);
}

// QEvent::Show is late enough for the popup's live model, style, and wrapper
// window to exist. Always return false so Qt still performs the actual show.
class PopupSizingFilter final : public QObject {
public:
  explicit PopupSizingFilter(QObject *parent) : QObject(parent) {}

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::Paint) {
      if (auto *combo = qobject_cast<QComboBox *>(watched)) {
        paintComboBox(*combo);
        return true;
      }
      return false;
    }
    if (event->type() != QEvent::Show)
      return false;
    auto *view = qobject_cast<QAbstractItemView *>(watched);
    if (!view)
      return false;
    if (auto *combo = comboForPopup(*view)) {
      flattenPopup(*combo);
      resizePopup(*combo);
      fillPopupContainer(*combo);
    }
    return false;
  }
};

const ResolvedLayout &currentLayout() {
  Q_ASSERT(resolvedLayout);
  return *resolvedLayout;
}

} // namespace

// Validate before the idempotence check: initialize(application, 0) must never
// report success merely because a valid layout was established earlier.
bool initialize(QApplication &application, int baseFontPx) {
  if (baseFontPx <= 0)
    return false;
  if (resolvedLayout) {
    const auto &resolved = *resolvedLayout;
    return resolved.application == &application &&
           resolved.baseFontPx == baseFontPx;
  }
  // Resolve the complete spacing scale before publishing process-wide state.
  // The stylesheet and later space() calls then consume the same pixel values.
  const auto spaces = resolveSpaces(baseFontPx);
  resolvedLayout =
      ResolvedLayout{&application, baseFontPx, spaces,
                     buildGeometryStyleSheet(baseFontPx, spaces)};
  application.setStyleSheet(resolvedLayout->geometry);
  // QApplication owns the filter. The initialization guard above ensures only
  // one filter is installed for the process-wide Layout state.
  application.installEventFilter(new PopupSizingFilter(&application));
  return true;
}

int space(Space token) {
  const auto &resolved = currentLayout();
  return resolvedSpace(resolved.spaces, token);
}

int fontPx(double multiplier) {
  return resolve(currentLayout().baseFontPx, multiplier);
}

int singlePixel() { return 1; }
TwoLineTextLayout::TwoLineTextLayout(const QFont &primary,
                                     const QFont &alternatePrimary,
                                     const QFont &secondary, Space gap)
    : m_primaryHeight(qMax(QFontMetrics(primary).lineSpacing(),
                           QFontMetrics(alternatePrimary).lineSpacing())),
      m_secondaryHeight(QFontMetrics(secondary).lineSpacing()),
      m_gap(space(gap)) {}

int TwoLineTextLayout::height() const {
  return m_primaryHeight + m_gap + m_secondaryHeight;
}

TwoLineTextBoxes
TwoLineTextLayout::align(const QRect &bounds,
                         VerticalAlignment verticalAlignment) const {
  auto top = bounds.top();
  switch (verticalAlignment) {
  case VerticalAlignment::Top:
    break;
  case VerticalAlignment::Center:
    top += (bounds.height() - height()) / 2;
    break;
  case VerticalAlignment::Bottom:
    top += bounds.height() - height();
    break;
  }

  return {
      QRect(bounds.left(), top, bounds.width(), m_primaryHeight),
      QRect(bounds.left(), top + m_primaryHeight + m_gap, bounds.width(),
            m_secondaryHeight),
  };
}

TwoLineTextLayout twoLineText(const QFont &primary,
                              const QFont &alternatePrimary,
                              const QFont &secondary, Space gap) {
  return TwoLineTextLayout(primary, alternatePrimary, secondary, gap);
}

int chromeRowHeight(const QFont &applicationFont, int iconExtent) {
  return qMax(QFontMetrics(applicationFont).lineSpacing(), iconExtent) +
         2 * space(Space::Half) + 2 * singlePixel();
}

QString composeStyleSheet(const QString &colorStyleSheet) {
  return currentLayout().geometry + colorStyleSheet;
}
// For a consistent scrollbar everywhere.
void configureListPositionIndicator(QScrollBar &scrollBar) {
  scrollBar.setObjectName(QStringLiteral("listPositionIndicator"));
  scrollBar.style()->unpolish(&scrollBar);
  scrollBar.style()->polish(&scrollBar);
}

} // namespace layout
