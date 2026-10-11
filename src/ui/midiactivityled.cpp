#include "midiactivityled.h"

#include "layout.h"
#include "theme/themeruntime.h"

#include <QPainter>
#include <QTimer>

MidiActivityLed::MidiActivityLed(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_off = new QTimer(this);
    m_off->setSingleShot(true);
    // The timer alone turns the light off. Comparing a clock against the
    // flash length at paint time left it stuck on whenever the (coarse)
    // timer fired a little early: the repaint still read "lit", and nothing
    // repainted again until the next message.
    connect(m_off, &QTimer::timeout, this, [this] {
        m_lit = false;
        update();
    });
}

void MidiActivityLed::flash()
{
    m_lit = true;
    m_off->start(kFlashMs);
    update();
}

bool MidiActivityLed::isLit() const
{
    return m_lit;
}

void MidiActivityLed::setListening(bool listening)
{
    if (m_listening == listening)
        return;
    m_listening = listening;
    update();
}

QSize MidiActivityLed::sizeHint() const
{
    const int side = fontMetrics().height();
    return {side + 2 * ::layout::space(::layout::Space::Two), side};
}

void MidiActivityLed::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal diameter = qMax<qreal>(4.0, fontMetrics().height() * 0.5);
    const QRectF dot(QPointF((width() - diameter) / 2.0, (height() - diameter) / 2.0),
                     QSizeF(diameter, diameter));
    QColor ink = themes::color(themes::Role::transport_text);
    if (isLit()) {
        // The Polyphony panel's sounding-channel color: lit means "input".
        painter.setPen(Qt::NoPen);
        painter.setBrush(themes::color(themes::Role::polyphony_cell_active_background));
        painter.drawEllipse(dot);
    } else if (m_listening) {
        ink.setAlphaF(0.7f);
        painter.setPen(QPen(ink, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(dot.adjusted(0.75, 0.75, -0.75, -0.75));
    } else {
        ink.setAlphaF(0.25f);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(dot);
    }
}
