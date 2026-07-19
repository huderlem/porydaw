#include "ui/polyphonypanel.h"

#include <algorithm>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core/miditimeline.h"
#include "ui/m4asemantics.h"

namespace {

constexpr int kLogCap = 500;      // event-log rows kept
constexpr qint64 kFlashMs = 1000; // row flash fade time

// Cell colors matching the CLAP plugin's channel_cell(): free (dark), active
// (green), releasing (amber), lost sound playing on a shadow channel (blue).
const QColor kCellColors[4] = {
    QColor(51, 51, 59),
    QColor(38, 140, 56),
    QColor(184, 135, 26),
    QColor(41, 97, 191),
};

// Bar:beat.tick position for an event, honoring time-signature changes the
// way the ruler grid does (SongView::gridSegAt): a signature change restarts
// the bar at its own tick, and a partial bar before it still counts as a bar.
// 4/4 is assumed before the first (or without any) signature.
QString formatBarBeat(const MidiTimeline *tl, uint32_t tick)
{
    if (!tl)
        return QString::number(tick);
    const uint64_t tpb = tl->ticksPerBeat ? tl->ticksPerBeat : 24;
    uint64_t segStart = 0;
    uint64_t num = 4;
    uint32_t denomPow2 = 2;
    uint64_t bar = 1;
    for (const TimeSigPoint &sig : tl->timeSigs) {
        if (sig.tick > tick)
            break;
        const uint64_t beatLen = std::max<uint64_t>(1, (tpb * 4) >> denomPow2);
        const uint64_t barLen = num * beatLen;
        bar += (sig.tick - segStart + barLen - 1) / barLen;
        segStart = sig.tick;
        num = sig.numerator ? sig.numerator : 4;
        denomPow2 = sig.denomPow2;
    }
    const uint64_t beatLen = std::max<uint64_t>(1, (tpb * 4) >> denomPow2);
    const uint64_t barLen = num * beatLen;
    const uint64_t rel = tick - segStart;
    return QStringLiteral("%1:%2.%3")
        .arg(bar + rel / barLen)
        .arg(rel % barLen / beatLen + 1)
        .arg(rel % barLen % beatLen);
}

} // namespace

// The live channel-usage grid: one cell per real PCM channel (up to the
// engine's limit) and per CGB channel, plus the shadow pool while invert mode
// is on. Cells wrap to the dock's width (the CLAP tab lays them in one row,
// but a dock is narrow).
class PolyChannelGrid : public QWidget
{
public:
    explicit PolyChannelGrid(QWidget *parent = nullptr) : QWidget(parent)
    {
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    void setSnapshot(const AudioEngine::PolySnapshot &snap)
    {
        const bool relayout = snap.maxPcmChannels != m_snap.maxPcmChannels
            || snap.invert != m_snap.invert;
        m_snap = snap;
        if (relayout)
            updateGeometry();
        update();
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return layoutHeight(width); }
    QSize sizeHint() const override
    {
        return QSize(kCellW * 5 + kGap * 4, layoutHeight(kCellW * 5 + kGap * 4));
    }
    QSize minimumSizeHint() const override
    {
        return QSize(kCellW + kGap, kCellH);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        int y = 0;
        paintGroup(&p, y, tr("PCM"), m_snap.pcm, m_snap.maxPcmChannels, false,
                   false);
        paintGroup(&p, y, tr("CGB"), m_snap.cgb, MAX_CGB_CHANNELS, true, false);
        if (m_snap.invert) {
            paintCaption(&p, y, tr("Lost sounds currently playing (solo overflow):"));
            paintGroup(&p, y, tr("PCM"), m_snap.pcm + MAX_PCM_CHANNELS,
                       MAX_PCM_CHANNELS, false, true);
            paintGroup(&p, y, tr("CGB"), m_snap.cgb + MAX_CGB_CHANNELS,
                       MAX_CGB_CHANNELS, true, true);
        }
    }

private:
    static constexpr int kCellW = 46;
    static constexpr int kCellH = 34;
    static constexpr int kGap = 4;
    static constexpr int kCaptionH = 18;

    int cellsPerRow(int width) const
    {
        return std::max(1, (width + kGap) / (kCellW + kGap));
    }

    int groupHeight(int count, int width) const
    {
        const int rows = (count + cellsPerRow(width) - 1) / cellsPerRow(width);
        return kCaptionH + rows * (kCellH + kGap);
    }

    int layoutHeight(int width) const
    {
        int h = groupHeight(std::max<int>(m_snap.maxPcmChannels, 1), width)
            + groupHeight(MAX_CGB_CHANNELS, width);
        if (m_snap.invert) {
            h += kCaptionH + groupHeight(MAX_PCM_CHANNELS, width)
                + groupHeight(MAX_CGB_CHANNELS, width);
        }
        return h;
    }

    void paintCaption(QPainter *p, int &y, const QString &text)
    {
        p->setPen(palette().color(QPalette::Disabled, QPalette::Text));
        p->drawText(QRect(0, y, width(), kCaptionH),
                    Qt::AlignLeft | Qt::AlignVCenter, text);
        y += kCaptionH;
    }

    void paintGroup(QPainter *p, int &y, const QString &caption,
                    const AudioEngine::PolyChannel *channels, int count,
                    bool isCgb, bool shadow)
    {
        static const char *cgbNames[MAX_CGB_CHANNELS] = {"Sq1", "Sq2", "Wave",
                                                         "Noise"};
        paintCaption(p, y, caption);
        const int perRow = cellsPerRow(width());
        for (int i = 0; i < count; i++) {
            const AudioEngine::PolyChannel &ch = channels[i];
            const int x = (i % perRow) * (kCellW + kGap);
            const int cy = y + (i / perRow) * (kCellH + kGap);
            const int state = !ch.on ? 0 : shadow ? 3 : ch.releasing ? 2 : 1;
            QString label;
            if (isCgb) {
                label = QString::fromLatin1(cgbNames[i]) + QLatin1Char('\n')
                    + (ch.on ? QStringLiteral("T%1 %2").arg(ch.track + 1).arg(
                           midiKeyName(ch.midiKey))
                             : QStringLiteral("--"));
            } else {
                label = ch.on ? QStringLiteral("T%1\n%2").arg(ch.track + 1).arg(
                            midiKeyName(ch.midiKey))
                              : QStringLiteral("--");
            }
            const QRect rect(x, cy, kCellW, kCellH);
            p->setPen(Qt::NoPen);
            p->setBrush(kCellColors[state]);
            p->drawRoundedRect(rect, 3, 3);
            p->setPen(state == 0 ? QColor(140, 140, 148) : QColor(240, 240, 244));
            QFont f = font();
            f.setPixelSize(10);
            p->setFont(f);
            p->drawText(rect, Qt::AlignCenter, label);
        }
        y += ((count + perRow - 1) / perRow) * (kCellH + kGap);
    }

    AudioEngine::PolySnapshot m_snap;
};

PolyphonyPanel::PolyphonyPanel(QWidget *parent) : QWidget(parent)
{
    m_clock.start();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_invert = new QCheckBox(tr("Solo overflow (invert audio)"), this);
    m_invert->setToolTip(tr("Mutes normal playback and makes ONLY the sounds "
                            "lost to the polyphony limit audible."));
    connect(m_invert, &QCheckBox::toggled, this, &PolyphonyPanel::invertToggled);
    layout->addWidget(m_invert);

    auto *usageLabel = new QLabel(tr("Channel usage"), this);
    usageLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(usageLabel);
    m_grid = new PolyChannelGrid(this);
    layout->addWidget(m_grid);

    auto *tableHeader = new QHBoxLayout;
    auto *tableLabel = new QLabel(tr("Overflow by track"), this);
    tableLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    tableHeader->addWidget(tableLabel);
    tableHeader->addStretch();
    m_reset = new QPushButton(tr("Reset"), this);
    connect(m_reset, &QPushButton::clicked, this, [this] {
        // The engine reset lands at the next audio callback; the log and
        // flash state clear immediately (the eventTotal drop the next
        // snapshot reports would clear them anyway).
        clearRuntimeState();
        m_log->clear();
        m_table->setRowCount(0);
        m_tableEmpty->show();
        emit resetRequested();
    });
    tableHeader->addWidget(m_reset);
    layout->addLayout(tableHeader);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Track"), tr("Dropped"), tr("Cut Off"), tr("Tail Cut")});
    const QString columnHelp[4] = {
        tr("Track whose sound was lost."),
        tr("Notes that never played at all: every channel was in use and none "
           "had low enough priority to steal. The most audible kind of "
           "overflow because the note is simply missing."),
        tr("Notes that were still sounding when a newer note stole their "
           "channel, cutting them off abruptly before their note-off."),
        tr("Notes that were already fading out (released) when a newer note "
           "reused their channel. This shortens the fade-out tail and is "
           "usually the least audible kind of overflow."),
    };
    for (int c = 0; c < 4; c++)
        m_table->horizontalHeaderItem(c)->setToolTip(columnHelp[c]);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; c++)
        m_table->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setMinimumHeight(90);
    layout->addWidget(m_table, 1);
    m_tableEmpty = new QLabel(tr("No overflow recorded"), this);
    m_tableEmpty->setEnabled(false);
    layout->addWidget(m_tableEmpty);

    auto *logLabel = new QLabel(tr("Recent events"), this);
    logLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(logLabel);
    m_log = new QListWidget(this);
    m_log->setSelectionMode(QAbstractItemView::SingleSelection);
    m_log->setToolTip(tr("Double-click an event to jump to its position."));
    connect(m_log, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { activateLogRow(m_log->row(item)); });
    layout->addWidget(m_log, 2);
}

void PolyphonyPanel::setTimeline(const MidiTimeline *timeline)
{
    m_timeline = timeline;
}

void PolyphonyPanel::setTrackNames(const QStringList &names)
{
    m_trackNames = names;
}

void PolyphonyPanel::setVoiceNames(const QStringList &names)
{
    m_voiceNames = names;
}

void PolyphonyPanel::clearSession()
{
    m_timeline = nullptr;
    m_trackNames.clear();
    m_voiceNames.clear();
    clearRuntimeState();
    m_log->clear();
    m_table->setRowCount(0);
    m_tableEmpty->show();
    m_grid->setSnapshot(AudioEngine::PolySnapshot{});
}

void PolyphonyPanel::clearRuntimeState()
{
    m_prevValid = false;
    m_lastSeenTotal = 0;
    for (int t = 0; t < MAX_TRACKS; t++)
        m_flashMs[t] = 0;
}

bool PolyphonyPanel::invertChecked() const
{
    return m_invert->isChecked();
}

int PolyphonyPanel::logRowCount() const
{
    return m_log->count();
}

QString PolyphonyPanel::logRowText(int row) const
{
    const QListWidgetItem *item = m_log->item(row);
    return item ? item->text() : QString();
}

void PolyphonyPanel::activateLogRow(int row)
{
    const QListWidgetItem *item = m_log->item(row);
    if (!item)
        return;
    const QVariant tick = item->data(Qt::UserRole);
    if (tick.isValid())
        emit jumpToEvent(tick.toULongLong(), item->data(Qt::UserRole + 1).toInt(),
                         item->data(Qt::UserRole + 2).toInt());
}

void PolyphonyPanel::updateSnapshot(const AudioEngine::PolySnapshot &snap)
{
    // A total below the drained mark means the stats were reset (play start,
    // or the Reset button): the log and counters describe the same run, so
    // both restart together. Re-arming m_prevValid also keeps the first
    // post-reset counters from flashing.
    if (snap.eventTotal < m_lastSeenTotal) {
        clearRuntimeState();
        m_log->clear();
    }

    // Drain new ring entries, oldest first so insertion at row 0 leaves the
    // newest on top. Entries older than the ring holds were overwritten.
    uint32_t first = m_lastSeenTotal;
    if (snap.eventTotal > M4A_POLY_EVENT_CAPACITY)
        first = std::max(first, snap.eventTotal - M4A_POLY_EVENT_CAPACITY);
    for (uint32_t i = first; i < snap.eventTotal; i++)
        appendEvent(snap.events[i % M4A_POLY_EVENT_CAPACITY]);
    m_lastSeenTotal = snap.eventTotal;
    while (m_log->count() > kLogCap)
        delete m_log->takeItem(m_log->count() - 1);

    refreshTable(snap);
    m_grid->setSnapshot(snap);
}

void PolyphonyPanel::appendEvent(const M4APolyEvent &ev)
{
    const QString pos = ev.tick == M4A_POLY_TICK_NONE
        ? tr("live")
        : formatBarBeat(m_timeline, ev.tick);
    QString voice = m_voiceNames.value(ev.program).trimmed();
    if (voice.isEmpty())
        voice = tr("voice %1").arg(ev.program);
    const QString who = QStringLiteral("Trk %1  %2 (%3)")
                            .arg(ev.trackIndex + 1)
                            .arg(midiKeyName(ev.midiKey), voice);
    QString text;
    QColor color;
    switch (ev.type) {
    case M4A_POLY_DROPPED:
        text = tr("%1 | %2: dropped (no channel available)").arg(pos, who);
        color = QColor(219, 88, 88);
        break;
    case M4A_POLY_STOLEN:
        text = tr("%1 | %2: cut off by Trk %3")
                   .arg(pos, who)
                   .arg(ev.byTrack + 1);
        color = QColor(219, 155, 66);
        break;
    default:
        text = tr("%1 | %2: release tail cut by Trk %3")
                   .arg(pos, who)
                   .arg(ev.byTrack + 1);
        color = palette().color(QPalette::Disabled, QPalette::Text);
        break;
    }
    auto *item = new QListWidgetItem(text);
    item->setForeground(color);
    if (ev.tick != M4A_POLY_TICK_NONE) {
        item->setData(Qt::UserRole, qulonglong(ev.tick));
        item->setData(Qt::UserRole + 1, int(ev.trackIndex));
        item->setData(Qt::UserRole + 2, int(ev.midiKey));
    }
    m_log->insertItem(0, item);
}

void PolyphonyPanel::refreshTable(const AudioEngine::PolySnapshot &snap)
{
    const qint64 now = m_clock.elapsed();
    bool changed = !m_prevValid;
    for (int t = 0; t < MAX_TRACKS; t++) {
        if (m_prevValid
            && (snap.drop[t] > m_prevDrop[t] || snap.steal[t] > m_prevSteal[t]
                || snap.tailCut[t] > m_prevTailCut[t]))
            m_flashMs[t] = now;
        if (snap.drop[t] != m_prevDrop[t] || snap.steal[t] != m_prevSteal[t]
            || snap.tailCut[t] != m_prevTailCut[t])
            changed = true;
        m_prevDrop[t] = snap.drop[t];
        m_prevSteal[t] = snap.steal[t];
        m_prevTailCut[t] = snap.tailCut[t];
    }
    m_prevValid = true;

    // Rebuilding 16 rows at the ui-tick rate is cheap, but skip it entirely
    // while nothing is changing and no flash is fading.
    // The window is slightly longer than the fade so an expired flash gets
    // one more rebuild to clear its residual background.
    bool flashing = false;
    for (int t = 0; t < MAX_TRACKS; t++) {
        if (m_flashMs[t] > 0 && now - m_flashMs[t] < kFlashMs + 100)
            flashing = true;
    }
    if (!changed && !flashing && m_table->rowCount() > 0)
        return;

    int row = 0;
    for (int t = 0; t < MAX_TRACKS; t++) {
        if (snap.drop[t] == 0 && snap.steal[t] == 0 && snap.tailCut[t] == 0)
            continue;
        if (m_table->rowCount() <= row)
            m_table->insertRow(row);
        QString name = m_trackNames.value(t).trimmed();
        if (name.isEmpty())
            name = tr("Track %1").arg(t + 1);
        const QString cells[4] = {name, QString::number(snap.drop[t]),
                                  QString::number(snap.steal[t]),
                                  QString::number(snap.tailCut[t])};
        QColor bg = Qt::transparent;
        if (m_flashMs[t] > 0 && now - m_flashMs[t] < kFlashMs) {
            bg = QColor(217, 38, 38);
            bg.setAlphaF(0.55f * (1.0f - float(now - m_flashMs[t]) / kFlashMs));
        }
        for (int c = 0; c < 4; c++) {
            auto *item = m_table->item(row, c);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(row, c, item);
            }
            if (item->text() != cells[c])
                item->setText(cells[c]);
            item->setBackground(bg);
        }
        row++;
    }
    m_table->setRowCount(row);
    m_tableEmpty->setVisible(row == 0);
}
