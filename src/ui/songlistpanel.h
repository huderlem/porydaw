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
    void focusSearch();
    // Marks the loaded song: selects it, scrolls it into view, and keeps it
    // selected across list rebuilds. -1 (or a filtered-out id) deselects.
    void setCurrentSong(int songId);

signals:
    void songActivated(int songId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildCategories();
    void rebuildList();
    bool matchesFilters(const SongInfo &song) const;
    void activateItem(QListWidgetItem *item);

    QVector<SongInfo> m_songs;
    QStringList m_knownPrefixes; // categories currently in the combo
    int m_currentSongId = -1;    // the loaded song, re-selected on rebuilds
    QLineEdit *m_search = nullptr;
    QComboBox *m_category = nullptr;
    QComboBox *m_sort = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_count = nullptr;
};
