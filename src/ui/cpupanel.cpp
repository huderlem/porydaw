#include "ui/cpupanel.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QEvent>
#include <QFontInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "ui/theme/themeruntime.h"
#include "ui/typography.h"

namespace {

using themes::Role;

const Role kTermFill[3] = {Role::cpu_meter_sequencer_fill, Role::cpu_meter_cgb_fill,
                           Role::cpu_meter_mixer_fill};
// The table's rows, largest term first as a rule: the mixer.
const int kRowTerm[3] = {AudioEngine::CpuMixer, AudioEngine::CpuSequencer, AudioEngine::CpuCgb};

QString panelTr(const char *text)
{
    return QCoreApplication::translate("CpuPanel", text);
}

QColor withAlpha(QColor c, double alpha)
{
    c.setAlphaF(float(std::clamp(alpha, 0.0, 1.0)));
    return c;
}

// A level's ink (CpuPanel::level): plain, amber, red.
QColor levelInk(int level)
{
    switch (level) {
    case 2:
        return themes::color(Role::cpu_meter_over_text);
    case 1:
        return themes::color(Role::cpu_meter_warn_text);
    default:
        return themes::color(Role::window_text);
    }
}

QColor secondaryInk()
{
    return themes::color(Role::secondary_text);
}

void setInk(QLabel *label, const QColor &color)
{
    QPalette pal = label->palette();
    if (pal.color(QPalette::WindowText) == color &&
        pal.isBrushSet(QPalette::Active, QPalette::WindowText))
        return;
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        pal.setColor(group, QPalette::WindowText, color);
    label->setPalette(pal);
}

int basePx(const QFont &font)
{
    return std::max(8, QFontInfo(font).pixelSize());
}

// The fixed-pitch face (the status bar's), scaled from the body size.
QFont monoFont(const QFont &base, double scale)
{
    QFont f = typography::bodyMono(base);
    f.setPixelSize(std::max(8, int(std::lround(basePx(base) * scale))));
    return f;
}

QString cyclesString(int64_t n)
{
    return QLocale().toString(qlonglong(n));
}

QString percentString(int64_t cycles, int32_t vblank)
{
    const double pct = vblank > 0 ? double(cycles) * 100.0 / double(vblank) : 0.0;
    return QStringLiteral("%1%").arg(std::lround(pct));
}

bool hasFrame(const CpuPanel::Reading &r)
{
    return r.loaded && r.playing && r.cpu.valid;
}

bool hasPeak(const CpuPanel::Reading &r)
{
    return r.loaded && r.cpu.peakValid;
}

double scaleCycles(const CpuPanel::Reading &r)
{
    return double(std::max<int32_t>(1, r.cpu.vblankCycles)) * CpuPanel::kScalePercent / 100.0;
}

// The section dividers.
class CpuRule : public QWidget
{
  public:
    explicit CpuRule(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(1);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), themes::color(Role::palette_outline));
    }
};

// A small square (a term's swatch) or a short line (a legend mark).
class CpuSwatch : public QWidget
{
  public:
    CpuSwatch(Role role, bool line, QWidget *parent) : QWidget(parent), m_role(role), m_line(line)
    {
        setFixedSize(10, 10);
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(themes::color(m_role));
        if (m_line)
            p.drawRect(QRectF(0, height() / 2.0 - 1.0, width(), 2.0));
        else
            p.drawRoundedRect(rect(), 3, 3);
    }

  private:
    Role m_role;
    bool m_line;
};

// The raised box behind the peak.
class CpuCard : public QWidget
{
  public:
    explicit CpuCard(QWidget *parent) : QWidget(parent) {}

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(themes::color(Role::cpu_meter_card_background));
        p.drawRoundedRect(QRectF(rect()), 8, 8);
    }
};

} // namespace

// The status pill: a dot and a word on a tint of the same ink.
class CpuStatusPill : public QWidget
{
  public:
    explicit CpuStatusPill(QWidget *parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setState(const QString &text, const QColor &ink)
    {
        if (text == m_text && ink == m_ink)
            return;
        m_text = text;
        m_ink = ink;
        updateGeometry();
        update();
    }
    const QString &text() const { return m_text; }
    const QColor &ink() const { return m_ink; }

    QSize sizeHint() const override
    {
        const QFontMetrics fm(font());
        return QSize(kPadX * 2 + kDot + kGap + fm.horizontalAdvance(m_text), fm.height() + 8);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(m_ink, 0.16));
        const QRectF r = QRectF(rect());
        p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
        p.setBrush(m_ink);
        p.drawEllipse(QRectF(kPadX, r.height() / 2.0 - kDot / 2.0, kDot, kDot));
        p.setPen(m_ink);
        p.drawText(QRectF(kPadX + kDot + kGap, 0, r.width() - kPadX * 2 - kDot - kGap, r.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_text);
    }

  private:
    static constexpr int kPadX = 9;
    static constexpr int kDot = 6;
    static constexpr int kGap = 6;
    QString m_text;
    QColor m_ink;
};

// The meter: one linear scale from 0 to 140 % of the VBlank window. The
// track carries a faint amber band from the budget to the VBlank line and a
// hatched red one past it; the current frame's three terms stacked from the
// left (clamped at the scale's end, with a chevron when past it); the budget
// (amber) and VBlank (red) lines a little taller than the track; the peak a
// dashed line with its percent above. Captions under the track: 0, the
// budget (right of it ending at its line), the VBlank end (starting at its
// line), the scale's end.
class CpuMeter : public QWidget
{
  public:
    explicit CpuMeter(QWidget *parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(160);
    }

    void setReading(const CpuPanel::Reading &r)
    {
        m_reading = r;
        relayout();
        update();
    }

    QFont captionFont() const { return typography::caption(font()); }
    QFont captionBold() const { return typography::bold(captionFont()); }
    QFont captionMono() const { return typography::bodyMono(captionFont()); }

    int peakRowHeight() const { return QFontMetrics(captionFont()).height() + 4; }

    QRect track() const
    {
        return QRect(0, peakRowHeight() + kAbove, std::max(40, width()), kTrackH);
    }

    int xFor(int64_t cycles) const
    {
        const QRect t = track();
        const int left = t.left() + kInset;
        const int w = t.width() - 2 * kInset;
        const double c = std::clamp(double(cycles), 0.0, scaleCycles(m_reading));
        return left + int(std::lround(c / scaleCycles(m_reading) * w));
    }

    int budgetX() const { return xFor(m_reading.budgetCycles); }
    int vblankX() const { return xFor(m_reading.cpu.vblankCycles); }
    bool peakShown() const { return hasPeak(m_reading); }
    int peakX() const { return peakShown() ? xFor(m_reading.cpu.peak[AudioEngine::CpuTotal]) : -1; }
    bool overflow() const
    {
        return hasFrame(m_reading) &&
               m_reading.cpu.last[AudioEngine::CpuTotal] > scaleCycles(m_reading);
    }

    // Cumulative edges in the table's order (mixer, sequencer, CGB from the
    // left), so the segments always add up to the total's width.
    QRect segment(int term) const
    {
        if (!hasFrame(m_reading) || term < 0 || term > 2)
            return QRect();
        const AudioEngine::CpuSnapshot &cpu = m_reading.cpu;
        int64_t from = 0;
        for (int k = 0; k < 3 && kRowTerm[k] != term; k++)
            from += cpu.last[kRowTerm[k]];
        const int64_t to = from + cpu.last[term];
        const QRect t = track();
        const int x0 = xFor(from), x1 = xFor(to);
        return QRect(x0, t.top() + kInset, x1 - x0, t.height() - 2 * kInset);
    }

    QString peakLabel() const
    {
        return peakShown()
                   ? panelTr("Peak %1").arg(percentString(m_reading.cpu.peak[AudioEngine::CpuTotal],
                                                          m_reading.cpu.vblankCycles))
                   : QString();
    }
    QRect peakLabelRect() const
    {
        if (!peakShown())
            return QRect();
        const QFontMetrics fm(captionFont());
        const int w = fm.horizontalAdvance(peakLabel());
        const int x = std::clamp(peakX() - w / 2, 0, std::max(0, width() - w));
        return QRect(x, 0, w, fm.height());
    }

    // A caption block: a title over a number (or one line for the scale's
    // ends).
    struct Block {
        QRect rect;
        QString title, number;
        int kind = 0; // 0 scale end, 1 budget, 2 VBlank
    };
    const QList<Block> &blocks() const { return m_blocks; }

    QList<CpuPanel::Caption> captions() const
    {
        QList<CpuPanel::Caption> out;
        if (peakShown())
            out.append({peakLabelRect(), peakLabel()});
        for (const Block &b : m_blocks)
            out.append(
                {b.rect, b.number.isEmpty() ? b.title : b.title + QLatin1Char(' ') + b.number});
        return out;
    }

    QSize sizeHint() const override { return QSize(300, m_height); }
    QSize minimumSizeHint() const override { return QSize(160, m_height); }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        relayout();
    }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange)
            relayout();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect t = track();
        const QColor amber = themes::color(Role::cpu_meter_warn_text);
        const QColor red = themes::color(Role::cpu_meter_over_text);
        const int bx = budgetX(), vx = vblankX();

        QPainterPath trackPath;
        trackPath.addRoundedRect(QRectF(t).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.setPen(QPen(themes::color(Role::palette_outline), 1.0));
        p.setBrush(themes::color(Role::cpu_meter_track_background));
        p.drawPath(trackPath);
        p.save();
        p.setClipPath(trackPath);
        p.setRenderHint(QPainter::Antialiasing, false);
        // Budget to VBlank: a faint amber band.
        if (bx < vx)
            p.fillRect(QRect(bx, t.top(), vx - bx, t.height()), withAlpha(amber, 0.14));
        // Past VBlank: hatched red to the track's end.
        const QRect past(vx, t.top(), t.right() - vx + 1, t.height());
        p.fillRect(past, withAlpha(red, 0.07));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setClipRect(past, Qt::IntersectClip);
        p.setPen(QPen(withAlpha(red, 0.20), 3.0));
        for (int x = past.left() - t.height(); x < past.right() + t.height(); x += 9)
            p.drawLine(QPointF(x, t.bottom() + 2), QPointF(x + t.height() + 4, t.top() - 2));
        p.restore();

        // The frame's terms, a hairline of track between neighbours.
        p.setRenderHint(QPainter::Antialiasing, false);
        int lastTerm = -1;
        for (const int term : kRowTerm)
            if (segment(term).width() > 0)
                lastTerm = term;
        for (const int term : kRowTerm) {
            QRect s = segment(term);
            if (s.width() <= 0)
                continue;
            if (term != lastTerm && s.width() > 2)
                s.setRight(s.right() - 1);
            p.fillRect(s, themes::color(kTermFill[term]));
        }
        if (overflow()) {
            // A chevron at the scale's end: the frame goes on past it.
            const int right = xFor(int64_t(scaleCycles(m_reading))) - 3;
            const int cy = t.center().y();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(themes::color(Role::polyphony_cell_text), 2.0, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            for (int k = 0; k < 2; k++) {
                const int x = right - k * 5;
                p.drawPolyline(
                    QPolygon({QPoint(x - 4, cy - 5), QPoint(x, cy), QPoint(x - 4, cy + 5)}));
            }
            p.setRenderHint(QPainter::Antialiasing, false);
        }

        // The budget, then the VBlank line over it (at a budget of the
        // VBlank window's cycles they coincide; VBlank stays visible).
        p.fillRect(QRect(bx - 1, t.top() - kAbove + 3, 2, t.height() + 6), amber);
        p.fillRect(QRect(vx - 1, t.top() - kAbove + 3, 2, t.height() + 6), red);
        if (peakShown()) {
            QPen dash(themes::color(Role::window_text), 1.0, Qt::DashLine);
            dash.setDashPattern({3.0, 2.0});
            p.setPen(dash);
            const int px = peakX();
            p.drawLine(px, t.top() - kAbove, px, t.bottom());
            p.setFont(captionFont());
            p.setPen(themes::color(Role::window_text));
            p.drawText(peakLabelRect(), Qt::AlignCenter, peakLabel());
        }

        const QFontMetrics fm(captionFont());
        for (const Block &b : m_blocks) {
            const Qt::Alignment align =
                b.kind == 1 ? (Qt::AlignRight | Qt::AlignTop) : (Qt::AlignLeft | Qt::AlignTop);
            if (b.kind == 0) {
                p.setFont(captionFont());
                p.setPen(secondaryInk());
                p.drawText(b.rect, Qt::AlignLeft | Qt::AlignTop, b.title);
                continue;
            }
            p.setFont(captionBold());
            p.setPen(b.kind == 1 ? amber : red);
            p.drawText(QRect(b.rect.left(), b.rect.top(), b.rect.width(), fm.height()), align,
                       b.title);
            p.setFont(captionMono());
            p.setPen(secondaryInk());
            p.drawText(
                QRect(b.rect.left(), b.rect.top() + fm.height(), b.rect.width(), fm.height()),
                align, b.number);
        }
    }

  private:
    static constexpr int kTrackH = 28;
    static constexpr int kInset = 4; // the scale's 0 and end inside the track
    static constexpr int kAbove = 6; // the peak line's reach above the track
    static constexpr int kGap = 6;   // a caption's distance from its line

    // Places the captions: never overlapping, the budget's dropped below
    // the others (a second row) when it has no room beside them, the scale
    // ends' dropped when only they are in the way.
    void relayout()
    {
        m_blocks.clear();
        const QFontMetrics fm(captionFont()), fb(captionBold()), fmono(captionMono());
        const int lineH = fm.height();
        const int top = track().bottom() + 1 + kAbove + 2;
        const int blockH = 2 * lineH;
        const int w = width();
        const auto blockWidth = [&](const QString &title, const QString &number) {
            return std::max(fb.horizontalAdvance(title), fmono.horizontalAdvance(number));
        };
        Block zero{QRect(0, top, fm.horizontalAdvance(QStringLiteral("0")), lineH),
                   QStringLiteral("0"), QString(), 0};
        const QString endText = QStringLiteral("%1%").arg(CpuPanel::kScalePercent);
        const int ew = fm.horizontalAdvance(endText);
        Block end{QRect(w - ew, top, ew, lineH), endText, QString(), 0};
        const QString vbTitle = panelTr("VBlank end");
        const QString vbNumber = cyclesString(m_reading.cpu.vblankCycles);
        const int vw = blockWidth(vbTitle, vbNumber);
        Block vblank{QRect(std::min(vblankX() + kGap, std::max(0, w - vw)), top, vw, blockH),
                     vbTitle, vbNumber, 2};
        bool keepZero = true;
        bool keepEnd = !vblank.rect.adjusted(-kGap, 0, kGap, 0).intersects(end.rect);

        const QString buTitle = panelTr("Budget");
        const QString buNumber = cyclesString(m_reading.budgetCycles);
        const int bw = blockWidth(buTitle, buNumber);
        const int bx = budgetX();
        const int stackedTop = top + blockH + 2;
        const QRect candidates[4] = {QRect(bx - kGap - bw, top, bw, blockH),
                                     QRect(bx + kGap, top, bw, blockH),
                                     QRect(bx - kGap - bw, stackedTop, bw, blockH),
                                     QRect(bx + kGap, stackedTop, bw, blockH)};
        QRect budget;
        QRect fallback;
        bool fallbackDropsZero = false, fallbackDropsEnd = false;
        for (const QRect &c : candidates) {
            if (c.left() < 0 || c.right() >= w)
                continue;
            const QRect padded = c.adjusted(-kGap / 2, 0, kGap / 2, 0);
            const bool hitsVblank = padded.intersects(vblank.rect);
            const bool hitsZero = padded.intersects(zero.rect);
            const bool hitsEnd = keepEnd && padded.intersects(end.rect);
            if (!hitsVblank && !hitsZero && !hitsEnd) {
                budget = c;
                break;
            }
            if (!hitsVblank && fallback.isNull()) {
                fallback = c;
                fallbackDropsZero = hitsZero;
                fallbackDropsEnd = hitsEnd;
            }
        }
        if (budget.isNull() && !fallback.isNull()) {
            budget = fallback;
            keepZero = !fallbackDropsZero;
            keepEnd = keepEnd && !fallbackDropsEnd;
        }
        if (budget.isNull()) // no room on either side: under the others, inside the meter
            budget =
                QRect(std::clamp(bx - kGap - bw, 0, std::max(0, w - bw)), stackedTop, bw, blockH);

        if (keepZero)
            m_blocks.append(zero);
        m_blocks.append(Block{budget, buTitle, buNumber, 1});
        m_blocks.append(vblank);
        if (keepEnd)
            m_blocks.append(end);
        int bottom = top + blockH;
        for (const Block &b : m_blocks)
            bottom = std::max(bottom, b.rect.bottom() + 1);
        const int h = bottom + 2;
        if (h != m_height) {
            m_height = h;
            setFixedHeight(h);
        }
    }

    CpuPanel::Reading m_reading;
    QList<Block> m_blocks;
    int m_height = 90;
};

// The last ten seconds of playback, frame by frame: a filled line on the
// meter's scale (0 to 140 % of the VBlank window), newest at the right,
// with the VBlank line (solid red) and the budget (dashed amber); the line
// and its fill take the band they are in. Stopped, the last run dimmed.
class CpuHistoryChart : public QWidget
{
  public:
    explicit CpuHistoryChart(QWidget *parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(104);
        setMinimumWidth(160);
    }

    void setReading(const CpuPanel::Reading &r)
    {
        m_reading = r;
        update();
    }

    QRectF plot() const { return QRectF(rect()).adjusted(1, 1, -1, -1); }

    double yFor(double cycles) const
    {
        const QRectF pl = plot();
        const double f = std::clamp(cycles / scaleCycles(m_reading), 0.0, 1.0);
        return pl.bottom() - f * pl.height();
    }

    int points() const { return int(m_reading.history.size()); }
    bool dimmed() const { return !m_reading.playing && points() > 0; }
    QString overlay() const
    {
        if (!m_reading.loaded)
            return QString();
        if (dimmed())
            return panelTr("Last run shown dimmed");
        if (points() == 0 && !m_reading.playing)
            return panelTr("Play the song to fill this in");
        return QString();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF pl = plot();
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.setPen(QPen(themes::color(Role::palette_outline), 1.0));
        p.setBrush(themes::color(Role::cpu_meter_track_background));
        p.drawPath(frame);
        p.setClipPath(frame);

        const QColor amber = themes::color(Role::cpu_meter_warn_text);
        const QColor red = themes::color(Role::cpu_meter_over_text);
        const QColor low = themes::color(Role::cpu_meter_history_fill);
        const double vbY = yFor(m_reading.cpu.vblankCycles);
        const bool budgetOnScale = m_reading.budgetCycles < scaleCycles(m_reading);
        const double buY = yFor(m_reading.budgetCycles);
        p.setPen(QPen(withAlpha(red, 0.7), 1.0));
        p.drawLine(QPointF(pl.left(), vbY), QPointF(pl.right(), vbY));
        if (budgetOnScale) {
            QPen dash(withAlpha(amber, 0.7), 1.0, Qt::CustomDashLine);
            dash.setDashPattern({4.0, 4.0});
            p.setPen(dash);
            p.drawLine(QPointF(pl.left(), buY), QPointF(pl.right(), buY));
        }

        const int n = points();
        if (n > 0) {
            const double step = pl.width() / double(AudioEngine::kCpuHistoryFrames - 1);
            QPainterPath line;
            for (int k = 0; k < n; k++) {
                const double x = pl.right() - double(n - 1 - k) * step;
                const double y = yFor(m_reading.history[size_t(k)].terms[AudioEngine::CpuTotal]);
                if (k == 0)
                    line.moveTo(x, y);
                else
                    line.lineTo(x, y);
            }
            const double x0 = pl.right() - double(n - 1) * step;
            QPainterPath area = line;
            area.lineTo(pl.right(), pl.bottom());
            area.lineTo(x0, pl.bottom());
            area.closeSubpath();
            // The bands: red above the VBlank line, amber between it and a
            // budget under it, the neutral blue below.
            const auto bands = [&](double alpha) {
                QLinearGradient g(QPointF(0, pl.top()), QPointF(0, pl.bottom()));
                const auto at = [&](double y) {
                    return std::clamp((y - pl.top()) / pl.height(), 0.0, 1.0);
                };
                const double fv = at(vbY);
                g.setColorAt(0.0, withAlpha(red, alpha));
                g.setColorAt(fv, withAlpha(red, alpha));
                const double fl = std::min(1.0, fv + 1e-4);
                if (m_reading.budgetCycles < m_reading.cpu.vblankCycles) {
                    const double fb = at(buY);
                    g.setColorAt(fl, withAlpha(amber, alpha));
                    g.setColorAt(fb, withAlpha(amber, alpha));
                    g.setColorAt(std::min(1.0, fb + 1e-4), withAlpha(low, alpha));
                } else {
                    g.setColorAt(fl, withAlpha(low, alpha));
                }
                g.setColorAt(1.0, withAlpha(low, alpha));
                return g;
            };
            const bool dim = dimmed();
            p.setPen(Qt::NoPen);
            p.setBrush(bands(dim ? 0.06 : 0.16));
            p.drawPath(area);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QBrush(bands(dim ? 0.35 : 1.0)), 1.75, Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin));
            p.drawPath(line);
        }
        const QString text = overlay();
        if (!text.isEmpty()) {
            // On a patch of the surface, clear of the lines under it.
            const QRect box = rect().adjusted(8, 8, -8, -8);
            const QRect ink =
                fontMetrics().boundingRect(box, Qt::AlignCenter | Qt::TextWordWrap, text);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(themes::color(Role::cpu_meter_track_background), 0.85));
            p.drawRoundedRect(QRectF(ink.adjusted(-8, -3, 8, 3)), 4, 4);
            p.setPen(secondaryInk());
            p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, text);
        }
    }

  private:
    CpuPanel::Reading m_reading;
};

// A term's share of the frame: a thin bar.
class CpuShareBar : public QWidget
{
  public:
    CpuShareBar(Role role, QWidget *parent) : QWidget(parent), m_role(role)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(6);
        setMinimumWidth(16);
    }

    void setShare(double share)
    {
        share = std::clamp(share, 0.0, 1.0);
        if (share == m_share)
            return;
        m_share = share;
        update();
    }
    int fillWidth() const { return int(std::lround(m_share * width())); }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(themes::color(Role::cpu_meter_track_background));
        p.drawRoundedRect(QRectF(rect()), 3, 3);
        if (fillWidth() > 0) {
            p.setBrush(themes::color(m_role));
            p.drawRoundedRect(QRectF(0, 0, fillWidth(), height()), 3, 3);
        }
    }

  private:
    Role m_role;
    double m_share = 0.0;
};

bool CpuPanel::Reading::operator==(const Reading &o) const
{
    const auto sameCpu = [](const AudioEngine::CpuSnapshot &a, const AudioEngine::CpuSnapshot &b) {
        return a.valid == b.valid && a.estimated == b.estimated && a.peakValid == b.peakValid &&
               std::equal(a.last, a.last + 4, b.last) && std::equal(a.peak, a.peak + 4, b.peak) &&
               a.vblankCycles == b.vblankCycles && a.frameCycles == b.frameCycles &&
               a.peakAtSeconds == b.peakAtSeconds;
    };
    const auto sameHistory = [](const std::vector<AudioEngine::CpuHistoryEntry> &a,
                                const std::vector<AudioEngine::CpuHistoryEntry> &b) {
        return std::equal(
            a.begin(), a.end(), b.begin(), b.end(),
            [](const AudioEngine::CpuHistoryEntry &x, const AudioEngine::CpuHistoryEntry &y) {
                return std::equal(x.terms, x.terms + 4, y.terms) && x.seconds == y.seconds;
            });
    };
    return loaded == o.loaded && playing == o.playing && sameCpu(cpu, o.cpu) &&
           budgetCycles == o.budgetCycles && mixRate == o.mixRate && maxPcm == o.maxPcm &&
           hqMixer == o.hqMixer && sameHistory(history, o.history);
}

int CpuPanel::level(int32_t total, int32_t budgetCycles)
{
    if (total > M4A_VBLANK_CYCLES)
        return 2;
    return total > budgetCycles ? 1 : 0;
}

QString CpuPanel::songTime(double seconds)
{
    const long long tenths = std::llround(std::max(0.0, seconds) * 10.0);
    const long long minutes = tenths / 600;
    const long long rest = tenths % 600;
    return QStringLiteral("%1:%2.%3")
        .arg(minutes)
        .arg(rest / 10, 2, 10, QLatin1Char('0'))
        .arg(rest % 10);
}

QLabel *CpuPanel::sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text.toUpper(), parent);
    m_sectionLabels.append(label);
    return label;
}

CpuPanel::CpuPanel(QWidget *parent) : QWidget(parent)
{
    // Scrollable content like the Polyphony dock: a short window scrolls
    // instead of clipping the bottom.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(m_scroll);
    m_content = new QWidget;
    auto *layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto section = [&](int top, int bottom) {
        auto *box = new QWidget(m_content);
        auto *v = new QVBoxLayout(box);
        v->setContentsMargins(14, top, 14, bottom);
        v->setSpacing(4);
        layout->addWidget(box);
        m_sections.append(box);
        return v;
    };
    const QString vblankHelp =
        tr("Percent of the GBA's VBlank window (%L1 cycles = 100%).").arg(M4A_VBLANK_CYCLES);

    // Header: the title and the status pill.
    {
        auto *v = section(10, 10);
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        auto *title = sectionLabel(tr("Audio CPU"), m_sections.back());
        title->setToolTip(tr("Estimated GBA CPU time of the sound driver (m4aSoundMain) per "
                             "frame, for the song engine."));
        row->addWidget(title);
        m_pill = new CpuStatusPill(m_sections.back());
        m_pill->setToolTip(tr("Within budget: at or under your CPU budget (Settings → Audio). "
                              "Over budget: above it but inside the VBlank window. Past VBlank: "
                              "the sound driver runs on into the frame."));
        row->addWidget(m_pill);
        row->addStretch();
        v->addLayout(row);
    }
    layout->addWidget(new CpuRule(m_content));

    // Hero: this frame.
    {
        auto *v = section(14, 4);
        QWidget *box = m_sections.back();
        m_heroTitle = new QLabel(tr("This frame"), box);
        m_secondaryLabels.append(m_heroTitle);
        v->addWidget(m_heroTitle);
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        m_percent = new QLabel(QStringLiteral("—"), box);
        m_percent->setObjectName(QStringLiteral("cpuPanelPercent"));
        m_percent->setToolTip(tr("Estimated GBA CPU time of the sound driver (m4aSoundMain) in "
                                 "the last frame.") +
                              QLatin1Char('\n') + vblankHelp);
        row->addWidget(m_percent, 0, Qt::AlignBaseline);
        m_heroCaption = new QLabel(box);
        m_heroCaption->setWordWrap(true);
        m_secondaryLabels.append(m_heroCaption);
        row->addWidget(m_heroCaption, 1, Qt::AlignBaseline);
        v->addLayout(row);
        m_detail = new QLabel(box);
        m_detail->setWordWrap(true);
        m_detail->setTextFormat(Qt::RichText);
        m_detail->setToolTip(tr("The last frame's cycles, and how far it is from your budget "
                                "(Settings → Audio) and the end of the VBlank window."));
        v->addWidget(m_detail);
    }

    // The meter.
    {
        auto *v = section(10, 4);
        m_meter = new CpuMeter(m_sections.back());
        m_meter->setToolTip(
            tr("The last frame's cost on a scale from 0 to %1% of the VBlank window: mixer, "
               "sequencer and CGB stacked from the left. The amber line is your budget "
               "(Settings → Audio), the red line the end of the VBlank window; the dashed line "
               "marks the peak.")
                .arg(kScalePercent));
        v->addWidget(m_meter);
    }

    // The last ten seconds.
    {
        auto *v = section(12, 4);
        QWidget *box = m_sections.back();
        v->setSpacing(6);
        auto *head = new QHBoxLayout;
        head->addWidget(sectionLabel(tr("Last 10 seconds"), box));
        head->addStretch();
        m_historyCaption = new QLabel(box);
        m_historyCaption->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_historyCaption->setWordWrap(true);
        m_secondaryLabels.append(m_historyCaption);
        head->addWidget(m_historyCaption, 1);
        v->addLayout(head);
        m_chart = new CpuHistoryChart(box);
        m_chart->setToolTip(tr("Every frame of the last ten seconds of playback, newest at the "
                               "right, on the meter's scale. Stopped, the last run stays (dimmed) "
                               "until the song plays again from the start of a run."));
        v->addWidget(m_chart);
        auto *axis = new QHBoxLayout;
        auto *from = new QLabel(tr("−10 s"), box);
        auto *now = new QLabel(tr("now"), box);
        now->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        for (QLabel *l : {from, now})
            m_secondaryLabels.append(l);
        axis->addWidget(from);
        axis->addStretch();
        axis->addWidget(now);
        v->addLayout(axis);
    }

    // Where it goes.
    {
        auto *v = section(12, 4);
        QWidget *box = m_sections.back();
        auto *grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);
        grid->addWidget(sectionLabel(tr("Where it goes"), box), 0, 0, 1, 2);
        auto *cyclesHead = new QLabel(tr("cycles"), box);
        auto *shareHead = new QLabel(tr("share"), box);
        shareHead->setToolTip(tr("Share of this frame's cycles."));
        for (QLabel *l : {cyclesHead, shareHead}) {
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_secondaryLabels.append(l);
        }
        grid->addWidget(cyclesHead, 0, 2);
        grid->addWidget(shareHead, 0, 3);
        const QString termHelp[3] = {
            tr("MPlayMain: reading the song's commands for every track."),
            tr("CgbSound: updating the four CGB channels (the squares, the wave and the noise)."),
            tr("SoundMainRAM: mixing the PCM channels into the sample buffer. Grows with the "
               "channels playing and the mix rate."),
        };
        const QString termName[3] = {tr("Sequencer"), tr("CGB"), tr("Mixer")};
        for (int row = 0; row < 3; row++) {
            const int term = kRowTerm[row];
            auto *nameBox = new QWidget(box);
            auto *nameRow = new QHBoxLayout(nameBox);
            nameRow->setContentsMargins(0, 0, 0, 0);
            nameRow->setSpacing(6);
            nameRow->addWidget(new CpuSwatch(kTermFill[term], false, nameBox), 0, Qt::AlignVCenter);
            m_termName[term] = new QLabel(termName[term], nameBox);
            nameRow->addWidget(m_termName[term]);
            m_termNote[term] = new QLabel(nameBox);
            m_secondaryLabels.append(m_termNote[term]);
            nameRow->addWidget(m_termNote[term]);
            nameRow->addStretch();
            m_termBar[term] = new CpuShareBar(kTermFill[term], box);
            m_termCycles[term] = new QLabel(QStringLiteral("—"), box);
            m_termShare[term] = new QLabel(QStringLiteral("—"), box);
            for (QLabel *l : {m_termCycles[term], m_termShare[term]}) {
                l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                m_monoLabels.append(l);
            }
            for (QWidget *w :
                 {static_cast<QWidget *>(nameBox), static_cast<QWidget *>(m_termName[term]),
                  static_cast<QWidget *>(m_termBar[term]),
                  static_cast<QWidget *>(m_termCycles[term]),
                  static_cast<QWidget *>(m_termShare[term])})
                w->setToolTip(termHelp[term]);
            grid->addWidget(nameBox, row + 1, 0);
            grid->addWidget(m_termBar[term], row + 1, 1, Qt::AlignVCenter);
            grid->addWidget(m_termCycles[term], row + 1, 2);
            grid->addWidget(m_termShare[term], row + 1, 3);
        }
        m_termNote[AudioEngine::CpuSequencer]->setToolTip(
            tr("Estimated: porydaw plays MIDI, not the compiled song, so the sequencer's work is "
               "worked out from the events it plays."));
        grid->setColumnStretch(1, 1);
        v->addLayout(grid);
    }

    // The peak card.
    {
        auto *v = section(14, 4);
        m_peakCard = new CpuCard(m_sections.back());
        m_peakGrid = new QGridLayout(m_peakCard);
        m_peakGrid->setContentsMargins(14, 12, 12, 12);
        m_peakGrid->setHorizontalSpacing(12);
        m_peakGrid->setVerticalSpacing(2);
        m_peakTitle = new QLabel(m_peakCard);
        m_secondaryLabels.append(m_peakTitle);
        m_peakLine = new QWidget(m_peakCard);
        auto *line = new QHBoxLayout(m_peakLine);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        m_peak = new QLabel(m_peakLine);
        m_peak->setToolTip(tr("The most expensive frame since the song last started playing "
                              "from stop, or since Reset. A change of mix rate, mixer or "
                              "voicegroup (a voice edit that reloads it) starts it over.") +
                           QLatin1Char('\n') + vblankHelp);
        line->addWidget(m_peak, 0, Qt::AlignBaseline);
        m_peakDetail = new QLabel(m_peakLine);
        m_peakDetail->setWordWrap(true);
        m_peakDetail->setToolTip(tr("The peak frame's cycles, and where in the song it played."));
        line->addWidget(m_peakDetail, 1, Qt::AlignBaseline);
        m_reset = new QPushButton(tr("Reset peak"), m_peakCard);
        m_reset->setToolTip(tr("Start the peak and the last ten seconds over from the next "
                               "frame."));
        connect(m_reset, &QPushButton::clicked, this, &CpuPanel::resetRequested);
        v->addWidget(m_peakCard);
    }

    layout->addStretch();

    // The legend and the configuration.
    layout->addWidget(new CpuRule(m_content));
    {
        auto *v = section(12, 14);
        QWidget *box = m_sections.back();
        v->setSpacing(8);
        auto *grid = new QGridLayout;
        m_legendGrid = grid;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(6);
        const Role marks[3] = {Role::cpu_meter_warn_text, Role::cpu_meter_over_text,
                               Role::secondary_text};
        const QString names[3] = {tr("Budget"), tr("VBlank"), tr("Frame")};
        const QString texts[3] = {tr("recommended ceiling for audio"),
                                  tr("past this, audio eats into the frame"),
                                  tr("whole frame, for scale")};
        const QString tips[3] = {
            tr("Your CPU budget, set in Settings → Audio: above it the readout turns amber."),
            tr("The VBlank window, 100%: everything the game does during VBlank shares it. Past "
               "it the readout turns red."),
            tr("One whole GBA frame (shown for scale)."),
        };
        for (int i = 0; i < 3; i++) {
            auto *nameBox = new QWidget(box);
            m_legendNameBox[i] = nameBox;
            auto *nameRow = new QHBoxLayout(nameBox);
            nameRow->setContentsMargins(0, 0, 0, 0);
            nameRow->setSpacing(6);
            nameRow->addWidget(new CpuSwatch(marks[i], true, nameBox), 0, Qt::AlignVCenter);
            m_legendName[i] = new QLabel(names[i], nameBox);
            nameRow->addWidget(m_legendName[i]);
            m_legendText[i] = new QLabel(texts[i], box);
            m_legendText[i]->setWordWrap(true);
            m_secondaryLabels.append(m_legendText[i]);
            m_legendValue[i] = new QLabel(box);
            m_legendValue[i]->setAlignment(Qt::AlignRight | Qt::AlignTop);
            for (QWidget *w :
                 {static_cast<QWidget *>(m_legendName[i]), static_cast<QWidget *>(m_legendText[i]),
                  static_cast<QWidget *>(m_legendValue[i])})
                w->setToolTip(tips[i]);
        }
        v->addLayout(grid);
        setNarrow(false);
        m_config = new QLabel(box);
        m_config->setWordWrap(true);
        m_config->setToolTip(
            tr("The engine settings the cost is estimated for (Settings → Audio)."));
        m_secondaryLabels.append(m_config);
        v->addWidget(m_config);
    }

    applyFonts();
    m_scroll->setWidget(m_content);
    refresh();
}

void CpuPanel::applyFonts()
{
    const QFont base = font();
    QFont section = typography::bold(typography::caption(base));
    section.setLetterSpacing(QFont::PercentageSpacing, 108);
    for (auto *label : m_sectionLabels)
        label->setFont(section);
    m_percent->setFont(monoFont(base, 3.4));
    m_peak->setFont(monoFont(base, 1.8));
    const QFont caption = typography::caption(base);
    const QFont mono = typography::bodyMono(caption);
    for (auto *label : m_monoLabels)
        label->setFont(mono);
    for (auto *label : m_legendValue)
        label->setFont(typography::bodyMono(caption));
    m_pill->setFont(typography::bold(caption));
    for (int term = 0; term < 3; term++)
        m_termNote[term]->setFont(caption);
    m_historyCaption->setFont(caption);
    m_config->setFont(caption);
}

void CpuPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool narrow = event->size().width() < kNarrowWidth;
    if (narrow != m_narrow)
        setNarrow(narrow);
}

// Wide, each legend row is the mark, its description and its value, and the
// peak card's Reset sits right of the peak; narrow, the descriptions go under
// their marks (indented to their names) and Reset beside the card's title.
void CpuPanel::setNarrow(bool stacked)
{
    m_peakGrid->removeWidget(m_peakTitle);
    m_peakGrid->removeWidget(m_peakLine);
    m_peakGrid->removeWidget(m_reset);
    m_peakGrid->addWidget(m_peakTitle, 0, 0);
    if (stacked) {
        m_peakGrid->addWidget(m_reset, 0, 1, Qt::AlignRight | Qt::AlignVCenter);
        m_peakGrid->addWidget(m_peakLine, 1, 0, 1, 2);
    } else {
        m_peakGrid->addWidget(m_peakLine, 1, 0);
        m_peakGrid->addWidget(m_reset, 0, 1, 2, 1, Qt::AlignVCenter);
    }
    m_peakGrid->setColumnStretch(0, 1);
    m_peakGrid->setColumnStretch(1, 0);

    for (int i = 0; i < 3; i++) {
        m_legendGrid->removeWidget(m_legendNameBox[i]);
        m_legendGrid->removeWidget(m_legendText[i]);
        m_legendGrid->removeWidget(m_legendValue[i]);
    }
    for (int i = 0; i < 3; i++) {
        if (stacked) {
            m_legendGrid->addWidget(m_legendNameBox[i], 2 * i, 0, Qt::AlignTop);
            m_legendGrid->addWidget(m_legendValue[i], 2 * i, 1, 1, 2);
            m_legendGrid->addWidget(m_legendText[i], 2 * i + 1, 0, 1, 3);
            m_legendText[i]->setContentsMargins(16, 0, 0, 0); // the swatch and its gap
        } else {
            m_legendText[i]->setContentsMargins(0, 0, 0, 0);
            m_legendGrid->addWidget(m_legendNameBox[i], i, 0, Qt::AlignTop);
            m_legendGrid->addWidget(m_legendText[i], i, 1);
            m_legendGrid->addWidget(m_legendValue[i], i, 2);
        }
    }
    m_legendGrid->setColumnStretch(1, 1);
    m_legendGrid->setVerticalSpacing(stacked ? 2 : 6);
    m_narrow = stacked;
}

void CpuPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::StyleChange:
        applyFonts();
        refresh();
        break;
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
        refresh();
        break;
    default:
        break;
    }
}

void CpuPanel::setReading(const Reading &reading)
{
    if (m_haveReading && reading == m_reading)
        return;
    m_reading = reading;
    m_haveReading = true;
    refresh();
}

void CpuPanel::refresh()
{
    if (!m_config)
        return; // still being built
    const Reading &r = m_reading;
    const AudioEngine::CpuSnapshot &cpu = r.cpu;
    const bool frame = hasFrame(r);
    const int32_t vblank = cpu.vblankCycles;
    const int32_t total = cpu.last[AudioEngine::CpuTotal];
    const int lvl = frame ? level(total, r.budgetCycles) : 0;
    m_levelName = !frame     ? QString()
                  : lvl == 2 ? QStringLiteral("over")
                  : lvl == 1 ? QStringLiteral("warn")
                             : QString();
    const QColor secondary = secondaryInk();
    for (QLabel *l : m_secondaryLabels)
        setInk(l, secondary);
    for (QLabel *l : m_sectionLabels)
        setInk(l, secondary);

    // Status.
    QString status;
    QColor statusInk = secondary;
    if (!r.loaded) {
        status = tr("No song");
    } else if (!r.playing) {
        status = tr("Stopped");
    } else if (!cpu.valid) {
        status = tr("Measuring");
    } else if (lvl == 2) {
        status = tr("Past VBlank");
        statusInk = themes::color(Role::cpu_meter_over_text);
    } else if (lvl == 1) {
        status = tr("Over budget");
        statusInk = themes::color(Role::cpu_meter_warn_text);
    } else {
        status = tr("Within budget");
        statusInk = themes::color(Role::cpu_meter_ok_text);
    }
    m_pill->setState(status, statusInk);

    // Hero.
    m_percent->setText(frame ? percentString(total, vblank) : QStringLiteral("—"));
    setInk(m_percent, frame ? levelInk(lvl) : secondary);
    m_heroCaption->setText(frame       ? tr("of the VBlank window")
                           : !r.loaded ? tr("no song open")
                           : r.playing ? tr("measuring")
                                       : tr("not playing"));
    if (frame) {
        if (lvl == 2)
            m_headroom = tr("%1 cycles past VBlank").arg(cyclesString(int64_t(total) - vblank));
        else if (lvl == 1)
            m_headroom = tr("%1 over budget, %2 left in VBlank")
                             .arg(cyclesString(int64_t(total) - r.budgetCycles),
                                  cyclesString(int64_t(vblank) - total));
        else
            m_headroom = tr("%1 under budget").arg(cyclesString(int64_t(r.budgetCycles) - total));
        const QString cycles = cyclesString(total);
        m_detailPlain = tr("%1 cycles · %2").arg(cycles, m_headroom);
        const QColor ink = lvl == 0 ? themes::color(Role::cpu_meter_ok_text) : levelInk(lvl);
        const QString mono = typography::bodyMono(font()).family().toHtmlEscaped();
        m_detail->setText(
            tr("%1 cycles · %2")
                .arg(QStringLiteral("<span style=\"font-family:'%1'\">%2</span>")
                         .arg(mono, cycles.toHtmlEscaped()),
                     QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span>")
                         .arg(ink.name(QColor::HexRgb), m_headroom.toHtmlEscaped())));
    } else {
        m_headroom.clear();
        m_detailPlain = !r.loaded   ? tr("Open a song to see what its sound costs.")
                        : r.playing ? tr("No reading yet.")
                                    : tr("Press Play to measure the sound driver frame by frame.");
        m_detail->setText(m_detailPlain.toHtmlEscaped());
    }
    setInk(m_detail, themes::color(Role::window_text));

    m_meter->setReading(r);

    // History: the window's maximum and its share over the budget.
    QString historyCaption;
    if (r.loaded && !r.history.empty()) {
        if (!r.playing) {
            historyCaption = tr("last run");
        } else {
            int32_t maxTotal = 0;
            size_t over = 0;
            for (const AudioEngine::CpuHistoryEntry &e : r.history) {
                maxTotal = std::max(maxTotal, e.terms[AudioEngine::CpuTotal]);
                over += e.terms[AudioEngine::CpuTotal] > r.budgetCycles ? 1 : 0;
            }
            const QString maxText = percentString(maxTotal, vblank);
            if (over == 0) {
                historyCaption = tr("max %1, never over budget").arg(maxText);
            } else {
                // Never "0%" with a frame over, never "100%" with one under.
                long share = std::lround(double(over) * 100.0 / double(r.history.size()));
                share = std::max(share, 1L);
                if (over < r.history.size())
                    share = std::min(share, 99L);
                historyCaption = tr("max %1, over budget %2% of the time").arg(maxText).arg(share);
            }
        }
    }
    m_historyCaption->setText(historyCaption);
    m_chart->setReading(r);

    // Where it goes.
    for (int term = 0; term < 3; term++) {
        const int32_t n = cpu.last[term];
        m_termCycles[term]->setText(frame ? cyclesString(n) : QStringLiteral("—"));
        const double share = frame && total > 0 ? double(n) / double(total) : 0.0;
        m_termShare[term]->setText(frame ? QStringLiteral("%1%").arg(std::lround(share * 100.0))
                                         : QStringLiteral("—"));
        m_termBar[term]->setShare(share);
        m_termNote[term]->setText(QString());
        for (QLabel *l : {m_termName[term], m_termCycles[term], m_termShare[term]})
            setInk(l, themes::color(Role::window_text));
    }
    m_termNote[AudioEngine::CpuSequencer]->setText(r.loaded && cpu.estimated ? tr("est.")
                                                                             : QString());

    // The peak card.
    const bool peak = hasPeak(r);
    m_peakCard->parentWidget()->setVisible(peak);
    if (peak) {
        const int32_t peakTotal = cpu.peak[AudioEngine::CpuTotal];
        m_peakTitle->setText(r.playing ? tr("Peak since Play") : tr("Peak in last run"));
        m_peak->setText(percentString(peakTotal, vblank));
        setInk(m_peak, levelInk(level(peakTotal, r.budgetCycles)));
        m_peakDetail->setText(
            cpu.peakAtSeconds >= 0.0
                ? tr("%1 cycles at %2").arg(cyclesString(peakTotal), songTime(cpu.peakAtSeconds))
                : tr("%1 cycles").arg(cyclesString(peakTotal)));
        setInk(m_peakDetail, themes::color(Role::window_text));
    } else {
        m_peakTitle->clear();
        m_peak->clear();
        m_peakDetail->clear();
    }
    m_reset->setEnabled(r.loaded);

    // The legend.
    const int64_t values[3] = {r.budgetCycles, vblank, cpu.frameCycles};
    for (int i = 0; i < 3; i++) {
        m_legendValue[i]->setText(QStringLiteral("%1 · %2").arg(cyclesString(values[i]),
                                                                percentString(values[i], vblank)));
        setInk(m_legendName[i], themes::color(Role::window_text));
        setInk(m_legendValue[i], themes::color(Role::window_text));
    }
    m_config->setText(r.loaded ? tr("%1 Hz mix rate · %2 PCM channels · %3")
                                     .arg(r.mixRate)
                                     .arg(r.maxPcm)
                                     .arg(r.hqMixer ? tr("HQ mixer") : tr("stock mixer"))
                               : QString());
    m_config->setVisible(r.loaded);
}

QString CpuPanel::statusText() const
{
    return m_pill->text();
}

QColor CpuPanel::statusColor() const
{
    return m_pill->ink();
}

QString CpuPanel::percentText() const
{
    return m_percent->text();
}

QString CpuPanel::heroCaptionText() const
{
    return m_heroCaption->text();
}

QString CpuPanel::detailText() const
{
    return m_detailPlain;
}

QString CpuPanel::headroomText() const
{
    return m_headroom;
}

QString CpuPanel::levelName() const
{
    return m_levelName;
}

QColor CpuPanel::percentColor() const
{
    return m_percent->palette().color(QPalette::WindowText);
}

QRect CpuPanel::meterTrackRect() const
{
    return m_meter->track();
}

int CpuPanel::meterXFor(int64_t cycles) const
{
    return m_meter->xFor(cycles);
}

int CpuPanel::budgetMarkX() const
{
    return m_meter->budgetX();
}

int CpuPanel::vblankMarkX() const
{
    return m_meter->vblankX();
}

int CpuPanel::peakMarkX() const
{
    return m_meter->peakX();
}

QRect CpuPanel::segmentRect(int term) const
{
    return m_meter->segment(term);
}

bool CpuPanel::overflowShown() const
{
    return m_meter->overflow();
}

QString CpuPanel::peakLabelText() const
{
    return m_meter->peakLabel();
}

QList<CpuPanel::Caption> CpuPanel::meterCaptions() const
{
    return m_meter->captions();
}

QImage CpuPanel::meterImage() const
{
    return m_meter->grab().toImage();
}

QRect CpuPanel::meterRect() const
{
    return QRect(m_meter->mapTo(m_content, QPoint(0, 0)), m_meter->size());
}

QString CpuPanel::historyCaption() const
{
    return m_historyCaption->text();
}

int CpuPanel::historyPoints() const
{
    return m_chart->points();
}

bool CpuPanel::historyDimmed() const
{
    return m_chart->dimmed();
}

QString CpuPanel::historyOverlayText() const
{
    return m_chart->overlay();
}

QImage CpuPanel::historyImage() const
{
    return m_chart->grab().toImage();
}

QRect CpuPanel::historyRect() const
{
    return QRect(m_chart->mapTo(m_content, QPoint(0, 0)), m_chart->size());
}

int CpuPanel::historyYFor(int64_t cycles) const
{
    return int(std::lround(m_chart->yFor(double(cycles))));
}

QString CpuPanel::termCyclesText(int term) const
{
    return term >= 0 && term < 3 ? m_termCycles[term]->text() : QString();
}

QString CpuPanel::termShareText(int term) const
{
    return term >= 0 && term < 3 ? m_termShare[term]->text() : QString();
}

int CpuPanel::termBarFillWidth(int term) const
{
    return term >= 0 && term < 3 ? m_termBar[term]->fillWidth() : 0;
}

QString CpuPanel::sequencerNoteText() const
{
    return m_termNote[AudioEngine::CpuSequencer]->text();
}

bool CpuPanel::peakCardShown() const
{
    return m_peakCard->parentWidget()->isVisibleTo(this);
}

QString CpuPanel::peakTitleText() const
{
    return m_peakTitle->text();
}

QString CpuPanel::peakText() const
{
    return m_peak->text();
}

QString CpuPanel::peakDetailText() const
{
    return m_peakDetail->text();
}

QColor CpuPanel::peakColor() const
{
    return m_peak->palette().color(QPalette::WindowText);
}

QStringList CpuPanel::legendTexts() const
{
    QStringList out;
    for (int i = 0; i < 3; i++)
        out << m_legendName[i]->text() << m_legendText[i]->text() << m_legendValue[i]->text();
    return out;
}

QString CpuPanel::configText() const
{
    return m_config->text();
}

QList<QRect> CpuPanel::sectionRects() const
{
    QList<QRect> out;
    for (QWidget *box : m_sections)
        if (box->isVisibleTo(m_content))
            out.append(box->geometry());
    return out;
}

int CpuPanel::hScrollRange() const
{
    return m_scroll->horizontalScrollBar()->maximum();
}

QStringList CpuPanel::visibleStrings() const
{
    QStringList out;
    for (const QLabel *label : findChildren<QLabel *>())
        out << label->text() << label->toolTip();
    for (const QWidget *w : findChildren<QWidget *>())
        if (!qobject_cast<const QLabel *>(w))
            out << w->toolTip();
    out << m_pill->text() << m_chart->overlay() << m_reset->text();
    for (const Caption &c : m_meter->captions())
        out << c.text;
    out.removeAll(QString());
    return out;
}

void CpuPanel::clickReset()
{
    m_reset->click();
}
