#pragma once

#include <QPushButton>
#include <QStringList>
#include <functional>

class QLabel;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

// Row metadata the picker displays, resolved by the owner from the project's
// committed sample files (the same WaveData the engine would play).
struct SamplePickInfo {
    bool known = false; // symbol resolved to sample data
    bool looped = false;
    int rateHz = 0;
    double seconds = 0.0; // length at the sample's own rate
};

// The symbol as shown to the user: the loader's vg_set_voice_name prefix
// stripping ("DirectSoundWaveData_sc88pro_trumpet" -> "sc88pro_trumpet").
QString vgSampleDisplayName(const QString &symbol);

// The Sample field of the voicegroup editor: a button showing the current
// sample's display name; clicking opens a searchable popup of the project's
// samples (sectioned into keysplits/samples/phonemes) with loop badges,
// audition-on-highlight, and a "use typed symbol" fallback so unknown
// symbols can still be entered, like the editable combo it replaces.
class SamplePickerButton : public QPushButton
{
    Q_OBJECT

public:
    explicit SamplePickerButton(QWidget *parent = nullptr);

    // The three sections, each already sorted. Keysplit instruments audition
    // as nothing (no single sample to play) but commit like any symbol.
    void setChoices(const QStringList &keysplits, const QStringList &samples,
                    const QStringList &phonemes);
    void setCurrentSymbol(const QString &symbol);
    QString currentSymbol() const { return m_currentSymbol; }

    // Loop badge / detail line lookup. May be empty (no badges, no details).
    void setInfoProvider(std::function<SamplePickInfo(const QString &)> provider);

    // Opens the popup (also the click handler; public for the harnesses).
    void openPopup();
    bool popupVisible() const;

signals:
    // A choice was committed (list row or typed symbol). The owner treats it
    // like the old combo's activated(): adopt the symbol and commit the edit.
    void symbolPicked(const QString &symbol);
    // Highlight moved onto a sample row: play it so browsing is audible.
    void auditionRequested(const QString &symbol);
    void auditionStopRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildList();
    void applyFilter();
    void updateDetail();
    void commitItem(QTreeWidgetItem *item);
    void updateButtonText();
    QTreeWidgetItem *addSection(const QString &title);
    QTreeWidgetItem *firstSelectableRow() const;

    QStringList m_keysplits, m_samples, m_phonemes;
    QString m_currentSymbol;
    std::function<SamplePickInfo(const QString &)> m_info;

    QWidget *m_popup = nullptr; // created on first open
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_detail = nullptr;
    QTreeWidgetItem *m_typedRow = nullptr; // "Use typed symbol" fallback
    QTimer *m_auditionOffTimer = nullptr;
    bool m_positioning = false; // suppress audition while (re)building
};
