#include "oklchpicker.h"
#include "themeresolver.h"

#include "ui/layout.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr double fullTurn = 360.0;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double minimumPairContrast = 3.0;
} // namespace

OklchPicker::OklchPicker(QWidget *parent) : QWidget(parent) {
  setMinimumSize(::layout::fontPx(27), ::layout::fontPx(18));
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMouseTracking(true);
  setFocusPolicy(Qt::NoFocus);
}

void OklchPicker::setSelection(const QColor &selected, const QColor &other,
                               bool hasOther) {
  const auto nextSelected =
      selected.isValid() ? selected : QColor(128, 128, 128);
  const auto nextOklch = themes::oklchFromColor(nextSelected);
  const auto nextHasOther = hasOther && other.isValid();
  // Plane pixels depend on hue and the comparison-color constraint; changing
  // only the selected lightness/chroma moves the marker over the same image.
  if (!qFuzzyCompare(m_oklch.hue + 1.0, nextOklch.hue + 1.0) ||
      m_other != other || m_hasOther != nextHasOther)
    m_planeImage = {};
  m_selected = nextSelected;
  m_other = other;
  m_hasOther = nextHasOther;
  m_oklch = nextOklch;
  update();
}

void OklchPicker::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.fillRect(rect(), palette().brush(QPalette::Base));

  const int hueWidth = 24;
  const int gap = 10;
  const QRect plane = QRect(8, 8, std::max(1, width() - hueWidth - gap - 16),
                            std::max(1, height() - 16));
  const QRect hueBar(plane.right() + gap, plane.top(), hueWidth, plane.height());

  ensurePlane(plane.size());
  painter.drawImage(plane.topLeft(), m_planeImage);

  // Close the cyclic gradient with hue zero. OKLCh uses [0, 360), so sampled
  // positions below stop just short of the otherwise equivalent 360 endpoint.
  QLinearGradient hueGradient(hueBar.topLeft(), hueBar.bottomLeft());
  for (int i = 0; i <= 12; ++i) {
    const double hue =
        i == 12 ? 0.0 : fullTurn * static_cast<double>(i) / 12.0;
    hueGradient.setColorAt(static_cast<double>(i) / 12.0,
                           inGamutHueColor(hue));
  }
  painter.fillRect(hueBar, hueGradient);
  painter.save();
  painter.setPen(Qt::NoPen);
  for (int y = 0; y < hueBar.height(); ++y) {
    themes::Oklch candidate{m_oklch.lightness, m_oklch.chroma,
                            (fullTurn - 1.0e-6) * static_cast<double>(y) /
                                std::max(1, hueBar.height() - 1)};
    if (!candidateAllowed(candidate))
      painter.fillRect(hueBar.left(), hueBar.top() + y, hueBar.width(), 1,
                       QColor(0, 0, 0, 120));
  }
  painter.restore();
  painter.setPen(QPen(palette().color(QPalette::Mid), 1));
  painter.drawRect(plane);
  painter.drawRect(hueBar);

  const double chroma = std::clamp(m_oklch.chroma, 0.0, maxChroma);
  const double lightness = std::clamp(m_oklch.lightness, 0.0, 1.0);
  const QPoint marker(
      plane.left() + qRound(chroma / maxChroma * (plane.width() - 1)),
      plane.top() + qRound((1.0 - lightness) * (plane.height() - 1)));
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(Qt::black, 2));
  painter.drawEllipse(marker, 6, 6);
  painter.setPen(QPen(Qt::white, 1));
  painter.drawEllipse(marker, 5, 5);

  const int hueY =
      hueBar.top() + qRound(std::fmod(std::max(0.0, m_oklch.hue), fullTurn) /
                            fullTurn * (hueBar.height() - 1));
  painter.setPen(QPen(Qt::black, 2));
  painter.drawLine(hueBar.left() - 2, hueY, hueBar.right() + 2, hueY);
  painter.setPen(QPen(Qt::white, 1));
  painter.drawLine(hueBar.left(), hueY, hueBar.right(), hueY);
}

void OklchPicker::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    m_dragging = true;
    chooseAt(event->position().toPoint());
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void OklchPicker::mouseMoveEvent(QMouseEvent *event) {
  if (m_dragging && (event->buttons() & Qt::LeftButton)) {
    chooseAt(event->position().toPoint());
    event->accept();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void OklchPicker::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    m_dragging = false;
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

QColor OklchPicker::inGamutHueColor(double hue) {
  for (auto chroma = 0.18; chroma >= 0.0; chroma -= 0.01) {
    const auto color =
        themes::colorFromOklch(themes::Oklch{0.7, chroma, hue});
    if (color.isValid())
      return color;
  }
  return QColor(128, 128, 128);
}

void OklchPicker::ensurePlane(const QSize &size) {
  if (!m_planeImage.isNull() && m_planeImage.size() == size)
    return;
  m_planeImage = QImage(size, QImage::Format_RGB32);
  const auto cosine = std::cos(m_oklch.hue * pi / 180.0);
  const auto sine = std::sin(m_oklch.hue * pi / 180.0);
  const auto otherOpaque = m_other.alpha() == 255;
  const auto otherLuminance = themes::relativeLuminance(m_other);
  for (int y = 0; y < m_planeImage.height(); ++y) {
    const auto lightness =
        1.0 - static_cast<double>(y) / std::max(1, m_planeImage.height() - 1);
    auto *pixels = reinterpret_cast<QRgb *>(m_planeImage.scanLine(y));
    for (int x = 0; x < m_planeImage.width(); ++x) {
      const auto chroma = 0.4 * static_cast<double>(x) /
                          std::max(1, m_planeImage.width() - 1);
      const auto alternate = ((x + y) / 4) % 2 == 0;
      themes::SrgbSample sample;
      const auto lab =
          themes::Oklab{lightness, chroma * cosine, chroma * sine};
      if (!themes::sampleSrgb(lab, sample)) {
        pixels[x] = alternate ? qRgb(96, 96, 96) : qRgb(176, 176, 176);
        continue;
      }
      if (m_hasOther &&
          (!otherOpaque || themes::contrastRatioFromLuminance(
                               themes::relativeLuminance(sample),
                               otherLuminance) < minimumPairContrast)) {
        pixels[x] = alternate ? qRgb(64, 64, 64) : qRgb(232, 232, 232);
        continue;
      }
      pixels[x] = qRgb(sample.red, sample.green, sample.blue);
    }
  }
}

QRect OklchPicker::planeRect() const {
  const int hueWidth = 24;
  const int gap = 10;
  return QRect(8, 8, std::max(1, width() - hueWidth - gap - 16),
               std::max(1, height() - 16));
}

QRect OklchPicker::hueRect() const {
  const QRect plane = planeRect();
  return QRect(plane.right() + 10, plane.top(), 24, plane.height());
}

QRect OklchPicker::selectionMarkerRect(const QRect &plane) const {
  const double chroma = std::clamp(m_oklch.chroma, 0.0, maxChroma);
  const double lightness = std::clamp(m_oklch.lightness, 0.0, 1.0);
  const QPoint marker(
      plane.left() + qRound(chroma / maxChroma * (plane.width() - 1)),
      plane.top() + qRound((1.0 - lightness) * (plane.height() - 1)));
  return QRect(marker - QPoint(8, 8), QSize(17, 17));
}

bool OklchPicker::candidateAllowed(const themes::Oklch &candidate,
                                   QColor *converted) const {
  const QColor color = themes::colorFromOklch(candidate);
  if (!color.isValid())
    return false;
  if (m_hasOther && !themes::isValidColorPair(color, m_other))
    return false;
  if (converted)
    *converted = color;
  return true;
}

void OklchPicker::chooseAt(const QPoint &point) {
  const QRect plane = planeRect();
  const QRect hue = hueRect();
  const QRect previousMarker = selectionMarkerRect(plane);
  themes::Oklch candidate = m_oklch;
  if (plane.contains(point)) {
    candidate.lightness =
        1.0 - std::clamp(static_cast<double>(point.y() - plane.top()) /
                             std::max(1, plane.height() - 1),
                         0.0, 1.0);
    candidate.chroma =
        0.4 * std::clamp(static_cast<double>(point.x() - plane.left()) /
                             std::max(1, plane.width() - 1),
                         0.0, 1.0);
  } else if (hue.contains(point)) {
    // Keep the bottom pixel inside the [0, 360) hue domain.
    candidate.hue = (fullTurn - 1.0e-6) *
                    std::clamp(static_cast<double>(point.y() - hue.top()) /
                                   std::max(1, hue.height() - 1),
                               0.0, 1.0);
  } else {
    return;
  }

  QColor color;
  if (!candidateAllowed(candidate, &color))
    return;
  const bool hueChanged =
      !qFuzzyCompare(m_oklch.hue + 1.0, candidate.hue + 1.0);
  m_oklch = candidate;
  m_selected = color;
  if (hueChanged) {
    m_planeImage = {};
    update();
  } else {
    update(previousMarker.united(selectionMarkerRect(plane))
               .adjusted(-2, -2, 2, 2));
  }
  emit colorSelected(color);
}
