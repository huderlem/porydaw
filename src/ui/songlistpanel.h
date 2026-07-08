#pragma once

#include <QWidget>

#include "project/decompproject.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

// The Songs dock: the project's playable songs with a filter box (substring
// per word, falling back to fuzzy subsequence), ID/alphabetical ordering, and
// a category dropdown built from the labels' shared prefixes (mus_/se_/...),
// so the hundreds of entries in a real project stay navigable.
class SongListPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SongListPanel(QWidget *parent = nullptr);

    // Replaces the panel's contents (non-playable songs are dropped) while
    // keeping the current search text, sort, and — if it still exists —
    // category selection.
    void setSongs(const QVector<SongInfo> &songs);
    // Filter state, persisted across runs by MainWindow (QSettings). The
    // restored category stays pending until a project's songs are set, since
    // the combo only gains real entries then; a category the project doesn't
    // have falls back to All.
    QString searchText() const;
    int sortIndex() const;
    QString categoryPrefix() const;
    void restoreFilters(const QString &search, int sortIndex,
                        const QString &categoryPrefix);
    void focusSearch();
    // Marks the loaded song: selects it, scrolls it into view, and keeps it
    // selected across list rebuilds. -1 (or a filtered-out id) deselects.
    void setCurrentSong(int songId);

signals:
    void songActivated(int songId);
    // Context-menu "Open in New Tab" (plain activation replaces the current
    // tab's song).
    void songOpenInNewTabRequested(int songId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildCategories();
    void rebuildList();
    bool matchesFilters(const SongInfo &song) const;
    void activateItem(QListWidgetItem *item);

    QVector<SongInfo> m_songs;
    QStringList m_knownPrefixes; // categories currently in the combo
    QString m_pendingCategory;   // restored category awaiting its first rebuild
    int m_currentSongId = -1;    // the loaded song, re-selected on rebuilds
    QLineEdit *m_search = nullptr;
    QComboBox *m_category = nullptr;
    QComboBox *m_sort = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_count = nullptr;
};
