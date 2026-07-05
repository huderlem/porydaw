#pragma once

#include <QHash>
#include <QTreeWidget>
#include <QWidget>

extern "C" {
#include "voicegroup_loader.h"
}

#include "project/voicegroupsource.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

// The voicegroup dock (SPEC §6.1): the current song's 128 voicegroup entries
// with press-and-hold audition, plus an editor panel for the selected voice.
// Basic voice types (sample/square/wave/noise) are editable when a source
// file was located; keysplit/drumkit/cry voices stay read-only.
class VoicegroupBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit VoicegroupBrowser(QWidget *parent = nullptr);

    // vg may be nullptr (no song loaded). Not owned; the caller must clear it
    // (setVoicegroup(nullptr)) before the voicegroup is freed.
    void setVoicegroup(const LoadedVoiceGroup *vg, const QString &title = QString());

    // The editable source model behind the displayed voicegroup, or nullptr
    // when none could be located (editor shows why). Not owned; clear before
    // the source is destroyed. The symbol lists feed the sample/wave combos;
    // keysplit instruments appear at the top of the sample list.
    void setSource(VoicegroupSource *source, const QStringList &sampleSymbols,
                   const QStringList &waveSymbols,
                   const QList<QPair<QString, QString>> &keysplits);

    int currentSlot() const;
    void selectSlot(int slot);

signals:
    // velocity 0 releases. Routed to AudioEngine::previewVoice.
    void auditionVoice(int voice, int key, int velocity);
    // The selected voice's fields changed in the source model. structural
    // means scalar ToneData pokes aren't enough (type or symbol changed) and
    // the voicegroup needs a reload from rendered source to audition.
    void voiceEdited(int slot, bool structural);
    void saveRequested();
    void revertRequested();
    void newVoicegroupRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void pressedVoice(QTreeWidgetItem *item);
    void releaseVoice();
    void populateEditor();
    void commitEdit();
    void updateRow(int slot);
    void refreshButtons();
    void setEditorRowsVisible(VgMacro macro, bool visible);

    QLabel *m_title = nullptr;
    QTreeWidget *m_tree = nullptr;
    int m_soundingVoice = -1;

    const LoadedVoiceGroup *m_vg = nullptr;
    VoicegroupSource *m_source = nullptr;
    QStringList m_sampleChoices; // keysplits, then samples, then phonemes
    QStringList m_waveSymbols;
    QHash<QString, QString> m_keysplitTables; // sub-voicegroup -> table
    bool m_updating = false;

    QWidget *m_editor = nullptr;
    QLabel *m_notice = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QComboBox *m_symbolCombo = nullptr;
    QSpinBox *m_sweepSpin = nullptr;
    QComboBox *m_dutyCombo = nullptr;
    QComboBox *m_periodCombo = nullptr;
    QSpinBox *m_attackSpin = nullptr;
    QSpinBox *m_decaySpin = nullptr;
    QSpinBox *m_sustainSpin = nullptr;
    QSpinBox *m_releaseSpin = nullptr;
    QLabel *m_typeLabel = nullptr;
    QLabel *m_symbolLabel = nullptr;
    QLabel *m_sweepLabel = nullptr;
    QLabel *m_dutyLabel = nullptr;
    QLabel *m_periodLabel = nullptr;
    QLabel *m_adsrLabel = nullptr;
    QWidget *m_adsrRow = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_revertButton = nullptr;
    QPushButton *m_newButton = nullptr;
};
