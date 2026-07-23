#pragma once

#include "color_math.h"

#include <QColor>
#include <QImage>
#include <QWidget>

class QMouseEvent;
class QPaintEvent;

class OklchPicker final : public QWidget {
  Q_OBJECT

public:
  explicit OklchPicker(QWidget *parent = nullptr);

  void setSelection(const QColor &selected, const QColor &other, bool hasOther);

signals:
  void colorSelected(const QColor &color);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  static constexpr double maxChroma = 0.4;

  static QColor inGamutHueColor(double hue);
  void ensurePlane(const QSize &size);
  QRect planeRect() const;
  QRect hueRect() const;
  QRect selectionMarkerRect(const QRect &plane) const;
  bool candidateAllowed(const themes::Oklch &candidate,
                        QColor *converted = nullptr) const;
  void chooseAt(const QPoint &point);

  QColor m_selected;
  QColor m_other;
  themes::Oklch m_oklch{0.5, 0.0, 0.0};
  QImage m_planeImage;
  bool m_hasOther = false;
  bool m_dragging = false;
};
