#include "samplepicker.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kSymbolRole = Qt::UserRole; // full symbol; absent on sections
constexpr int kKeysplitRole = Qt::UserRole + 1;
constexpr int kPopupMinWidth = 340;
constexpr int kPopupMaxHeight = 420;
// Looped samples ring until released; cap the browse audition so moving
// through the list doesn't leave a drone playing.
constexpr int kAuditionOffMs = 2000;
} // namespace

QString vgSampleDisplayName(const QString &symbol)
{
    // vg_set_voice_name's prefix list (voicegroup_loader.c), so the picker,
    // the button, and the voice tree all shorten symbols the same way.
    static const char *const kPrefixes[] = {
        "DirectSoundWaveData_", "ProgrammableWaveData_", "voicegroup_"};
    for (const char *prefix : kPrefixes) {
        const QLatin1String p(prefix);
        if (symbol.startsWith(p) && symbol.size() > p.size())
            return symbol.mid(p.size());
    }
    return symbol;
}

SamplePickerButton::SamplePickerButton(QWidget *parent)
    : QPushButton(parent)
{
    setObjectName(QStringLiteral("vgSamplePickerButton"));
    // The button must shrink with a narrow dock; the label elides instead.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(this, &QPushButton::clicked, this, &SamplePickerButton::openPopup);
    updateButtonText();
}

void SamplePickerButton::setChoices(const QStringList &keysplits,
                                    const QStringList &samples,
                                    const QStringList &phonemes)
{
    m_keysplits = keysplits;
    m_samples = samples;
    m_phonemes = phonemes;
}

void SamplePickerButton::setCurrentSymbol(const QString &symbol)
{
    m_currentSymbol = symbol;
    updateButtonText();
}

void SamplePickerButton::setDisplayFullSymbols(bool on)
{
    if (m_fullNames == on)
        return;
    m_fullNames = on;
    updateButtonText();
}

void SamplePickerButton::setInfoProvider(
    std::function<SamplePickInfo(const QString &)> provider)
{
    m_info = std::move(provider);
}

bool SamplePickerButton::popupVisible() const
{
    return m_popup && m_popup->isVisible();
}

void SamplePickerButton::updateButtonText()
{
    const QString name =
        m_fullNames ? m_currentSymbol : vgSampleDisplayName(m_currentSymbol);
    const int avail = width() - 24; // frame + a hint of breathing room
    setText(name.isEmpty()
                ? tr("(none)")
                : fontMetrics().elidedText(name, Qt::ElideMiddle,
                                           qMax(40, avail)));
    setToolTip(m_currentSymbol);
}

void SamplePickerButton::resizeEvent(QResizeEvent *event)
{
    QPushButton::resizeEvent(event);
    updateButtonText();
}

void SamplePickerButton::openPopup()
{
    if (!m_popup) {
        auto *frame = new QFrame(this, Qt::Popup);
        frame->setObjectName(QStringLiteral("vgSamplePickerPopup"));
        frame->setFrameShape(QFrame::StyledPanel);
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        m_search = new QLineEdit(frame);
        m_search->setObjectName(QStringLiteral("vgSamplePickerSearch"));
        m_search->setPlaceholderText(tr("Search samples…"));
        m_search->setClearButtonEnabled(true);
        layout->addWidget(m_search);

        m_list = new QTreeWidget(frame);
        m_list->setObjectName(QStringLiteral("vgSamplePickerList"));
        m_list->setColumnCount(2);
        m_list->setHeaderHidden(true);
        m_list->setRootIsDecorated(false);
        m_list->setUniformRowHeights(true);
        m_list->setIndentation(12);
        m_list->header()->setStretchLastSection(false);
        m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        // Keyboard stays in the search box (arrows are forwarded); a
        // focusable list would swallow the typing.
        m_list->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(m_list, 1);

        m_detail = new QLabel(frame);
        m_detail->setObjectName(QStringLiteral("vgSamplePickerDetail"));
        m_detail->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(m_detail);

        m_auditionOffTimer = new QTimer(this);
        m_auditionOffTimer->setSingleShot(true);
        m_auditionOffTimer->setInterval(kAuditionOffMs);
        connect(m_auditionOffTimer, &QTimer::timeout, this,
                &SamplePickerButton::auditionStopRequested);

        connect(m_search, &QLineEdit::textChanged, this,
                [this] { applyFilter(); });
        connect(m_search, &QLineEdit::returnPressed, this,
                [this] { commitItem(m_list->currentItem()); });
        connect(m_list, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem *item) { commitItem(item); });
        connect(m_list, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *item) {
                    updateDetail();
                    if (m_positioning || !item)
                        return;
                    const QString symbol = item->data(0, kSymbolRole).toString();
                    if (symbol.isEmpty() || item == m_typedRow)
                        return;
                    emit auditionRequested(symbol);
                    m_auditionOffTimer->start();
                });
        m_search->installEventFilter(this);
        frame->installEventFilter(this);
        m_popup = frame;
    }

    rebuildList();

    // Below the button, clamped to the screen; above when it won't fit.
    const int w = qMax(width(), kPopupMinWidth);
    const int h = kPopupMaxHeight;
    m_popup->resize(w, h);
    QPoint pos = mapToGlobal(QPoint(0, height()));
    if (QScreen *screen = this->screen()) {
        const QRect avail = screen->availableGeometry();
        pos.setX(qBound(avail.left(), pos.x(), avail.right() - w + 1));
        if (pos.y() + h > avail.bottom() + 1)
            pos.setY(qMax(avail.top(), mapToGlobal(QPoint(0, 0)).y() - h));
    }
    m_popup->move(pos);
    m_popup->show();
    m_search->setFocus();
}

QTreeWidgetItem *SamplePickerButton::addSection(const QString &title)
{
    auto *item = new QTreeWidgetItem(m_list, {title});
    item->setFlags(Qt::ItemIsEnabled); // a label, never current/selected
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    return item;
}

void SamplePickerButton::rebuildList()
{
    m_positioning = true;
    m_search->clear();
    m_list->clear();
    m_typedRow = nullptr;

    struct Section {
        QString title;
        const QStringList *symbols;
        bool keysplit;
    };
    const Section sections[] = {
        {tr("Keysplits"), &m_keysplits, true},
        {tr("Samples"), &m_samples, false},
        {tr("Phonemes"), &m_phonemes, false},
    };
    int nonEmpty = 0;
    for (const Section &s : sections)
        nonEmpty += s.symbols->isEmpty() ? 0 : 1;

    QTreeWidgetItem *currentRow = nullptr;
    for (const Section &s : sections) {
        if (s.symbols->isEmpty())
            continue;
        // A lone section (typically plain samples) needs no header.
        QTreeWidgetItem *parent =
            nonEmpty > 1 ? addSection(s.title) : m_list->invisibleRootItem();
        for (const QString &symbol : *s.symbols) {
            auto *row = new QTreeWidgetItem(parent);
            row->setText(0, m_fullNames ? symbol
                                        : vgSampleDisplayName(symbol));
            row->setData(0, kSymbolRole, symbol);
            row->setData(0, kKeysplitRole, s.keysplit);
            row->setToolTip(0, symbol);
            if (!s.keysplit && m_info) {
                const SamplePickInfo info = m_info(symbol);
                if (info.known && info.looped) {
                    row->setText(1, QStringLiteral("∞"));
                    row->setToolTip(1, tr("Loops"));
                }
            }
            if (symbol == m_currentSymbol)
                currentRow = row;
        }
    }
    m_list->expandAll();
    if (currentRow) {
        m_list->setCurrentItem(currentRow);
        m_list->scrollToItem(currentRow, QAbstractItemView::PositionAtCenter);
    }
    m_positioning = false;
    updateDetail();
}

void SamplePickerButton::applyFilter()
{
    const QString filter = m_search->text().trimmed();
    bool exactMatch = false;
    QTreeWidgetItem *root = m_list->invisibleRootItem();
    const auto rowMatches = [&](QTreeWidgetItem *row) {
        if (filter.isEmpty())
            return true;
        const QString symbol = row->data(0, kSymbolRole).toString();
        return row->text(0).contains(filter, Qt::CaseInsensitive)
            || symbol.contains(filter, Qt::CaseInsensitive);
    };
    const auto visitRow = [&](QTreeWidgetItem *row) {
        const bool match = rowMatches(row);
        row->setHidden(!match);
        if (match && row->data(0, kSymbolRole).toString() == filter)
            exactMatch = true;
        return match;
    };
    for (int i = 0; i < root->childCount(); i++) {
        QTreeWidgetItem *top = root->child(i);
        if (top == m_typedRow)
            continue;
        if (top->childCount() == 0) {
            visitRow(top);
            continue;
        }
        int visible = 0;
        for (int j = 0; j < top->childCount(); j++)
            visible += visitRow(top->child(j)) ? 1 : 0;
        top->setHidden(visible == 0);
    }

    // The editable combo accepted any symbol; keep that power as an explicit
    // trailing row whenever the typed text isn't already a listed symbol.
    const bool wantTyped = !filter.isEmpty() && !exactMatch;
    if (wantTyped && !m_typedRow) {
        m_typedRow = new QTreeWidgetItem(m_list);
        QFont f = m_typedRow->font(0);
        f.setItalic(true);
        m_typedRow->setFont(0, f);
    }
    if (m_typedRow) {
        m_typedRow->setHidden(!wantTyped);
        if (wantTyped) {
            m_typedRow->setText(0, tr("Use \"%1\"").arg(filter));
            m_typedRow->setData(0, kSymbolRole, filter);
        }
    }

    // Keep a live highlight on the top match so Return commits it (and the
    // audition follows the typing).
    QTreeWidgetItem *current = m_list->currentItem();
    if (!current || current->isHidden() || current == m_typedRow) {
        if (QTreeWidgetItem *first = firstSelectableRow())
            m_list->setCurrentItem(first);
    }
}

QTreeWidgetItem *SamplePickerButton::firstSelectableRow() const
{
    for (QTreeWidgetItemIterator it(m_list); *it; ++it) {
        QTreeWidgetItem *item = *it;
        if (item->isHidden() || (item->parent() && item->parent()->isHidden()))
            continue;
        if (item->data(0, kSymbolRole).toString().isEmpty())
            continue; // section label
        return item;
    }
    return nullptr;
}

void SamplePickerButton::updateDetail()
{
    QTreeWidgetItem *item = m_list ? m_list->currentItem() : nullptr;
    const QString symbol =
        item ? item->data(0, kSymbolRole).toString() : QString();
    if (symbol.isEmpty()) {
        m_detail->setText(QString());
        return;
    }
    if (item->data(0, kKeysplitRole).toBool()) {
        m_detail->setText(tr("Keysplit instrument"));
        return;
    }
    const SamplePickInfo info = m_info ? m_info(symbol) : SamplePickInfo{};
    if (!info.known) {
        m_detail->setText(item == m_typedRow ? tr("Unlisted symbol")
                                             : QString());
        return;
    }
    m_detail->setText(tr("%1 · %2 Hz · %3 s")
                          .arg(info.looped ? tr("Loops") : tr("One-shot"))
                          .arg(info.rateHz)
                          .arg(info.seconds, 0, 'f', 2));
}

void SamplePickerButton::commitItem(QTreeWidgetItem *item)
{
    if ((!item || item->isHidden()) && m_typedRow && !m_typedRow->isHidden())
        item = m_typedRow;
    if (!item || item->isHidden())
        return;
    const QString symbol = item->data(0, kSymbolRole).toString();
    if (symbol.isEmpty())
        return; // section label
    m_popup->hide();
    setCurrentSymbol(symbol);
    emit symbolPicked(symbol);
}

bool SamplePickerButton::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_popup && event->type() == QEvent::Hide) {
        m_auditionOffTimer->stop();
        emit auditionStopRequested();
    }
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QApplication::sendEvent(m_list, event);
            return true;
        default:
            break;
        }
    }
    return QPushButton::eventFilter(watched, event);
}
