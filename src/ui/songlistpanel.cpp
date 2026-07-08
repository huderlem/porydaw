#include "songlistpanel.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Combo data for the synthetic categories.
const QString kAllCategory;
const QString kOtherCategory = QStringLiteral("<other>");

// A song's category is its label up to and including the first underscore
// ("mus_", "se_", ...); labels without one fall into the Other bucket.
QString prefixOf(const QString &label)
{
    const int underscore = label.indexOf(QLatin1Char('_'));
    return underscore > 0 ? label.left(underscore + 1) : QString();
}

// Friendly names for the prefixes every Gen 3 decomp shares; anything else is
// shown as the raw prefix so unusual projects still get their categories.
QString categoryName(const QString &prefix)
{
    if (prefix == QLatin1String("mus_"))
        return SongListPanel::tr("Music (mus_)");
    if (prefix == QLatin1String("se_"))
        return SongListPanel::tr("Sound effects (se_)");
    if (prefix == QLatin1String("ph_"))
        return SongListPanel::tr("Bard phonemes (ph_)");
    return prefix + QLatin1Char('*');
}

bool isSubsequence(const QString &needle, const QString &hay)
{
    int i = 0;
    for (const QChar c : hay) {
        if (i < needle.size() && c == needle.at(i))
            i++;
    }
    return i == needle.size();
}

} // namespace

SongListPanel::SongListPanel(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("songListSearch")); // findChild for tests
    m_search->setPlaceholderText(tr("Filter songs (Ctrl+F)"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &SongListPanel::rebuildList);
    // Enter loads the selected match (or the first one) without leaving the box.
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        activateItem(m_list->currentItem() ? m_list->currentItem() : m_list->item(0));
    });
    layout->addWidget(m_search);

    auto *filters = new QHBoxLayout;
    filters->setSpacing(4);
    m_category = new QComboBox(this);
    m_category->setObjectName(QStringLiteral("songListCategory"));
    m_category->addItem(tr("All"), kAllCategory);
    connect(m_category, &QComboBox::currentIndexChanged, this,
            &SongListPanel::rebuildList);
    filters->addWidget(m_category, 1);
    m_sort = new QComboBox(this);
    m_sort->setObjectName(QStringLiteral("songListSort"));
    m_sort->addItem(tr("ID order"));
    m_sort->addItem(tr("A–Z"));
    m_sort->setToolTip(tr("Sort by song ID or alphabetically"));
    connect(m_sort, &QComboBox::currentIndexChanged, this,
            &SongListPanel::rebuildList);
    filters->addWidget(m_sort);
    layout->addLayout(filters);

    m_list = new QListWidget(this);
    connect(m_list, &QListWidget::itemActivated, this, &SongListPanel::activateItem);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QListWidgetItem *item = m_list->itemAt(pos);
                if (!item)
                    return;
                const int songId = item->data(Qt::UserRole).toInt();
                QMenu menu(this);
                menu.addAction(tr("Open"), this,
                               [this, songId] { emit songActivated(songId); });
                menu.addAction(tr("Open in New Tab"), this, [this, songId] {
                    emit songOpenInNewTabRequested(songId);
                });
                menu.exec(m_list->viewport()->mapToGlobal(pos));
            });
    layout->addWidget(m_list, 1);

    m_count = new QLabel(this);
    m_count->setEnabled(false); // dimmed, it's a caption
    layout->addWidget(m_count);

    // Focusing the panel means the list — arrows browse songs right away.
    setFocusProxy(m_list);
    // Bare Space stays the window-level play/pause toggle everywhere in the
    // panel (same convention as the voicegroup editor's inputs — see
    // eventFilter). Filter queries never need a literal space: labels have
    // none and the fuzzy fallback covers multi-part queries.
    for (QWidget *w :
         std::initializer_list<QWidget *>{m_search, m_category, m_sort, m_list})
        w->installEventFilter(this);
}

void SongListPanel::setSongs(const QVector<SongInfo> &songs)
{
    m_songs.clear();
    for (const SongInfo &song : songs) {
        if (song.isPlayable())
            m_songs.append(song);
    }
    rebuildCategories();
    rebuildList();
}

QString SongListPanel::searchText() const
{
    return m_search->text();
}

int SongListPanel::sortIndex() const
{
    return m_sort->currentIndex();
}

QString SongListPanel::categoryPrefix() const
{
    // Before any project opened, the restored category is still pending —
    // report it, not the combo's placeholder All, so a project-less run
    // doesn't wipe the remembered category.
    return m_pendingCategory.isEmpty() ? m_category->currentData().toString()
                                       : m_pendingCategory;
}

void SongListPanel::restoreFilters(const QString &search, int sortIndex,
                                   const QString &categoryPrefix)
{
    m_pendingCategory = categoryPrefix;
    if (sortIndex >= 0 && sortIndex < m_sort->count())
        m_sort->setCurrentIndex(sortIndex);
    m_search->setText(search);
}

void SongListPanel::focusSearch()
{
    m_search->setFocus();
    m_search->selectAll();
}

void SongListPanel::setCurrentSong(int songId)
{
    m_currentSongId = songId;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(Qt::UserRole).toInt() == songId) {
            m_list->setCurrentItem(item);
            m_list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
    m_list->setCurrentItem(nullptr);
}

// Up/Down (and paging) in the search box steer the list, so
// type-arrow-Enter works without a focus dance.
bool SongListPanel::eventFilter(QObject *watched, QEvent *event)
{
    // Leave plain Space unaccepted so the window-level play/pause shortcut
    // fires instead of the input inserting a space / toggling.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Space
            && keyEvent->modifiers() == Qt::NoModifier) {
            keyEvent->ignore();
            return true;
        }
    }
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_PageUp
            || key == Qt::Key_PageDown) {
            QCoreApplication::sendEvent(m_list, event);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SongListPanel::rebuildCategories()
{
    QMap<QString, int> counts;
    for (const SongInfo &song : m_songs)
        counts[prefixOf(song.label)]++;

    // Prefixes with at least two songs become categories, biggest first;
    // singletons and underscore-less labels pool into Other.
    m_knownPrefixes.clear();
    int other = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (!it.key().isEmpty() && it.value() >= 2)
            m_knownPrefixes.append(it.key());
        else
            other += it.value();
    }
    std::sort(m_knownPrefixes.begin(), m_knownPrefixes.end(),
              [&counts](const QString &a, const QString &b) {
                  if (counts[a] != counts[b])
                      return counts[a] > counts[b];
                  return a < b;
              });

    const QString previous = m_pendingCategory.isEmpty()
                                 ? m_category->currentData().toString()
                                 : m_pendingCategory;
    m_pendingCategory.clear(); // one shot: a missing category falls back to All
    QSignalBlocker blocker(m_category); // rebuildList runs once, after
    m_category->clear();
    m_category->addItem(tr("All (%1)").arg(m_songs.size()), kAllCategory);
    for (const QString &prefix : m_knownPrefixes)
        m_category->addItem(
            QStringLiteral("%1 (%2)").arg(categoryName(prefix)).arg(counts[prefix]),
            prefix);
    if (other > 0)
        m_category->addItem(tr("Other (%1)").arg(other), kOtherCategory);
    const int keep = m_category->findData(previous);
    m_category->setCurrentIndex(keep >= 0 ? keep : 0);
}

bool SongListPanel::matchesFilters(const SongInfo &song) const
{
    const QString category = m_category->currentData().toString();
    if (category == kOtherCategory) {
        if (m_knownPrefixes.contains(prefixOf(song.label)))
            return false;
    } else if (!category.isEmpty() && !song.label.startsWith(category)) {
        return false;
    }

    const QString query = m_search->text().trimmed().toLower();
    if (query.isEmpty())
        return true;
    const QString hay =
        song.label.toLower() + QLatin1Char(' ') + song.constant.toLower();
    const QStringList words = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (std::all_of(words.begin(), words.end(),
                    [&hay](const QString &w) { return hay.contains(w); }))
        return true;
    // Fuzzy fallback: "musrival" finds mus_rival.
    return words.size() == 1
        && (isSubsequence(query, song.label.toLower())
            || isSubsequence(query, song.constant.toLower()));
}

void SongListPanel::rebuildList()
{
    QVector<const SongInfo *> visible;
    for (const SongInfo &song : m_songs) {
        if (matchesFilters(song))
            visible.append(&song);
    }
    if (m_sort->currentIndex() == 1) {
        std::sort(visible.begin(), visible.end(),
                  [](const SongInfo *a, const SongInfo *b) {
                      const int cmp = QString::compare(a->label, b->label,
                                                       Qt::CaseInsensitive);
                      return cmp != 0 ? cmp < 0 : a->id < b->id;
                  });
    }

    m_list->clear();
    for (const SongInfo *song : visible) {
        QString text = song->label;
        if (!song->registered)
            text += tr("  ⚠ not registered");
        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, song->id);
        if (!song->registered) {
            item->setForeground(QColor(0xc0, 0x80, 0x30));
            item->setToolTip(tr("This song's .mid exists but song_table.inc has no "
                                "entry. Open it and use File → Register Song."));
        }
    }

    m_count->setText(visible.size() == m_songs.size()
                         ? tr("%1 songs").arg(m_songs.size())
                         : tr("%1 of %2 songs").arg(visible.size()).arg(m_songs.size()));

    // Keep the loaded song selected across rebuilds — except mid-search,
    // where the selection must stay clear so Enter takes the first match.
    if (m_currentSongId >= 0 && m_search->text().trimmed().isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(Qt::UserRole).toInt() == m_currentSongId) {
                m_list->setCurrentItem(m_list->item(i));
                break;
            }
        }
    }
}

void SongListPanel::activateItem(QListWidgetItem *item)
{
    if (item)
        emit songActivated(item->data(Qt::UserRole).toInt());
}
