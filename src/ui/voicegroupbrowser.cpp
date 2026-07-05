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
// offered as a choice.
const VgMacro kSelectableMacros[] = {
    VgMacro::DirectSound,   VgMacro::DirectSoundNoResample,
    VgMacro::DirectSoundAlt, VgMacro::Keysplit,
    VgMacro::KeysplitAll,    VgMacro::Square1,
    VgMacro::Square2,        VgMacro::ProgWave,
    VgMacro::Noise,
};

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

    m_sweepSpin = new QSpinBox(m_editor);
    m_sweepSpin->setRange(0, 255);
    m_sweepSpin->setToolTip(tr("GB sweep register (0 = off)."));
    addRow(tr("Sweep"), m_sweepSpin, &m_sweepLabel);

    m_dutyCombo = new QComboBox(m_editor);
    m_dutyCombo->addItems({QStringLiteral("12.5%"), QStringLiteral("25%"),
                           QStringLiteral("50%"), QStringLiteral("75%")});
    addRow(tr("Duty"), m_dutyCombo, &m_dutyLabel);

    m_periodCombo = new QComboBox(m_editor);
    m_periodCombo->addItems({tr("0 (15-bit, hiss)"), tr("1 (7-bit, metallic)")});
    addRow(tr("Period"), m_periodCombo, &m_periodLabel);

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

    auto *buttons = new QWidget(m_editor);
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 2, 0, 0);
    m_saveButton = new QPushButton(tr("Save"), buttons);
    m_revertButton = new QPushButton(tr("Revert"), buttons);
    m_newButton = new QPushButton(tr("New…"), buttons);
    m_newButton->setToolTip(tr("Create a new voicegroup file."));
    buttonLayout->addWidget(m_saveButton);
    buttonLayout->addWidget(m_revertButton);
    buttonLayout->addWidget(m_newButton);
    buttonLayout->addStretch(1);
    form->addRow(buttons);
    connect(m_saveButton, &QPushButton::clicked, this,
            &VoicegroupBrowser::saveRequested);
    connect(m_revertButton, &QPushButton::clicked, this,
            &VoicegroupBrowser::revertRequested);
    connect(m_newButton, &QPushButton::clicked, this,
            &VoicegroupBrowser::newVoicegroupRequested);
    layout->addWidget(m_editor);

    // Space stays the transport toggle even while an editor field has focus:
    // text inputs claim plain printable keys via ShortcutOverride, so the
    // filter refuses Space on their behalf (see eventFilter). The buttons are
    // mouse targets — keyboard focus on them would also swallow Space.
    for (QPushButton *button : {m_saveButton, m_revertButton, m_newButton})
        button->setFocusPolicy(Qt::NoFocus);
    for (QWidget *w : std::initializer_list<QWidget *>{
             m_tree, m_typeCombo, m_symbolCombo, m_dutyCombo, m_periodCombo,
             m_sweepSpin, m_attackSpin, m_decaySpin, m_sustainSpin, m_releaseSpin}) {
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
    for (QSpinBox *spin : {m_sweepSpin, m_attackSpin, m_decaySpin, m_sustainSpin,
                           m_releaseSpin})
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
        refreshButtons();
        return;
    }
    m_title->setText(title.isEmpty() ? tr("Voicegroup") : title);

    for (int i = 0; i < VOICEGROUP_SIZE; i++) {
        const ToneData &voice = vg->voices[i];
        QString name = QString::fromUtf8(vg->voiceNames[i]).trimmed();
        const QString type = m4aVoiceTypeName(voice.type);
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
    refreshButtons();
}

void VoicegroupBrowser::setSource(VoicegroupSource *source,
                                  const QStringList &sampleSymbols,
                                  const QStringList &waveSymbols,
                                  const QList<QPair<QString, QString>> &keysplits,
                                  const QStringList &drumkits)
{
    m_source = source;
    m_waveSymbols = waveSymbols;
    m_drumkitChoices = drumkits;
    m_keysplitTables.clear();
    m_sampleChoices.clear();
    for (const auto &pair : keysplits) {
        m_keysplitTables.insert(pair.first, pair.second);
        m_sampleChoices.append(pair.first);
    }
    m_sampleChoices += sampleSymbols; // already sorted, phonemes last
    populateEditor();
    refreshButtons();
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

void VoicegroupBrowser::setEditorRowsVisible(VgMacro macro, bool visible)
{
    const auto showRow = [](QLabel *label, QWidget *field, bool on) {
        label->setVisible(on);
        field->setVisible(on);
    };
    showRow(m_typeLabel, m_typeCombo, visible);
    showRow(m_symbolLabel, m_symbolCombo, visible && vgMacroHasSymbol(macro));
    showRow(m_sweepLabel, m_sweepSpin, visible && macroIsSquare1(macro));
    showRow(m_dutyLabel, m_dutyCombo,
            visible && (macroIsSquare1(macro) || macroIsSquare2(macro)));
    showRow(m_periodLabel, m_periodCombo, visible && macroIsNoise(macro));
    showRow(m_adsrLabel, m_adsrRow,
            visible && macro != VgMacro::Keysplit && !macroIsDrumkit(macro));
}

void VoicegroupBrowser::populateEditor()
{
    m_updating = true;
    const int slot = currentSlot();
    const VgVoice *voice =
        (m_source && slot >= 0) ? m_source->voiceAt(slot) : nullptr;

    if (!voice) {
        setEditorRowsVisible(VgMacro::DirectSound, false);
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

    m_notice->setVisible(false);
    setEditorRowsVisible(voice->macro, true);

    // The selectable types, plus the current one when it's an _alt CGB
    // variant we display but never offer.
    m_typeCombo->clear();
    for (VgMacro macro : kSelectableMacros)
        m_typeCombo->addItem(vgMacroDisplayName(macro), int(macro));
    if (m_typeCombo->findData(int(voice->macro)) < 0)
        m_typeCombo->addItem(vgMacroDisplayName(voice->macro), int(voice->macro));
    m_typeCombo->setCurrentIndex(m_typeCombo->findData(int(voice->macro)));

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
        m_symbolLabel->setText(wave ? tr("Wave")
                                    : drumkit ? tr("Drumkit") : tr("Sample"));
        m_symbolCombo->clear();
        m_symbolCombo->addItems(wave ? m_waveSymbols
                                     : drumkit ? m_drumkitChoices : m_sampleChoices);
        m_symbolCombo->setCurrentText(voice->symbol);
    }
    m_sweepSpin->setValue(voice->sweep);
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

    VgVoice v = *cur; // key & pan carry over: no UI for them
    v.macro = VgMacro(m_typeCombo->currentData().toInt());
    const bool typeChanged = v.macro != cur->macro;

    // Resolve the instrument symbol first: for sample-list types the chosen
    // symbol decides between a keysplit and a plain sample voice.
    if (vgMacroHasSymbol(v.macro)) {
        const bool wave = macroIsWave(v.macro);
        const bool drumkit = macroIsDrumkit(v.macro);
        // The combo only holds this family's list when the current voice
        // shares it (samples and keysplits share one list; waves and
        // drumkits each have their own).
        const bool comboShowsThisList = wave
            ? macroIsWave(cur->macro)
            : drumkit ? macroIsDrumkit(cur->macro) : macroUsesSampleList(cur->macro);
        QString symbol =
            comboShowsThisList ? m_symbolCombo->currentText().trimmed() : QString();
        // A deliberate Type change between Keysplit and the sample types
        // overrides the stale symbol shown in the combo.
        if (!wave && !drumkit && typeChanged) {
            const bool symbolIsKeysplit = m_keysplitTables.contains(symbol);
            if (symbolIsKeysplit != (v.macro == VgMacro::Keysplit))
                symbol.clear();
        }
        if (symbol.isEmpty()) {
            if (wave)
                symbol = m_waveSymbols.value(0);
            else if (drumkit)
                symbol = m_drumkitChoices.value(0);
            else if (v.macro == VgMacro::Keysplit)
                symbol = m_sampleChoices.value(0); // keysplits sort first
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
            if (v.macro == VgMacro::Keysplit)
                v.macro = VgMacro::DirectSound; // keysplit type, plain sample picked
            v.keysplitTable.clear();
        }
    } else {
        v.symbol.clear();
        v.keysplitTable.clear();
    }

    if (v.macro != VgMacro::Keysplit && !macroIsDrumkit(v.macro)) {
        if (cur->macro == VgMacro::Keysplit || macroIsDrumkit(cur->macro)) {
            // Leaving keysplit/drumkit: their scalar fields are all zero —
            // start from the plain full-sustain envelope instead of a silent one.
            v.attack = 255;
            v.decay = 0;
            v.sustain = 255;
            v.release = 0;
        } else {
            v.attack = m_attackSpin->value();
            v.decay = m_decaySpin->value();
            v.sustain = m_sustainSpin->value();
            v.release = m_releaseSpin->value();
        }
        v.sweep = m_sweepSpin->value();
        v.duty = m_dutyCombo->currentIndex();
        v.period = m_periodCombo->currentIndex();
        if (vgMacroIsCgb(v.macro)) {
            // A type change can carry DirectSound-range ADSR; the spin
            // ranges only narrow when the editor repopulates.
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
    const bool structural = vgVoiceStructuralChange(*cur, v);
    m_source->setVoice(slot, v);
    updateRow(slot);
    refreshButtons();
    emit voiceEdited(slot, structural);
    if (structural)
        populateEditor(); // field set / symbol list may have changed
}

void VoicegroupBrowser::updateRow(int slot)
{
    QTreeWidgetItem *item = m_tree->topLevelItem(slot);
    const VgVoice *voice = m_source ? m_source->voiceAt(slot) : nullptr;
    if (!item || !voice)
        return;
    const QString type = m4aVoiceTypeName(vgMacroVoiceType(voice->macro));
    const QString name = vgMacroHasSymbol(voice->macro) ? voice->symbol : QString();
    item->setText(0, QStringLiteral("%1  %2").arg(slot, 3, 10, QLatin1Char('0'))
                         .arg(name.isEmpty() ? type : name));
    item->setText(1, type);
    item->setText(2, adsrText(*voice));
}

void VoicegroupBrowser::refreshButtons()
{
    const bool dirty = m_source && m_source->dirty();
    m_saveButton->setEnabled(dirty);
    m_revertButton->setEnabled(dirty);
}
