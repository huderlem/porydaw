#pragma once

#include <QHash>
#include <QTreeWidget>
#include <QWidget>
#include <functional>

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
// Basic voice types (sample/square/wave/noise) plus keysplit, drumkit, and
// Golden Sun synth voices are editable when a source file was located; cry
// voices stay read-only.
class VoicegroupBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit VoicegroupBrowser(QWidget *parent = nullptr);

    // vg may be nullptr (no song loaded). Not owned; the caller must clear it
    // (setVoicegroup(nullptr)) before the voicegroup is freed.
    void setVoicegroup(const LoadedVoiceGroup *vg);

    // The selector at the top of the dock: the project's -G args (editable,
    // so unknown/new symbols still work) and the song's current one. Choices
    // are kept if the list is unchanged; the current arg never emits back.
    void setVoicegroupChoices(const QStringList &args);
    void setCurrentVoicegroupArg(const QString &arg);

    // The editable source model behind the displayed voicegroup, or nullptr
    // when none could be located (editor shows why). Not owned; clear before
    // the source is destroyed. The symbol lists feed the sample/wave/drumkit
    // combos; keysplit instruments appear at the top of the sample list.
    // adsrDefaults seeds the envelope a voice adopts on a family-crossing
    // type change (project-typical values; see VoicegroupSource::typicalAdsr).
    // synths lists the project's on-disk Golden Sun synth instruments (the
    // definition dropdown); pendingSynths are minted-but-unsaved definitions
    // (looked up, never listed — they persist only when their voicegroup
    // saves). mintSynth resolves an edited descriptor to a symbol without
    // touching disk, returning "" on failure after reporting the error
    // itself.
    void setSource(VoicegroupSource *source, const QStringList &sampleSymbols,
                   const QStringList &waveSymbols,
                   const QList<QPair<QString, QString>> &keysplits,
                   const QStringList &drumkits,
                   const VgAdsrDefaults &adsrDefaults = VgAdsrDefaults(),
                   const VgSynthCatalog &synths = VgSynthCatalog(),
                   const QHash<QString, VgSynthDesc> &pendingSynths = {},
                   std::function<QString(const VgSynthDesc &)> mintSynth = {});

    int currentSlot() const;
    void selectSlot(int slot);
    // The source's voice at slot changed outside the editor (an undo/redo,
    // or the owner applying a requested edit): refresh the slot's row, and
    // the editor panel when it currently shows that slot.
    void voiceChanged(int slot);

signals:
    // velocity 0 releases. Routed to AudioEngine::previewVoice.
    void auditionVoice(int voice, int key, int velocity);
    // The user edited the selected voice. The browser does not touch the
    // source itself: the owner applies the edit (as a song undo command) and
    // reflects it back via voiceChanged / a full setVoicegroup. structural
    // means scalar ToneData pokes aren't enough (type or symbol changed) and
    // the voicegroup needs a reload from rendered source to audition.
    void voiceEditRequested(int slot, const VgVoice &voice, bool structural);
    void newVoicegroupRequested();
    // The user picked/typed a different voicegroup arg in the selector. The
    // browser does not switch anything itself: the owner commits it as an
    // undoable cfg edit and reflects it back via setCurrentVoicegroupArg.
    void voicegroupChangeRequested(const QString &arg);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void pressedVoice(QTreeWidgetItem *item);
    void releaseVoice();
    void populateEditor();
    void commitEdit();
    void updateRow(int slot);
    void setEditorRowsVisible(VgMacro macro, bool synth, bool visible);
    // Whether the voice at slot is a Golden Sun synth, filling desc. Known
    // synth symbols answer directly; zero-size samples (.bin descriptors with
    // no set_synth entry) classify from the loaded ToneData.
    bool synthDescFor(const VgVoice &voice, int slot, VgSynthDesc *desc) const;

    QComboBox *m_vgCombo = nullptr;
    QString m_vgArg;         // the arg the selector currently stands at
    QStringList m_vgChoices; // last list handed to setVoicegroupChoices
    QTreeWidget *m_tree = nullptr;
    int m_soundingVoice = -1;

    const LoadedVoiceGroup *m_vg = nullptr;
    VoicegroupSource *m_source = nullptr;
    QStringList m_sampleChoices; // keysplits, then samples, then phonemes
    QStringList m_waveSymbols;
    QStringList m_drumkitChoices; // sub-voicegroups used as drumkits
    QHash<QString, QString> m_keysplitTables; // sub-voicegroup -> table
    VgSynthCatalog m_synths; // on-disk definitions only (the dropdown)
    // Symbol lookup: on-disk definitions plus pending (unsaved) ones.
    QHash<QString, VgSynthDesc> m_synthBySymbol;
    std::function<QString(const VgSynthDesc &)> m_mintSynth;
    VgAdsrDefaults m_adsrDefaults;
    // The envelope each slot last had in each family, so switching a voice's
    // type away and back restores it. Keyed slot -> vgAdsrFamily(); survives
    // the setSource() that follows every structural edit and resets when the
    // source object changes (a different voicegroup/song was loaded).
    QHash<int, QHash<int, VgAdsr>> m_adsrHistory;
    bool m_updating = false;

    QWidget *m_editor = nullptr;
    QLabel *m_notice = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QComboBox *m_symbolCombo = nullptr;
    QWidget *m_sweepRow = nullptr;
    QSpinBox *m_sweepTimeSpin = nullptr;
    QComboBox *m_sweepDirCombo = nullptr;
    QSpinBox *m_sweepShiftSpin = nullptr;
    QComboBox *m_dutyCombo = nullptr;
    QComboBox *m_periodCombo = nullptr;
    QComboBox *m_synthWaveCombo = nullptr;
    QWidget *m_synthParamsRow = nullptr;
    QSpinBox *m_synthDutySpin = nullptr;
    QSpinBox *m_synthStepSpin = nullptr;
    QSpinBox *m_synthDepthSpin = nullptr;
    QSpinBox *m_synthPhaseSpin = nullptr;
    QSpinBox *m_attackSpin = nullptr;
    QSpinBox *m_decaySpin = nullptr;
    QSpinBox *m_sustainSpin = nullptr;
    QSpinBox *m_releaseSpin = nullptr;
    QLabel *m_typeLabel = nullptr;
    QLabel *m_symbolLabel = nullptr;
    QLabel *m_sweepLabel = nullptr;
    QLabel *m_dutyLabel = nullptr;
    QLabel *m_periodLabel = nullptr;
    QLabel *m_synthWaveLabel = nullptr;
    QLabel *m_synthParamsLabel = nullptr;
    QLabel *m_adsrLabel = nullptr;
    QWidget *m_adsrRow = nullptr;
    QPushButton *m_newButton = nullptr;
};
