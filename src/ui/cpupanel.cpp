#include "ui/cpupanel.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QEvent>
#include <QFontInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

#include "ui/theme/themeruntime.h"
#include "ui/typography.h"

namespace {

// Panel width at which the breakdown moves to the right of the readout
// instead of stacking under it (the Polyphony dock's pattern).
constexpr int kWideLayoutMinWidth = 520;
constexpr int kReadoutColumnWidth = 190; // readout column width in wide mode

const themes::Role kTermFill[3] = {themes::Role::cpu_meter_sequencer_fill,
                                   themes::Role::cpu_meter_cgb_fill,
                                   themes::Role::cpu_meter_mixer_fill};

double percentOf(int64_t cycles, int32_t vblank)
{
    return vblank > 0 ? double(cycles) * 100.0 / double(vblank) : 0.0;
}

// A small square in a term's colour, beside its name.
class CpuSwatch : public QWidget
{
  public:
    CpuSwatch(themes::Role role, QWidget *parent) : QWidget(parent), m_role(role)
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
        p.drawRoundedRect(rect(), 2, 2);
    }

  private:
    themes::Role m_role;
};

} // namespace

// The breakdown bar: the three terms stacked from the left over a two-zone
// scale. The first three quarters of the track are the VBlank window (0 to
// 100 %, linear); the last quarter, tinted, is the rest of the frame up to
// its end (100 % to 335 %, compressed), so a typical reading fills a
// readable share of the bar while the frame end stays on it. Marks: the
// VBlank boundary (a window-text line across the track, drawn over the
// budget), the budget (an amber line with its caption above the track),
// the frame end (red, at the right end) and the peak (a triangle under the
// track). Captions above (budget) and below (VBlank, frame end) are
// painted in the caption face.
class CpuBar : public QWidget
{
  public:
    explicit CpuBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setReading(const CpuPanel::Reading &r)
    {
        m_reading = r;
        update();
    }

    bool hasReading() const { return m_reading.loaded && m_reading.playing && m_reading.cpu.valid; }

    int labelHeight() const { return QFontMetrics(captionFont()).height(); }

    QRect track() const
    {
        return QRect(kPad, labelHeight() + kGap, std::max(4, width() - 2 * kPad), kBarH);
    }

    // The width of the VBlank zone (0..100 %).
    static int vblankZoneWidth(int trackWidth) { return int(std::lround(0.75 * trackWidth)); }

    int xFor(int64_t cycles) const
    {
        const QRect t = track();
        const int64_t vblank = std::max<int32_t>(1, m_reading.cpu.vblankCycles);
        const int64_t frame = std::max<int64_t>(vblank + 1, m_reading.cpu.frameCycles);
        const int64_t c = std::clamp<int64_t>(cycles, 0, frame);
        const int64_t w = t.width();
        const int64_t w0 = vblankZoneWidth(t.width());
        if (c <= vblank)
            return t.left() + int((c * w0 + vblank / 2) / vblank);
        const int64_t span = frame - vblank;
        return t.left() + int(w0 + ((c - vblank) * (w - w0) + span / 2) / span);
    }

    // Cumulative edges, so the segments always add up to the total's width.
    QRect segment(int term) const
    {
        if (!hasReading() || term < 0 || term > 2)
            return QRect();
        const AudioEngine::CpuSnapshot &cpu = m_reading.cpu;
        int64_t from = 0;
        for (int i = 0; i < term; i++)
            from += cpu.last[i];
        const int64_t to = from + cpu.last[term];
        const QRect t = track();
        const int x0 = xFor(from), x1 = xFor(to);
        return QRect(x0, t.top(), x1 - x0, t.height());
    }

    int budgetX() const { return xFor(m_reading.budgetCycles); }
    int vblankX() const { return xFor(m_reading.cpu.vblankCycles); }
    int ceilingX() const { return xFor(m_reading.cpu.frameCycles); }

    // The marks as painted (empty when not drawn).
    QRect vblankMark() const
    {
        const QRect t = track();
        return m_reading.loaded ? QRect(vblankX(), t.top() - 2, 1, t.height() + 4) : QRect();
    }
    QRect budgetMark() const
    {
        const QRect t = track();
        return m_reading.loaded ? QRect(budgetX() - 1, t.top() - kGap, 2, t.height() + kGap)
                                : QRect();
    }
    QRect ceilingMark() const
    {
        const QRect t = track();
        return m_reading.loaded ? QRect(ceilingX() - 2, t.top() - 2, 2, t.height() + 4) : QRect();
    }
    QRect peakMark() const
    {
        if (!m_reading.loaded || !m_reading.cpu.peakValid)
            return QRect();
        const QRect t = track();
        const int px = xFor(m_reading.cpu.peak[AudioEngine::CpuTotal]);
        return QRect(px - kPeakHalf, t.bottom() + 2, 2 * kPeakHalf + 1, kPeakH);
    }
    QColor vblankColor() const { return palette().color(QPalette::WindowText); }
    QColor budgetColor() const { return themes::color(themes::Role::cpu_meter_warn_text); }
    QColor peakColor() const { return palette().color(QPalette::WindowText); }
    QColor ceilingColor() const { return themes::color(themes::Role::cpu_meter_over_text); }

    // The captions as painted: budget above the track, VBlank and frame end
    // below it (the VBlank one shortened or dropped when it would crowd the
    // frame end's). Empty rects for captions not drawn.
    struct Captions {
        QRect budget, vblank, frameEnd;
        QString budgetText, vblankText, frameEndText;
    };
    Captions captions() const
    {
        Captions c;
        if (!m_reading.loaded)
            return c;
        const QFontMetrics fm(captionFont());
        const QRect t = track();
        c.budgetText = QCoreApplication::translate("CpuPanel", "Budget %1")
                           .arg(QLocale().toString(m_reading.budgetCycles));
        const int bw = fm.horizontalAdvance(c.budgetText);
        c.budget =
            QRect(std::clamp(budgetX() - bw / 2, 0, std::max(0, width() - bw)), 0, bw, fm.height());
        const int labelTop = t.bottom() + 2 + kPeakH + 1;
        // VBlank's caption ends at its line, the frame end's at the right
        // end; each shortens to its number when they would crowd.
        const int pct =
            int(std::lround(percentOf(m_reading.cpu.frameCycles, m_reading.cpu.vblankCycles)));
        const QString vblankLong = QCoreApplication::translate("CpuPanel", "VBlank 100%");
        const QString vblankShort = QStringLiteral("100%");
        const QString frameLong = QCoreApplication::translate("CpuPanel", "Frame end %1%").arg(pct);
        const QString frameShort = QStringLiteral("%1%").arg(pct);
        const int vx = vblankX();
        const QString pairs[3][2] = {
            {vblankLong, frameLong}, {vblankLong, frameShort}, {vblankShort, frameShort}};
        for (const auto &pair : pairs) {
            const int vw = fm.horizontalAdvance(pair[0]);
            const int fw = fm.horizontalAdvance(pair[1]);
            const QRect v(std::max(0, vx - 1 - vw), labelTop, vw, fm.height());
            const QRect f(std::max(0, width() - fw), labelTop, fw, fm.height());
            c.frameEnd = f;
            c.frameEndText = pair[1];
            if (v.right() + kGap < f.left()) {
                c.vblank = v;
                c.vblankText = pair[0];
                break;
            }
        }
        return c;
    }

    QSize sizeHint() const override
    {
        return QSize(240, 2 * labelHeight() + 2 * kGap + kBarH + kPeakH + 3);
    }
    QSize minimumSizeHint() const override { return QSize(120, sizeHint().height()); }

  protected:
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange)
            updateGeometry();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect t = track();
        const QFont caption = captionFont();
        p.setFont(caption);

        // The track, and everything in it clipped to its rounded shape (no
        // antialiasing, as the Polyphony cells).
        QPainterPath trackPath;
        trackPath.addRoundedRect(QRectF(t), 3, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(themes::color(themes::Role::polyphony_cell_free_background));
        p.drawPath(trackPath);
        p.save();
        p.setClipPath(trackPath);
        // Past the VBlank window, under the frame end: tinted.
        QColor zone = palette().color(QPalette::WindowText);
        zone.setAlpha(50);
        p.fillRect(QRect(QPoint(vblankX(), t.top()), t.bottomRight()), zone);
        for (int term = 0; term < 3; term++) {
            const QRect s = segment(term);
            if (s.width() > 0)
                p.fillRect(s, themes::color(kTermFill[term]));
        }
        p.restore();
        if (!m_reading.loaded)
            return;

        // The budget, then the VBlank boundary over it (at a budget of
        // the VBlank window's cycles they coincide, and the boundary must
        // stay visible).
        p.fillRect(budgetMark(), budgetColor());
        p.fillRect(vblankMark(), vblankColor());
        p.fillRect(ceilingMark(), ceilingColor());
        // The peak: a triangle under the track, pointing up at it.
        const QRect pk = peakMark();
        if (!pk.isEmpty()) {
            const QPolygon tri({QPoint(pk.center().x(), pk.top()), QPoint(pk.left(), pk.bottom()),
                                QPoint(pk.right(), pk.bottom())});
            p.setPen(Qt::NoPen);
            p.setBrush(peakColor());
            p.drawPolygon(tri);
        }

        const Captions c = captions();
        p.setPen(budgetColor());
        p.drawText(c.budget, Qt::AlignLeft | Qt::AlignVCenter, c.budgetText);
        p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        p.drawText(c.frameEnd, Qt::AlignLeft | Qt::AlignVCenter, c.frameEndText);
        if (!c.vblank.isEmpty())
            p.drawText(c.vblank, Qt::AlignLeft | Qt::AlignVCenter, c.vblankText);
    }

  private:
    static constexpr int kPad = 2;
    static constexpr int kGap = 3;
    static constexpr int kBarH = 22;
    static constexpr int kPeakH = 6;
    static constexpr int kPeakHalf = 4;

    QFont captionFont() const { return typography::caption(font()); }

    CpuPanel::Reading m_reading;
};

bool CpuPanel::Reading::operator==(const Reading &o) const
{
    const auto sameCpu = [](const AudioEngine::CpuSnapshot &a, const AudioEngine::CpuSnapshot &b) {
        return a.valid == b.valid && a.estimated == b.estimated && a.peakValid == b.peakValid &&
               std::equal(a.last, a.last + 4, b.last) && std::equal(a.peak, a.peak + 4, b.peak) &&
               a.vblankCycles == b.vblankCycles && a.frameCycles == b.frameCycles;
    };
    return loaded == o.loaded && playing == o.playing && sameCpu(cpu, o.cpu) &&
           budgetCycles == o.budgetCycles && mixRate == o.mixRate && maxPcm == o.maxPcm &&
           hqMixer == o.hqMixer;
}

int CpuPanel::level(int32_t total, int32_t budgetCycles)
{
    if (total >= M4A_FRAME_CYCLES)
        return 2;
    return total > budgetCycles ? 1 : 0;
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
    m_scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(m_scroll);
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    const QString vblankHelp =
        tr("Percent of the GBA's VBlank window (%L1 cycles = 100%).").arg(M4A_VBLANK_CYCLES);

    // The readout: the last frame and the peak.
    m_readoutBox = new QWidget(content);
    auto *readout = new QVBoxLayout(m_readoutBox);
    readout->setContentsMargins(0, 0, 0, 0);
    readout->setSpacing(4);
    auto *currentLabel = new QLabel(tr("Sound driver per frame"), m_readoutBox);
    m_sectionLabels.append(currentLabel);
    readout->addWidget(currentLabel);
    m_percent = new QLabel(QStringLiteral("--"), m_readoutBox);
    m_percent->setObjectName(QStringLiteral("cpuPanelPercent"));
    m_percent->setAttribute(Qt::WA_StyledBackground);
    m_percent->setContentsMargins(8, 2, 8, 2);
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_percent->setToolTip(tr("Estimated GBA CPU time of the sound driver (m4aSoundMain) in "
                             "the last frame.") +
                          QLatin1Char('\n') + vblankHelp);
    auto *percentRow = new QHBoxLayout;
    percentRow->addWidget(m_percent);
    m_percentCaption = new QLabel(tr("of the VBlank window"), m_readoutBox);
    m_percentCaption->setEnabled(false);
    m_percentCaption->setWordWrap(true);
    percentRow->addWidget(m_percentCaption, 1);
    readout->addLayout(percentRow);
    m_cycles = new QLabel(QStringLiteral("--"), m_readoutBox);
    m_cycles->setToolTip(tr("GBA CPU cycles of the last frame's m4aSoundMain."));
    readout->addWidget(m_cycles);
    m_note = new QLabel(m_readoutBox);
    m_note->setEnabled(false);
    m_note->setWordWrap(true);
    readout->addWidget(m_note);

    auto *peakHeader = new QHBoxLayout;
    auto *peakLabel = new QLabel(tr("Peak since playback started"), m_readoutBox);
    peakLabel->setWordWrap(true);
    m_sectionLabels.append(peakLabel);
    peakHeader->addWidget(peakLabel, 1);
    m_reset = new QPushButton(tr("Reset"), m_readoutBox);
    m_reset->setToolTip(tr("Start the peak over from the next frame."));
    connect(m_reset, &QPushButton::clicked, this, &CpuPanel::resetRequested);
    peakHeader->addWidget(m_reset);
    readout->addLayout(peakHeader);
    m_peak = new QLabel(QStringLiteral("--"), m_readoutBox);
    m_peak->setToolTip(tr("The most expensive frame since the song last started playing from "
                          "stop, or since Reset. A change of mix rate, mixer or voicegroup "
                          "(a voice edit that reloads it) starts it over.") +
                       QLatin1Char('\n') + vblankHelp);
    readout->addWidget(m_peak);
    m_peakCycles = new QLabel(QStringLiteral("--"), m_readoutBox);
    readout->addWidget(m_peakCycles);
    readout->addStretch();

    // The breakdown: the bar, the three terms, the marks, the configuration.
    m_breakdownBox = new QWidget(content);
    auto *breakdown = new QVBoxLayout(m_breakdownBox);
    breakdown->setContentsMargins(0, 0, 0, 0);
    breakdown->setSpacing(4);
    auto *breakdownLabel = new QLabel(tr("Breakdown"), m_breakdownBox);
    m_sectionLabels.append(breakdownLabel);
    breakdown->addWidget(breakdownLabel);
    m_bar = new CpuBar(m_breakdownBox);
    m_bar->setToolTip(
        tr("The last frame's cost: sequencer, CGB and mixer from the left. The first three "
           "quarters of the bar are the VBlank window, up to the line at 100%; the tinted last "
           "quarter is the rest of the frame, compressed, up to the red frame end (%1%), past "
           "which the next frame's sound is skipped. The amber line is your budget in cycles "
           "(Settings → Audio); the triangle under the bar marks the peak.")
            .arg(std::lround(percentOf(M4A_FRAME_CYCLES, M4A_VBLANK_CYCLES))));
    breakdown->addWidget(m_bar);
    m_termsBox = new QWidget(m_breakdownBox);
    auto *terms = new QGridLayout(m_termsBox);
    terms->setContentsMargins(0, 0, 0, 0);
    terms->setHorizontalSpacing(6);
    terms->setVerticalSpacing(2);
    const QString termHelp[3] = {
        tr("MPlayMain: reading the song's commands for every track."),
        tr("CgbSound: updating the four CGB channels (the squares, the wave and the noise)."),
        tr("SoundMainRAM: mixing the PCM channels into the sample buffer. Grows with the "
           "channels playing and the mix rate."),
    };
    // Column captions: the share is of the VBlank window, like the readout.
    auto *cyclesHeader = new QLabel(tr("cycles"), m_termsBox);
    auto *shareHeader = new QLabel(tr("of VBlank"), m_termsBox);
    for (QLabel *l : {cyclesHeader, shareHeader}) {
        l->setEnabled(false);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    shareHeader->setToolTip(vblankHelp);
    terms->addWidget(cyclesHeader, 0, 2);
    terms->addWidget(shareHeader, 0, 3);
    for (int term = 0; term < 3; term++) {
        auto *swatch = new CpuSwatch(kTermFill[term], m_termsBox);
        m_termName[term] = new QLabel(m_termsBox);
        m_termName[term]->setWordWrap(true); // "Sequencer (estimated)" in a sliver dock
        m_termCycles[term] = new QLabel(QStringLiteral("--"), m_termsBox);
        m_termPercent[term] = new QLabel(QStringLiteral("--"), m_termsBox);
        m_termCycles[term]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_termPercent[term]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_termName[term]->setToolTip(termHelp[term]);
        m_termCycles[term]->setToolTip(termHelp[term]);
        m_termPercent[term]->setToolTip(termHelp[term] + QLatin1Char('\n') + vblankHelp);
        terms->addWidget(swatch, term + 1, 0, Qt::AlignVCenter);
        terms->addWidget(m_termName[term], term + 1, 1);
        terms->addWidget(m_termCycles[term], term + 1, 2);
        terms->addWidget(m_termPercent[term], term + 1, 3);
    }
    terms->setColumnStretch(1, 1);
    breakdown->addWidget(m_termsBox);
    m_marks = new QLabel(m_breakdownBox);
    m_marks->setEnabled(false);
    m_marks->setWordWrap(true);
    breakdown->addWidget(m_marks);
    m_config = new QLabel(m_breakdownBox);
    m_config->setEnabled(false);
    m_config->setWordWrap(true);
    m_config->setToolTip(tr("The engine settings the cost is estimated for (Settings → Audio)."));
    breakdown->addWidget(m_config);
    breakdown->addStretch();

    m_bodyGrid = new QGridLayout;
    m_bodyGrid->setHorizontalSpacing(16);
    m_bodyGrid->setVerticalSpacing(12);
    layout->addLayout(m_bodyGrid);
    layout->addStretch();
    setWideLayout(false);

    applyFonts();
    m_scroll->setWidget(content);
    refresh();
}

void CpuPanel::applyFonts()
{
    const QFont base = font();
    const auto sectionFont = typography::bold(base);
    for (auto *label : m_sectionLabels)
        label->setFont(sectionFont);
    const int px = std::max(8, QFontInfo(base).pixelSize());
    QFont big = typography::bold(base);
    big.setPixelSize(px * 2);
    m_percent->setFont(big);
    QFont peak = typography::bold(base);
    peak.setPixelSize(px * 3 / 2);
    m_peak->setFont(peak);
}

void CpuPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool wide = event->size().width() >= kWideLayoutMinWidth;
    if (wide != m_wideLayout)
        setWideLayout(wide);
}

void CpuPanel::setWideLayout(bool wide)
{
    m_bodyGrid->removeWidget(m_readoutBox);
    m_bodyGrid->removeWidget(m_breakdownBox);
    m_bodyGrid->addWidget(m_readoutBox, 0, 0);
    m_bodyGrid->addWidget(m_breakdownBox, wide ? 0 : 1, wide ? 1 : 0);
    // Wide, the readout keeps a fixed-ish column and the bar takes the rest.
    m_bodyGrid->setColumnMinimumWidth(0, wide ? kReadoutColumnWidth : 0);
    m_bodyGrid->setColumnStretch(0, wide ? 0 : 1);
    m_bodyGrid->setColumnStretch(1, wide ? 1 : 0);
    m_wideLayout = wide;
    if (m_marks)
        refresh(); // the marks line is shorter when narrow
}

void CpuPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        applyFonts();
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
    const Reading &r = m_reading;
    const AudioEngine::CpuSnapshot &cpu = r.cpu;
    const bool hasReading = r.loaded && r.playing && cpu.valid;
    const bool hasPeak = r.loaded && cpu.peakValid;
    const QLocale locale;
    const auto cycles = [&](int64_t n) {
        return tr("%1 cycles").arg(locale.toString(qlonglong(n)));
    };
    const auto percent = [&](int64_t n) {
        return QStringLiteral("%1%").arg(std::lround(percentOf(n, cpu.vblankCycles)));
    };
    const int32_t total = cpu.last[AudioEngine::CpuTotal];

    m_percent->setText(hasReading ? percent(total) : QStringLiteral("--"));
    m_cycles->setText(hasReading ? cycles(total) : QStringLiteral("--"));
    const int lvl = hasReading ? level(total, r.budgetCycles) : 0;
    const QString levelName = lvl == 2   ? QStringLiteral("over")
                              : lvl == 1 ? QStringLiteral("warn")
                                         : QString();
    if (m_percent->property("cpuLevel").toString() != levelName) {
        m_percent->setProperty("cpuLevel", levelName);
        // Property selectors are matched at polish time.
        m_percent->style()->unpolish(m_percent);
        m_percent->style()->polish(m_percent);
    }
    QString note;
    if (!r.loaded)
        note = tr("Open a song to see what its sound costs.");
    else if (!r.playing)
        note = tr("Shown while the song plays.");
    else if (!cpu.valid)
        note = tr("No reading yet.");
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());

    m_peak->setText(hasPeak ? percent(cpu.peak[AudioEngine::CpuTotal]) : QStringLiteral("--"));
    m_peakCycles->setText(hasPeak ? cycles(cpu.peak[AudioEngine::CpuTotal]) : QStringLiteral("--"));
    m_reset->setEnabled(r.loaded);

    m_termName[AudioEngine::CpuSequencer]->setText(
        r.loaded && cpu.estimated ? tr("Sequencer (estimated)") : tr("Sequencer"));
    m_termName[AudioEngine::CpuCgb]->setText(tr("CGB"));
    m_termName[AudioEngine::CpuMixer]->setText(tr("Mixer"));
    for (int term = 0; term < 3; term++) {
        const int32_t n = cpu.last[term];
        m_termCycles[term]->setText(hasReading ? locale.toString(n) : QStringLiteral("--"));
        m_termPercent[term]->setText(hasReading ? QStringLiteral("%1%").arg(locale.toString(
                                                      percentOf(n, cpu.vblankCycles), 'f', 1))
                                                : QStringLiteral("--"));
    }

    // Wide, the marks in full; narrow, only the budget in cycles (the bar's
    // captions carry the rest).
    const QString budgetCycles = cycles(r.budgetCycles);
    m_marks->setText(m_wideLayout
                         ? tr("▲ peak · Budget %1 = %2 · VBlank window %3 = 100% · Frame end "
                              "%4 = %5")
                               .arg(budgetCycles, percent(r.budgetCycles), cycles(cpu.vblankCycles),
                                    cycles(cpu.frameCycles), percent(cpu.frameCycles))
                         : tr("▲ peak · Budget %1").arg(budgetCycles));
    m_config->setText(r.loaded ? tr("%1 Hz mix rate · %2 PCM channels · %3")
                                     .arg(r.mixRate)
                                     .arg(r.maxPcm)
                                     .arg(r.hqMixer ? tr("HQ mixer") : tr("stock mixer"))
                               : QString());
    m_config->setVisible(r.loaded);
    m_bar->setReading(r);
}

QString CpuPanel::percentText() const
{
    return m_percent->text();
}

QString CpuPanel::peakText() const
{
    return m_peak->text();
}

QString CpuPanel::cyclesText() const
{
    return m_cycles->text();
}

QString CpuPanel::peakCyclesText() const
{
    return m_peakCycles->text();
}

QString CpuPanel::levelName() const
{
    return m_percent->property("cpuLevel").toString();
}

QColor CpuPanel::percentColor() const
{
    return m_percent->palette().color(QPalette::WindowText);
}

QRect CpuPanel::segmentRect(int term) const
{
    return m_bar->segment(term);
}

int CpuPanel::barTrackWidth() const
{
    return m_bar->track().width();
}

int CpuPanel::budgetMarkX() const
{
    return m_bar->budgetX();
}

int CpuPanel::vblankMarkX() const
{
    return m_bar->vblankX();
}

int CpuPanel::ceilingMarkX() const
{
    return m_bar->ceilingX();
}

bool CpuPanel::wideLayoutActive() const
{
    return m_wideLayout;
}

QRect CpuPanel::readoutSectionRect() const
{
    return m_readoutBox->geometry();
}

QRect CpuPanel::breakdownSectionRect() const
{
    return m_breakdownBox->geometry();
}

QRect CpuPanel::barRect() const
{
    return QRect(m_bar->mapTo(m_breakdownBox->parentWidget(), QPoint(0, 0)), m_bar->size());
}

QStringList CpuPanel::visibleStrings() const
{
    QStringList out;
    for (const QLabel *label : findChildren<QLabel *>())
        out << label->text() << label->toolTip();
    out << m_bar->toolTip() << m_reset->text() << m_reset->toolTip();
    return out;
}

QRect CpuPanel::markRect(Mark mark) const
{
    switch (mark) {
    case MarkVblank:
        return m_bar->vblankMark();
    case MarkBudget:
        return m_bar->budgetMark();
    case MarkPeak:
        return m_bar->peakMark();
    default:
        return m_bar->ceilingMark();
    }
}

QColor CpuPanel::markColor(Mark mark) const
{
    switch (mark) {
    case MarkVblank:
        return m_bar->vblankColor();
    case MarkBudget:
        return m_bar->budgetColor();
    case MarkPeak:
        return m_bar->peakColor();
    default:
        return m_bar->ceilingColor();
    }
}

QList<QRect> CpuPanel::captionRects() const
{
    const CpuBar::Captions c = m_bar->captions();
    QList<QRect> out;
    for (const QRect &r : {c.budget, c.vblank, c.frameEnd})
        if (!r.isEmpty())
            out.append(r);
    return out;
}

QRect CpuPanel::barTrackRect() const
{
    return m_bar->track();
}

QImage CpuPanel::barImage() const
{
    return m_bar->grab().toImage();
}

QRect CpuPanel::legendRect() const
{
    return QRect(m_termsBox->mapTo(m_breakdownBox->parentWidget(), QPoint(0, 0)),
                 m_termsBox->size());
}

QString CpuPanel::marksText() const
{
    return m_marks->text();
}

int CpuPanel::hScrollRange() const
{
    return m_scroll->horizontalScrollBar()->maximum();
}

void CpuPanel::clickReset()
{
    m_reset->click();
}
