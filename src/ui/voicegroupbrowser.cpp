#include "voicegroupbrowser.h"

#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/m4asemantics.h"

namespace {
constexpr int kAuditionKey = 60; // middle C; drumkits play that key's percussion
constexpr int kAuditionVelocity = 112;

// The Type dropdown offers each voice family once: the _alt CGB variants are
// engine-identical duplicates, so they parse and display but are never
// offered as a choice. Keysplit isn't offered either — keysplit instruments
// share the Sample dropdown with plain samples, and the chosen symbol alone
// decides which macro the line gets (see commitEdit).
const VgMacro kSelectableMacros[] = {
    VgMacro::DirectSound,    VgMacro::DirectSoundNoResample,
    VgMacro::DirectSoundAlt, VgMacro::KeysplitAll,
    VgMacro::Square1,        VgMacro::Square2,
    VgMacro::ProgWave,       VgMacro::Noise,
};

// The macro the Type dropdown shows for a voice: keysplit voices read as
// plain "Sample" there.
VgMacro typeComboMacro(VgMacro m)
{
    return m == VgMacro::Keysplit ? VgMacro::DirectSound : m;
}

// The Type dropdown's userData for the Synth pseudo-type: synth voices are
// voice_directsound lines whose symbol is a synth descriptor, so like
// Keysplit there is no VgMacro for them (all VgMacro values are >= 0).
constexpr int kSynthTypeData = -1;

bool macroIsDsFamily(VgMacro m)
{
    return m == VgMacro::DirectSound || m == VgMacro::DirectSoundNoResample
        || m == VgMacro::DirectSoundAlt;
}

// A loaded DirectSound tone whose sample has size 0 is a Golden Sun synth
// descriptor, not PCM (m4a_pcm_channel_start; 0x18 = the fix/alt type bits).
bool toneIsSynth(const ToneData &td)
{
    return (td.type & ~0x18) == 0 && td.wav && td.wav->size == 0 && td.wav->data;
}

bool macroIsDrumkit(VgMacro m)
{
    return m == VgMacro::KeysplitAll;
}

bool macroIsWave(VgMacro m)
{
    return m == VgMacro::ProgWave || m == VgMacro::ProgWaveAlt;
}

bool macroUsesSampleList(VgMacro m)
{
    return m == VgMacro::Keysplit
        || (vgMacroHasSymbol(m) && !macroIsWave(m) && !macroIsDrumkit(m));
}

bool macroIsSquare1(VgMacro m)
{
    return m == VgMacro::Square1 || m == VgMacro::Square1Alt;
}
bool macroIsSquare2(VgMacro m)
{
    return m == VgMacro::Square2 || m == VgMacro::Square2Alt;
}
bool macroIsNoise(VgMacro m)
{
    return m == VgMacro::Noise || m == VgMacro::NoiseAlt;
}

// The ADSR text shown in the tree: the values the engine sees (CGB envelopes
// are masked on load), matching what setVoicegroup renders from ToneData.
QString adsrText(const VgVoice &v)
{
    if (v.macro == VgMacro::Keysplit || v.macro == VgMacro::KeysplitAll)
        return QString();
    if (vgMacroIsCgb(v.macro))
        return QStringLiteral("%1 %2 %3 %4")
            .arg(v.attack & 7)
            .arg(v.decay & 7)
            .arg(v.sustain & 15)
            .arg(v.release & 7);
    return QStringLiteral("%1 %2 %3 %4")
        .arg(uint8_t(v.attack))
        .arg(uint8_t(v.decay))
        .arg(uint8_t(v.sustain))
        .arg(uint8_t(v.release));
}

} // namespace

VoicegroupBrowser::VoicegroupBrowser(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_title = new QLabel(tr("No song loaded"), this);
    m_title->setContentsMargins(4, 2, 4, 0);
    layout->addWidget(m_title);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Voice"), tr("Type"), tr("ADSR")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->setToolTip(tr("Click and hold to audition (middle C)."));
    layout->addWidget(m_tree, 1);

    // Press-and-hold audition, matching the piano keys: press sounds the
    // voice, releasing the mouse anywhere releases the note.
    connect(m_tree, &QTreeWidget::itemPressed, this, &VoicegroupBrowser::pressedVoice);
    m_tree->viewport()->installEventFilter(this);
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this] { populateEditor(); });

    // ---- editor panel for the selected voice ----
    m_editor = new QWidget(this);
    auto *form = new QFormLayout(m_editor);
    form->setContentsMargins(4, 2, 4, 2);
    form->setVerticalSpacing(2);

    m_notice = new QLabel(m_editor);
    m_notice->setWordWrap(true);
    m_notice->setStyleSheet(QStringLiteral("color: palette(mid);"));
    form->addRow(m_notice);

    const auto addRow = [form](const QString &text, QWidget *field, QLabel **labelOut) {
        auto *label = new QLabel(text);
        form->addRow(label, field);
        *labelOut = label;
    };

    m_typeCombo = new QComboBox(m_editor);
    for (VgMacro macro : kSelectableMacros)
        m_typeCombo->addItem(vgMacroDisplayName(macro), int(macro));
    addRow(tr("Type"), m_typeCombo, &m_typeLabel);

    m_symbolCombo = new QComboBox(m_editor);
    m_symbolCombo->setEditable(true);
    m_symbolCombo->setInsertPolicy(QComboBox::NoInsert);
    addRow(tr("Sample"), m_symbolCombo, &m_symbolLabel);

    // The sweep byte is the GB NR10 register: three packed fields, edited
    // separately (value = 16*time + 8*direction + shift).
    m_sweepRow = new QWidget(m_editor);
    auto *sweepLayout = new QHBoxLayout(m_sweepRow);
    sweepLayout->setContentsMargins(0, 0, 0, 0);
    sweepLayout->setSpacing(2);
    m_sweepTimeSpin = new QSpinBox(m_sweepRow);
    m_sweepTimeSpin->setRange(0, 7);
    m_sweepTimeSpin->setSpecialValueText(tr("Off"));
    m_sweepTimeSpin->setToolTip(
        tr("Speed: 128 Hz clocks between pitch steps (1 = fastest, 7 = "
           "slowest, Off = no sweep)."));
    m_sweepDirCombo = new QComboBox(m_sweepRow);
    m_sweepDirCombo->addItems({tr("Rise"), tr("Fall")});
    m_sweepDirCombo->setToolTip(
        tr("Direction. A rising sweep silences the note when it overflows "
           "the highest pitch."));
    m_sweepShiftSpin = new QSpinBox(m_sweepRow);
    m_sweepShiftSpin->setRange(0, 7);
    m_sweepShiftSpin->setToolTip(
        tr("Step size: each step moves the frequency by 1/2^n of itself, so "
           "1 is wild, 7 is subtle, and 0 moves it the whole way at once."));
    sweepLayout->addWidget(m_sweepTimeSpin);
    sweepLayout->addWidget(m_sweepDirCombo);
    sweepLayout->addWidget(m_sweepShiftSpin);
    addRow(tr("Sweep"), m_sweepRow, &m_sweepLabel);

    m_dutyCombo = new QComboBox(m_editor);
    m_dutyCombo->addItems({QStringLiteral("12.5%"), QStringLiteral("25%"),
                           QStringLiteral("50%"), QStringLiteral("75%")});
    addRow(tr("Duty"), m_dutyCombo, &m_dutyLabel);

    m_periodCombo = new QComboBox(m_editor);
    m_periodCombo->addItems({tr("0 (15-bit, hiss)"), tr("1 (7-bit, metallic)")});
    addRow(tr("Period"), m_periodCombo, &m_periodLabel);

    // Golden Sun synth voices: waveform, plus the pulse duty-cycle LFO.
    m_synthWaveCombo = new QComboBox(m_editor);
    for (int waveform = 0; waveform < 3; waveform++)
        m_synthWaveCombo->addItem(vgSynthWaveformName(waveform));
    addRow(tr("Waveform"), m_synthWaveCombo, &m_synthWaveLabel);

    m_synthParamsRow = new QWidget(m_editor);
    auto *synthLayout = new QHBoxLayout(m_synthParamsRow);
    synthLayout->setContentsMargins(0, 0, 0, 0);
    synthLayout->setSpacing(2);
    for (QSpinBox **spin : {&m_synthDutySpin, &m_synthStepSpin, &m_synthDepthSpin,
                            &m_synthPhaseSpin}) {
        *spin = new QSpinBox(m_synthParamsRow);
        (*spin)->setRange(0, 255);
        // Commit on steps/editing-finished only: every distinct value here
        // mints a shared definition and reloads the voicegroup, so
        // per-keystroke commits would litter the synth data file.
        (*spin)->setKeyboardTracking(false);
        synthLayout->addWidget(*spin);
    }
    m_synthDutySpin->setToolTip(
        tr("Base duty cycle: the pulse width the wave centers on "
           "(128 = 50% square)."));
    m_synthStepSpin->setToolTip(
        tr("Duty LFO step per frame: how fast the pulse width wobbles "
           "(0 = static)."));
    m_synthDepthSpin->setToolTip(
        tr("Modulation amount: how far the pulse width swings around the "
           "base duty."));
    m_synthPhaseSpin->setToolTip(tr("Duty LFO phase offset."));
    addRow(tr("Duty LFO"), m_synthParamsRow, &m_synthParamsLabel);

    m_adsrRow = new QWidget(m_editor);
    auto *adsrLayout = new QHBoxLayout(m_adsrRow);
    adsrLayout->setContentsMargins(0, 0, 0, 0);
    adsrLayout->setSpacing(2);
    for (QSpinBox **spin : {&m_attackSpin, &m_decaySpin, &m_sustainSpin, &m_releaseSpin}) {
        *spin = new QSpinBox(m_adsrRow);
        (*spin)->setRange(0, 255); // narrowed per voice family on populate
        adsrLayout->addWidget(*spin);
    }
    m_attackSpin->setToolTip(tr("Attack"));
    m_decaySpin->setToolTip(tr("Decay"));
    m_sustainSpin->setToolTip(tr("Sustain"));
    m_releaseSpin->setToolTip(tr("Release"));
    addRow(tr("ADSR"), m_adsrRow, &m_adsrLabel);

    // Voice edits save with the song (Ctrl+S) and undo through the song's
    // undo stack, so the only button here is New…
    auto *buttons = new QWidget(m_editor);
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 2, 0, 0);
    m_newButton = new QPushButton(tr("New…"), buttons);
    m_newButton->setToolTip(tr("Create a new voicegroup file."));
    buttonLayout->addWidget(m_newButton);
    buttonLayout->addStretch(1);
    form->addRow(buttons);
    connect(m_newButton, &QPushButton::clicked, this,
            &VoicegroupBrowser::newVoicegroupRequested);
    layout->addWidget(m_editor);

    // Space stays the transport toggle even while an editor field has focus:
    // text inputs claim plain printable keys via ShortcutOverride, so the
    // filter refuses Space on their behalf (see eventFilter). The button is
    // a mouse target — keyboard focus on it would also swallow Space.
    m_newButton->setFocusPolicy(Qt::NoFocus);
    for (QWidget *w : std::initializer_list<QWidget *>{
             m_tree, m_typeCombo, m_symbolCombo, m_dutyCombo, m_periodCombo,
             m_synthWaveCombo, m_synthDutySpin, m_synthStepSpin, m_synthDepthSpin,
             m_synthPhaseSpin, m_sweepTimeSpin, m_sweepDirCombo, m_sweepShiftSpin,
             m_attackSpin, m_decaySpin, m_sustainSpin, m_releaseSpin}) {
        w->installEventFilter(this);
        for (QLineEdit *edit : w->findChildren<QLineEdit *>())
            edit->installEventFilter(this);
    }

    const auto commit = [this] { commitEdit(); };
    connect(m_typeCombo, &QComboBox::activated, this, commit);
    connect(m_symbolCombo, &QComboBox::activated, this, commit);
    connect(m_symbolCombo->lineEdit(), &QLineEdit::editingFinished, this, commit);
    connect(m_dutyCombo, &QComboBox::activated, this, commit);
    connect(m_periodCombo, &QComboBox::activated, this, commit);
    connect(m_sweepDirCombo, &QComboBox::activated, this, commit);
    connect(m_synthWaveCombo, &QComboBox::activated, this, commit);
    for (QSpinBox *spin : {m_sweepTimeSpin, m_sweepShiftSpin, m_synthDutySpin,
                           m_synthStepSpin, m_synthDepthSpin, m_synthPhaseSpin,
                           m_attackSpin, m_decaySpin, m_sustainSpin, m_releaseSpin})
        connect(spin, &QSpinBox::valueChanged, this, commit);

    populateEditor();
}

void VoicegroupBrowser::setVoicegroup(const LoadedVoiceGroup *vg, const QString &title)
{
    releaseVoice();
    m_vg = vg;
    if (!vg)
        m_source = nullptr; // a cleared voicegroup invalidates the source too
    m_tree->clear();
    if (!vg) {
        m_title->setText(tr("No song loaded"));
        populateEditor();
        return;
    }
    m_title->setText(title.isEmpty() ? tr("Voicegroup") : title);

    for (int i = 0; i < VOICEGROUP_SIZE; i++) {
        const ToneData &voice = vg->voices[i];
        QString name = QString::fromUtf8(vg->voiceNames[i]).trimmed();
        const QString type =
            toneIsSynth(voice) ? tr("Synth") : m4aVoiceTypeName(voice.type);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QStringLiteral("%1  %2").arg(i, 3, 10, QLatin1Char('0'))
                             .arg(name.isEmpty() ? type : name));
        item->setText(1, type);
        if (voice.type != VOICE_KEYSPLIT && voice.type != VOICE_KEYSPLIT_ALL)
            item->setText(2, QStringLiteral("%1 %2 %3 %4")
                                 .arg(voice.attack)
                                 .arg(voice.decay)
                                 .arg(voice.sustain)
                                 .arg(voice.release));
        item->setData(0, Qt::UserRole, i);
    }
    populateEditor();
}

void VoicegroupBrowser::setSource(VoicegroupSource *source,
                                  const QStringList &sampleSymbols,
                                  const QStringList &waveSymbols,
                                  const QList<QPair<QString, QString>> &keysplits,
                                  const QStringList &drumkits,
                                  const VgAdsrDefaults &adsrDefaults,
                                  const VgSynthCatalog &synths,
                                  std::function<QString(const VgSynthDesc &)> ensureSynth)
{
    if (source != m_source)
        m_adsrHistory.clear();
    m_source = source;
    m_adsrDefaults = adsrDefaults;
    m_waveSymbols = waveSymbols;
    m_drumkitChoices = drumkits;
    m_synths = synths;
    m_ensureSynth = std::move(ensureSynth);
    m_synthBySymbol.clear();
    for (const auto &def : m_synths.defs) {
        if (!m_synthBySymbol.contains(def.first)) // first definition wins
            m_synthBySymbol.insert(def.first, def.second);
    }
    m_keysplitTables.clear();
    m_sampleChoices.clear();
    for (const auto &pair : keysplits) {
        m_keysplitTables.insert(pair.first, pair.second);
        m_sampleChoices.append(pair.first);
    }
    m_sampleChoices += sampleSymbols; // already sorted, phonemes last
    populateEditor();
}

int VoicegroupBrowser::currentSlot() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    return item ? item->data(0, Qt::UserRole).toInt() : -1;
}

void VoicegroupBrowser::selectSlot(int slot)
{
    if (slot < 0 || slot >= m_tree->topLevelItemCount())
        return;
    m_tree->setCurrentItem(m_tree->topLevelItem(slot));
}

void VoicegroupBrowser::pressedVoice(QTreeWidgetItem *item)
{
    releaseVoice();
    if (!item)
        return;
    const int voice = item->data(0, Qt::UserRole).toInt();
    m_soundingVoice = voice;
    emit auditionVoice(voice, kAuditionKey, kAuditionVelocity);
}

void VoicegroupBrowser::releaseVoice()
{
    if (m_soundingVoice < 0)
        return;
    emit auditionVoice(m_soundingVoice, kAuditionKey, 0);
    m_soundingVoice = -1;
}

bool VoicegroupBrowser::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tree->viewport() && event->type() == QEvent::MouseButtonRelease)
        releaseVoice();
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
    return QWidget::eventFilter(watched, event);
}

void VoicegroupBrowser::setEditorRowsVisible(VgMacro macro, bool synth, bool visible)
{
    const auto showRow = [](QLabel *label, QWidget *field, bool on) {
        label->setVisible(on);
        field->setVisible(on);
    };
    showRow(m_typeLabel, m_typeCombo, visible);
    showRow(m_symbolLabel, m_symbolCombo, visible && vgMacroHasSymbol(macro));
    showRow(m_sweepLabel, m_sweepRow, visible && macroIsSquare1(macro));
    showRow(m_dutyLabel, m_dutyCombo,
            visible && (macroIsSquare1(macro) || macroIsSquare2(macro)));
    showRow(m_periodLabel, m_periodCombo, visible && macroIsNoise(macro));
    showRow(m_synthWaveLabel, m_synthWaveCombo, visible && synth);
    // The pulse LFO row narrows further by waveform (populateEditor).
    showRow(m_synthParamsLabel, m_synthParamsRow, visible && synth);
    showRow(m_adsrLabel, m_adsrRow,
            visible && macro != VgMacro::Keysplit && !macroIsDrumkit(macro));
}

bool VoicegroupBrowser::synthDescFor(const VgVoice &voice, int slot,
                                     VgSynthDesc *desc) const
{
    if (!macroIsDsFamily(voice.macro))
        return false;
    const auto it = m_synthBySymbol.constFind(voice.symbol);
    if (it != m_synthBySymbol.constEnd()) {
        *desc = it.value();
        return true;
    }
    if (m_vg && slot >= 0 && slot < VOICEGROUP_SIZE) {
        const ToneData &td = m_vg->voices[slot];
        if (toneIsSynth(td)) {
            const auto *d = reinterpret_cast<const uint8_t *>(td.wav->data);
            desc->waveform = d[1] > 2 ? 2 : d[1];
            desc->baseDuty = d[2];
            desc->dutyStep = d[3];
            desc->modDepth = d[4];
            desc->phase = d[5];
            return true;
        }
    }
    return false;
}

void VoicegroupBrowser::populateEditor()
{
    m_updating = true;
    const int slot = currentSlot();
    const VgVoice *voice =
        (m_source && slot >= 0) ? m_source->voiceAt(slot) : nullptr;

    if (!voice) {
        setEditorRowsVisible(VgMacro::DirectSound, false, false);
        QString notice;
        if (!m_vg)
            notice.clear();
        else if (!m_source)
            notice = tr("This voicegroup's source file couldn't be located; "
                        "voices are read-only.");
        else if (slot < 0)
            notice = tr("Select a voice to edit it.");
        else {
            switch (m_source->kindAt(slot)) {
            case VgLineKind::ReadOnlyVoice:
                notice = tr("Cry voices are read-only.");
                break;
            case VgLineKind::Broken:
                notice = tr("This voice line couldn't be parsed; it is kept as-is.");
                break;
            default:
                notice = tr("No voice is defined at this slot.");
                break;
            }
        }
        m_notice->setText(notice);
        m_notice->setVisible(!notice.isEmpty());
        m_updating = false;
        return;
    }

    VgSynthDesc synthDesc;
    const bool synth = synthDescFor(*voice, slot, &synthDesc);

    m_notice->setVisible(false);
    setEditorRowsVisible(voice->macro, synth, true);
    if (synth) {
        m_synthParamsLabel->setVisible(synthDesc.waveform == 0);
        m_synthParamsRow->setVisible(synthDesc.waveform == 0);
    }

    // The selectable types (with Synth after the Sample variants when the
    // project has synth support), plus the current one when it's an _alt CGB
    // variant we display but never offer.
    const int shownData = synth ? kSynthTypeData : int(typeComboMacro(voice->macro));
    m_typeCombo->clear();
    for (VgMacro macro : kSelectableMacros) {
        m_typeCombo->addItem(vgMacroDisplayName(macro), int(macro));
        if (macro == VgMacro::DirectSoundAlt && (m_synths.available() || synth))
            m_typeCombo->addItem(tr("Synth"), kSynthTypeData);
    }
    if (m_typeCombo->findData(shownData) < 0)
        m_typeCombo->addItem(vgMacroDisplayName(VgMacro(shownData)), shownData);
    m_typeCombo->setCurrentIndex(m_typeCombo->findData(shownData));

    // Real GBA ranges; out-of-range file values clamp here (and the engine
    // masks them to the same effect on load anyway).
    const bool cgb = vgMacroIsCgb(voice->macro);
    m_attackSpin->setRange(0, cgb ? 7 : 255);
    m_decaySpin->setRange(0, cgb ? 7 : 255);
    m_sustainSpin->setRange(0, cgb ? 15 : 255);
    m_releaseSpin->setRange(0, cgb ? 7 : 255);

    if (vgMacroHasSymbol(voice->macro)) {
        const bool wave = macroIsWave(voice->macro);
        const bool drumkit = macroIsDrumkit(voice->macro);
        m_symbolLabel->setText(synth ? tr("Synth")
                                     : wave ? tr("Wave")
                                            : drumkit ? tr("Drumkit") : tr("Sample"));
        m_symbolCombo->clear();
        if (synth) {
            // The current symbol may be a zero-size .bin descriptor with no
            // set_synth entry; setCurrentText still shows it (editable combo).
            for (const auto &def : m_synths.defs)
                m_symbolCombo->addItem(def.first);
        } else {
            m_symbolCombo->addItems(wave ? m_waveSymbols
                                         : drumkit ? m_drumkitChoices
                                                   : m_sampleChoices);
        }
        m_symbolCombo->setCurrentText(voice->symbol);
    }
    m_synthWaveCombo->setCurrentIndex(std::clamp(synthDesc.waveform, 0, 2));
    m_synthDutySpin->setValue(synthDesc.baseDuty);
    m_synthStepSpin->setValue(synthDesc.dutyStep);
    m_synthDepthSpin->setValue(synthDesc.modDepth);
    m_synthPhaseSpin->setValue(synthDesc.phase);
    m_sweepTimeSpin->setValue((voice->sweep >> 4) & 7);
    m_sweepDirCombo->setCurrentIndex((voice->sweep >> 3) & 1);
    m_sweepShiftSpin->setValue(voice->sweep & 7);
    m_dutyCombo->setCurrentIndex(voice->duty & 3);
    m_periodCombo->setCurrentIndex(voice->period & 1);
    m_attackSpin->setValue(voice->attack);
    m_decaySpin->setValue(voice->decay);
    m_sustainSpin->setValue(voice->sustain);
    m_releaseSpin->setValue(voice->release);
    m_adsrRow->setToolTip(cgb ? tr("Attack/decay/release 0-7, sustain 0-15.")
                              : tr("Attack, decay, sustain, release: 0-255 each."));
    m_updating = false;
}

void VoicegroupBrowser::commitEdit()
{
    if (m_updating || !m_source)
        return;
    const int slot = currentSlot();
    const VgVoice *cur = m_source->voiceAt(slot);
    if (!cur)
        return;

    VgSynthDesc curDesc;
    const bool curSynth = synthDescFor(*cur, slot, &curDesc);

    VgVoice v = *cur; // key & pan carry over: no UI for them
    // The combo never holds Keysplit (a keysplit voice reads as "Sample"
    // there), so a keysplit voice's unchanged type comes back as DirectSound;
    // the symbol check below restores the keysplit macro. Synth voices read
    // as the kSynthTypeData pseudo-type instead.
    const int selData = m_typeCombo->currentData().toInt();
    const int shownData =
        curSynth ? kSynthTypeData : int(typeComboMacro(cur->macro));
    const bool typeChanged = selData != shownData;

    if (selData == kSynthTypeData) {
        // A synth voice stays a voice_directsound line; the symbol alone
        // carries the waveform and pulse parameters, deduplicated across the
        // project's definitions (param-named entries are minted on demand).
        v.macro = macroIsDsFamily(cur->macro) ? cur->macro : VgMacro::DirectSound;
        v.keysplitTable.clear();
        VgSynthDesc desc;
        const QString comboSymbol = m_symbolCombo->currentText().trimmed();
        if (!curSynth) {
            // Just switched to Synth: the waveform/param fields are stale, so
            // adopt the project's first definition (or a plain 50% pulse).
            if (!m_synths.defs.isEmpty())
                desc = m_synths.defs.first().second;
        } else if (comboSymbol != cur->symbol
                   && m_synthBySymbol.contains(comboSymbol)) {
            desc = m_synthBySymbol.value(comboSymbol); // picked a definition
        } else {
            desc.waveform = m_synthWaveCombo->currentIndex();
            desc.baseDuty = m_synthDutySpin->value();
            desc.dutyStep = m_synthStepSpin->value();
            desc.modDepth = m_synthDepthSpin->value();
            desc.phase = m_synthPhaseSpin->value();
        }
        QString symbol;
        if (curSynth && desc == curDesc) {
            symbol = cur->symbol; // unchanged (or a duplicate definition)
        } else {
            symbol = m_synths.symbolFor(desc);
            if (symbol.isEmpty() && m_ensureSynth)
                symbol = m_ensureSynth(desc);
        }
        if (symbol.isEmpty()) {
            populateEditor(); // no definition to point at — refuse the change
            return;
        }
        if (!m_synthBySymbol.contains(symbol)) {
            m_synths.defs.append({symbol, desc});
            m_synthBySymbol.insert(symbol, desc);
        }
        v.symbol = symbol;
    } else if (vgMacroHasSymbol(VgMacro(selData))) {
        // Resolve the instrument symbol first: for sample-list types the
        // chosen symbol decides between a keysplit and a plain sample voice.
        v.macro = VgMacro(selData);
        const bool wave = macroIsWave(v.macro);
        const bool drumkit = macroIsDrumkit(v.macro);
        // The combo only holds this family's list when the current voice
        // shares it (samples and keysplits share one list; waves, drumkits,
        // and synths each have their own).
        const bool comboShowsThisList = wave
            ? macroIsWave(cur->macro)
            : drumkit ? macroIsDrumkit(cur->macro)
                      : macroUsesSampleList(cur->macro) && !curSynth;
        QString symbol =
            comboShowsThisList ? m_symbolCombo->currentText().trimmed() : QString();
        // A deliberate Type change away from a keysplit or synth voice
        // overrides the stale symbol shown in the combo.
        if (!wave && !drumkit && typeChanged
            && (m_keysplitTables.contains(symbol) || m_synthBySymbol.contains(symbol)))
            symbol.clear();
        if (symbol.isEmpty()) {
            if (wave)
                symbol = m_waveSymbols.value(0);
            else if (drumkit)
                symbol = m_drumkitChoices.value(0);
            else
                symbol = m_sampleChoices.value(m_keysplitTables.size()); // first sample
        }
        if (symbol.isEmpty()) {
            populateEditor(); // no symbols to offer — refuse the change
            return;
        }
        v.symbol = symbol;
        if (!wave && !drumkit && m_keysplitTables.contains(symbol)) {
            v.macro = VgMacro::Keysplit;
            v.keysplitTable = m_keysplitTables.value(symbol);
        } else {
            v.keysplitTable.clear();
        }
    } else {
        v.macro = VgMacro(selData);
        v.symbol.clear();
        v.keysplitTable.clear();
    }

    // Crossing between envelope families whose numbers mean different things
    // (a CGB attack of 0 is instant; a DirectSound attack of 0 is silence) or
    // leaving an envelope-less keysplit/drumkit: carrying the digits across
    // gives a nonsense envelope, so remember the outgoing family's values for
    // this slot and adopt the incoming family's remembered or project-typical
    // ones instead.
    const int oldFamily = vgAdsrFamily(cur->macro);
    const int newFamily = vgAdsrFamily(v.macro);
    const bool crossedFamily = newFamily != oldFamily
        && (oldFamily < 0 || newFamily < 0
            || vgMacroIsCgb(v.macro) != vgMacroIsCgb(cur->macro));
    if (crossedFamily && oldFamily >= 0)
        m_adsrHistory[slot][oldFamily] =
            VgAdsr{cur->attack, cur->decay, cur->sustain, cur->release};

    if (v.macro != VgMacro::Keysplit && !macroIsDrumkit(v.macro)) {
        if (crossedFamily) {
            const QHash<int, VgAdsr> remembered = m_adsrHistory.value(slot);
            const VgAdsr adopted = remembered.contains(newFamily)
                ? remembered.value(newFamily)
                : vgDefaultAdsr(m_adsrDefaults, v.macro, v.symbol);
            v.attack = adopted.attack;
            v.decay = adopted.decay;
            v.sustain = adopted.sustain;
            v.release = adopted.release;
        } else {
            v.attack = m_attackSpin->value();
            v.decay = m_decaySpin->value();
            v.sustain = m_sustainSpin->value();
            v.release = m_releaseSpin->value();
            // An untouched envelope follows the instrument: when the values
            // are exactly what adoption chose for the old symbol (the user
            // never tuned them), a symbol change re-adopts for the new one.
            if (vgMacroHasSymbol(v.macro) && v.symbol != cur->symbol
                && VgAdsr{v.attack, v.decay, v.sustain, v.release}
                       == vgDefaultAdsr(m_adsrDefaults, cur->macro, cur->symbol)) {
                const VgAdsr fresh = vgDefaultAdsr(m_adsrDefaults, v.macro, v.symbol);
                v.attack = fresh.attack;
                v.decay = fresh.decay;
                v.sustain = fresh.sustain;
                v.release = fresh.release;
            }
        }
        // Recompose NR10 from the three fields; bit 7 is unused by the sweep
        // unit, so keep the file's value for it.
        v.sweep = (cur->sweep & 0x80) | (m_sweepTimeSpin->value() << 4)
            | (m_sweepDirCombo->currentIndex() << 3) | m_sweepShiftSpin->value();
        v.duty = m_dutyCombo->currentIndex();
        v.period = m_periodCombo->currentIndex();
        if (vgMacroIsCgb(v.macro)) {
            // A remembered envelope can hold a file value outside the CGB
            // ranges (the file is parsed raw; only the spin ranges narrow).
            v.attack = std::min(v.attack, 7);
            v.decay = std::min(v.decay, 7);
            v.sustain = std::min(v.sustain, 15);
            v.release = std::min(v.release, 7);
        }
    } else if (v.macro != cur->macro) {
        // Entering keysplit/drumkit: the line carries no scalar args, so
        // match what a fresh parse of it would produce.
        v = VgVoice{v.macro, 60, 0, v.symbol, v.keysplitTable};
    }

    if (v == *cur)
        return;
    // The owner applies the edit (through the song's undo stack) and echoes
    // it back: voiceChanged for a scalar poke, a full voicegroup swap for a
    // structural one — either path refreshes the row and this editor.
    emit voiceEditRequested(slot, v, vgVoiceStructuralChange(*cur, v));
}

void VoicegroupBrowser::voiceChanged(int slot)
{
    updateRow(slot);
    if (slot == currentSlot())
        populateEditor();
}

void VoicegroupBrowser::updateRow(int slot)
{
    QTreeWidgetItem *item = m_tree->topLevelItem(slot);
    const VgVoice *voice = m_source ? m_source->voiceAt(slot) : nullptr;
    if (!item || !voice)
        return;
    VgSynthDesc desc;
    const QString type = synthDescFor(*voice, slot, &desc)
                             ? tr("Synth")
                             : m4aVoiceTypeName(vgMacroVoiceType(voice->macro));
    const QString name = vgMacroHasSymbol(voice->macro) ? voice->symbol : QString();
    item->setText(0, QStringLiteral("%1  %2").arg(slot, 3, 10, QLatin1Char('0'))
                         .arg(name.isEmpty() ? type : name));
    item->setText(1, type);
    item->setText(2, adsrText(*voice));
}
