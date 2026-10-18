#include "voicegroupbrowser.h"

#include <QActionGroup>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/keymap.h"
#include "ui/layout.h"

#include "ui/m4asemantics.h"

#include "ui/theme/themeruntime.h"

#include "project/songregistry.h"

namespace {
constexpr int kAuditionKey = 60; // middle C; drumkits play that key's percussion
constexpr int kAuditionVelocity = 112;

// Rides beside Copy's text/plain macro line, carrying what the line can't: a
// synth voice's descriptor, so a pending (unsaved) synth symbol — known only
// to the project that minted it — pastes anywhere. JSON: {"line": ...,
// "synth": {waveform, baseDuty, dutyStep, modDepth, phase}}.
const QString kVoiceMimeType = QStringLiteral("application/x-porydaw-voice");

// What Clear writes: the vanilla unused-slot line.
const VgVoice kPlaceholderVoice{
    VgMacro::Square1, 60, 0, QString(), QString(), 0, 2, 0, 0, 0, 15, 0};

// Two voices that write the same .inc line are the same voice: VgVoice's
// operator== also compares fields the macro doesn't use (a Square 1's
// period, a sample's duty), which an edit can leave behind.
bool sameVoiceLine(const VgVoice &a, const VgVoice &b)
{
    return VoicegroupSource::renderVoiceLine(a) == VoicegroupSource::renderVoiceLine(b);
}

// The Type dropdown offers every voice family the engine treats as its own,
// each right after the plain variant it detunes. The one omission among the
// _alt macros is voice_noise_alt: CgbSound guards the FIX bit with `ch < 4`,
// so it is inert on the noise channel and the macro is an exact duplicate of
// voice_noise (it still parses and displays — populateEditor appends the
// current macro when it isn't offered). Keysplit isn't offered either —
// keysplit instruments share the Sample dropdown with plain samples, and the
// chosen symbol alone decides which macro the line gets (see commitEdit).
//
// The compressed Sample variants are pokeemerald-expansion macros, offered
// only in projects that define them. The DirectSoundAlt entry stands for
// "Sample (reversed)" in whichever spelling the project defines (_alt or its
// twin _reverse; see populateEditor), so it is listed once either way.
const VgMacro kSelectableMacros[] = {
    VgMacro::DirectSound,
    VgMacro::DirectSoundNoResample,
    VgMacro::DirectSoundAlt,
    VgMacro::DirectSoundCompressed,
    VgMacro::DirectSoundCompressedReverse,
    VgMacro::KeysplitAll,
    VgMacro::Square1,
    VgMacro::Square1Alt,
    VgMacro::Square2,
    VgMacro::Square2Alt,
    VgMacro::ProgWave,
    VgMacro::ProgWaveAlt,
    VgMacro::Noise,
};

// Shown on the sample variants and the _alt PSG entries.
QString typeItemTip(VgMacro macro)
{
    switch (macro) {
    case VgMacro::DirectSoundNoResample:
        return QObject::tr("Same as Sample, but the note's pitch is ignored. Typically used for "
                           "percussion instruments.");
    case VgMacro::DirectSoundAlt:
    case VgMacro::DirectSoundReverse:
        return QObject::tr("Same as Sample, but played backwards.");
    case VgMacro::DirectSoundCompressed:
        return QObject::tr("A DPCM-compressed sample, such as a Pokémon cry.");
    case VgMacro::DirectSoundCompressedReverse:
        return QObject::tr("A DPCM-compressed sample, such as a Pokémon cry, played backwards.");
    case VgMacro::Square1Alt:
    case VgMacro::Square2Alt:
    case VgMacro::ProgWaveAlt:
        return QObject::tr("Same as %1, with the pitch slightly detuned.")
            .arg(vgMacroDisplayName(VgMacro(vgAdsrFamily(macro))));
    default:
        return QString();
    }
}

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
    return vgMacroIsDirectSound(m);
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
    return m == VgMacro::Keysplit || (vgMacroHasSymbol(m) && !macroIsWave(m) && !macroIsDrumkit(m));
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

VoicegroupBrowser::VoicegroupBrowser(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(::layout::space(::layout::Space::Half));

    // The song's voicegroup (mid2agb -G), shown as the symbol it forms: a
    // fixed "voicegroup_" prefix and the editable name (the -G arg with its
    // leading underscore assumed into the prefix). Picking or typing another
    // name re-targets the song (an undoable cfg edit, via the owner).
    auto *vgRow = new QWidget(this);
    auto *vgLayout = new QHBoxLayout(vgRow);
    vgLayout->setContentsMargins(4, 2, 4, 0);
    vgLayout->setSpacing(2);
    vgLayout->addWidget(new QLabel(tr("voicegroup_"), vgRow));
    m_vgCombo = new QComboBox(vgRow);
    m_vgCombo->setObjectName(QStringLiteral("vgArgCombo"));
    m_vgCombo->setEditable(true);
    m_vgCombo->setInsertPolicy(QComboBox::NoInsert);
    m_vgCombo->setEnabled(false);
    m_vgCombo->lineEdit()->setPlaceholderText(tr("No song loaded"));
    m_vgCombo->setToolTip(tr("The song's voicegroup: the prefix plus this name form the "
                             "symbol, e.g. \"abandoned_ship\" → voicegroup_abandoned_ship "
                             "(mid2agb -G). Changing it is undoable."));
    vgLayout->addWidget(m_vgCombo, 1);
    layout->addWidget(vgRow);

    const auto commitVgArg = [this] {
        if (m_updating || !m_vgCombo->isEnabled())
            return;
        const QString arg =
            SongRegistry::voicegroupArgFromDisplay(m_vgCombo->currentText().trimmed(), m_vgChoices);
        if (arg == m_vgArg)
            return;
        m_vgArg = arg;
        emit voicegroupChangeRequested(arg);
    };
    connect(m_vgCombo, &QComboBox::activated, this, commitVgArg);
    connect(m_vgCombo->lineEdit(), &QLineEdit::editingFinished, this, commitVgArg);

    const int columnPadding = ::layout::space(::layout::Space::Two);

    m_tree = new QTreeWidget(this);
    ::layout::configureListPositionIndicator(*m_tree->verticalScrollBar());
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Voice"), tr("Type"), tr("ADSR")});
    m_tree->setStyleSheet(QStringLiteral("QTreeWidget::item{padding-left:%1px;}"
                                         "QHeaderView::section{padding-left:%1px;}")
                              .arg(columnPadding));
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->setToolTip(tr("Click and hold to audition (middle C). Right-click for Copy, "
                          "Paste, and Clear."));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            &VoicegroupBrowser::showVoiceMenu);
    layout->addWidget(m_tree, 1);

    // Press-and-hold audition, matching the piano keys: press sounds the
    // voice, releasing the mouse anywhere releases the note.
    connect(m_tree, &QTreeWidget::itemPressed, this, &VoicegroupBrowser::pressedVoice);
    m_tree->viewport()->installEventFilter(this);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { populateEditor(); });

    // ---- editor panel for the selected voice ----
    m_editor = new QWidget(this);
    // The editor adapts to whatever dock width the user set; it must never
    // force the dock wider (a DS voice's three-digit ADSR spins are wider
    // than a CGB voice's, and used to push the dock open on selection).
    // Fields shrink or clip instead.
    m_editor->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *form = new QFormLayout(m_editor);
    form->setContentsMargins(
        ::layout::space(::layout::Space::One), ::layout::space(::layout::Space::Half),
        ::layout::space(::layout::Space::One), ::layout::space(::layout::Space::Half));
    form->setVerticalSpacing(::layout::space(::layout::Space::Half));

    m_notice = new QLabel(m_editor);
    m_notice->setWordWrap(true);
    m_notice->setObjectName(QStringLiteral("voicegroupEditorNotice"));
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

    m_symbolRow = new QWidget(m_editor);
    auto *symbolLayout = new QHBoxLayout(m_symbolRow);
    symbolLayout->setContentsMargins(0, 0, 0, 0);
    symbolLayout->setSpacing(2);
    m_symbolCombo = new QComboBox(m_symbolRow);
    m_symbolCombo->setEditable(true);
    m_symbolCombo->setInsertPolicy(QComboBox::NoInsert);
    symbolLayout->addWidget(m_symbolCombo, 1);
    // Sample-list voices swap the combo for the searchable picker: the
    // project's sample list is huge and prefix-heavy, so it gets search,
    // loop badges, and audition-on-highlight instead of a mile-long menu.
    m_samplePicker = new SamplePickerButton(m_symbolRow);
    symbolLayout->addWidget(m_samplePicker, 1);
    connect(m_samplePicker, &SamplePickerButton::symbolPicked, this, [this] { commitEdit(); });
    connect(m_samplePicker, &SamplePickerButton::auditionRequested, this,
            [this](const QString &symbol) {
                const VgVoice *voice = m_source ? m_source->voiceAt(currentSlot()) : nullptr;
                // The destination voice's envelope, so the browse audition
                // sounds like the commit would. A keysplit row's envelope
                // comes from its resolved sub-voice instead (a keysplit
                // destination has no envelope of its own — all-zero:
                // silence), so adsr is left default there.
                AuditionSlots::Adsr adsr;
                VgAuditionKind kind = VgAuditionKind::Sample;
                if (voice && macroIsWave(voice->macro)) {
                    kind = VgAuditionKind::Wave;
                    adsr = {uint8_t(voice->attack & 0x07), uint8_t(voice->decay & 0x07),
                            uint8_t(voice->sustain & 0x0F), uint8_t(voice->release & 0x07)};
                } else if (m_keysplitTables.contains(symbol)) {
                    kind = VgAuditionKind::Keysplit;
                } else if (voice && macroIsDsFamily(voice->macro)) {
                    adsr = {uint8_t(voice->attack), uint8_t(voice->decay), uint8_t(voice->sustain),
                            uint8_t(voice->release)};
                }
                emit sampleAuditionRequested(symbol, kind, adsr);
            });
    connect(m_samplePicker, &SamplePickerButton::auditionStopRequested, this,
            &VoicegroupBrowser::sampleAuditionStopRequested);
    // Sample Editor entry (SPEC §6.2): visible on DirectSound sample voices.
    // Glyph-only tool buttons — the picker needs the row's width, and the
    // tooltips carry the words the labels used to.
    m_newSampleButton = new QToolButton(m_symbolRow);
    m_newSampleButton->setObjectName(QStringLiteral("vgNewSampleButton"));
    m_newSampleButton->setText(QStringLiteral("+"));
    m_newSampleButton->setAutoRaise(true);
    m_newSampleButton->setToolTip(tr("New sample: import a sample and assign it to this voice."));
    symbolLayout->addWidget(m_newSampleButton);
    connect(m_newSampleButton, &QToolButton::clicked, this,
            [this] { emit newSampleRequested(currentSlot()); });
    m_editSampleButton = new QToolButton(m_symbolRow);
    m_editSampleButton->setObjectName(QStringLiteral("vgEditSampleButton"));
    m_editSampleButton->setText(QStringLiteral("✎"));
    m_editSampleButton->setAutoRaise(true);
    m_editSampleButton->setToolTip(tr("Edit sample: reopen this voice's sample in Sample Editor."));
    symbolLayout->addWidget(m_editSampleButton);
    connect(m_editSampleButton, &QToolButton::clicked, this,
            [this] { emit editSampleRequested(currentSlot()); });
    addRow(tr("Sample"), m_symbolRow, &m_symbolLabel);

    // The sweep byte is the GB NR10 register: three packed fields, edited
    // separately (value = 16*time + 8*direction + shift).
    m_sweepRow = new QWidget(m_editor);
    auto *sweepLayout = new QHBoxLayout(m_sweepRow);
    sweepLayout->setContentsMargins(0, 0, 0, 0);
    sweepLayout->setSpacing(::layout::space(::layout::Space::Half));
    m_sweepTimeSpin = new QSpinBox(m_sweepRow);
    m_sweepTimeSpin->setRange(0, 7);
    m_sweepTimeSpin->setSpecialValueText(tr("Off"));
    m_sweepTimeSpin->setToolTip(tr("Speed: 128 Hz clocks between pitch steps (1 = fastest, 7 = "
                                   "slowest, Off = no sweep)."));
    m_sweepDirCombo = new QComboBox(m_sweepRow);
    m_sweepDirCombo->addItems({tr("Rise"), tr("Fall")});
    m_sweepDirCombo->setToolTip(tr("Direction. A rising sweep silences the note when it overflows "
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
    m_dutyCombo->addItems({QStringLiteral("12.5%"), QStringLiteral("25%"), QStringLiteral("50%"),
                           QStringLiteral("75%")});
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
    for (QSpinBox **spin :
         {&m_synthDutySpin, &m_synthStepSpin, &m_synthDepthSpin, &m_synthPhaseSpin}) {
        *spin = new QSpinBox(m_synthParamsRow);
        (*spin)->setRange(0, 255);
        // Commit on steps/editing-finished only: every distinct value here
        // mints a shared definition and reloads the voicegroup, so
        // per-keystroke commits would litter the synth data file.
        (*spin)->setKeyboardTracking(false);
        // A narrow dock narrows all four spins evenly rather than clipping
        // the last one off the edge.
        (*spin)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        synthLayout->addWidget(*spin);
    }
    m_synthDutySpin->setToolTip(tr("Base duty cycle: the pulse width the wave centers on "
                                   "(128 = 50% square)."));
    m_synthStepSpin->setToolTip(tr("Duty LFO step per frame: how fast the pulse width wobbles "
                                   "(0 = static)."));
    m_synthDepthSpin->setToolTip(tr("Modulation amount: how far the pulse width swings around the "
                                    "base duty."));
    m_synthPhaseSpin->setToolTip(tr("Duty LFO phase offset."));
    addRow(tr("Duty LFO"), m_synthParamsRow, &m_synthParamsLabel);

    m_adsrRow = new QWidget(m_editor);
    auto *adsrLayout = new QHBoxLayout(m_adsrRow);
    adsrLayout->setContentsMargins(0, 0, 0, 0);
    adsrLayout->setSpacing(::layout::space(::layout::Space::Half));
    for (QSpinBox **spin : {&m_attackSpin, &m_decaySpin, &m_sustainSpin, &m_releaseSpin}) {
        *spin = new QSpinBox(m_adsrRow);
        (*spin)->setRange(0, 255); // narrowed per voice family on populate
        // A narrow dock narrows all four spins evenly rather than clipping
        // the last one off the edge.
        (*spin)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
    buttonLayout->setContentsMargins(0, ::layout::space(::layout::Space::Half), 0, 0);
    m_newButton = new QPushButton(tr("New…"), buttons);
    m_newButton->setToolTip(tr("Create a new voicegroup file."));
    buttonLayout->addWidget(m_newButton);
    buttonLayout->addStretch(1);
    form->addRow(buttons);
    connect(m_newButton, &QPushButton::clicked, this, &VoicegroupBrowser::newVoicegroupRequested);
    layout->addWidget(m_editor);

    // Space stays the transport toggle even while an editor field has focus:
    // text inputs claim plain printable keys via ShortcutOverride, so the
    // filter refuses Space on their behalf (see eventFilter). The button is
    // a mouse target — keyboard focus on it would also swallow Space.
    m_newButton->setFocusPolicy(Qt::NoFocus);
    m_newSampleButton->setFocusPolicy(Qt::NoFocus);
    m_editSampleButton->setFocusPolicy(Qt::NoFocus);
    // The picker button too — its popup's search box takes focus in its own
    // popup window instead, where plain Space types rather than toggling
    // playback.
    m_samplePicker->setFocusPolicy(Qt::NoFocus);
    for (QWidget *w : std::initializer_list<QWidget *>{
             m_tree, m_vgCombo, m_typeCombo, m_symbolCombo, m_dutyCombo, m_periodCombo,
             m_synthWaveCombo, m_synthDutySpin, m_synthStepSpin, m_synthDepthSpin, m_synthPhaseSpin,
             m_sweepTimeSpin, m_sweepDirCombo, m_sweepShiftSpin, m_attackSpin, m_decaySpin,
             m_sustainSpin, m_releaseSpin}) {
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
    for (QSpinBox *spin :
         {m_sweepTimeSpin, m_sweepShiftSpin, m_synthDutySpin, m_synthStepSpin, m_synthDepthSpin,
          m_synthPhaseSpin, m_attackSpin, m_decaySpin, m_sustainSpin, m_releaseSpin})
        connect(spin, &QSpinBox::valueChanged, this, commit);

    populateEditor();
}

void VoicegroupBrowser::setViewOnly(bool viewOnly)
{
    m_viewOnly = viewOnly;
    m_editor->setEnabled(!viewOnly);
    m_newButton->setEnabled(!viewOnly);
    m_vgCombo->setEnabled(m_vg != nullptr && !viewOnly);
}

void VoicegroupBrowser::setVoicegroup(const LoadedVoiceGroup *vg)
{
    releaseVoice();
    m_vg = vg;
    if (!vg)
        m_source = nullptr; // a cleared voicegroup invalidates the source too
    m_tree->clear();
    m_vgCombo->setEnabled(vg != nullptr && !m_viewOnly);
    m_vgCombo->lineEdit()->setPlaceholderText(vg ? QStringLiteral("dummy") : tr("No song loaded"));
    if (!vg) {
        m_updating = true;
        m_vgArg.clear();
        m_vgCombo->setCurrentText(QString());
        m_updating = false;
        populateEditor();
        return;
    }

    for (int i = 0; i < VOICEGROUP_SIZE; i++) {
        const ToneData &voice = vg->voices[i];
        QString name = QString::fromUtf8(vg->voiceNames[i]).trimmed();
        const QString type =
            toneIsSynth(voice) ? tr("Synth (Golden Sun)") : m4aVoiceTypeName(voice.type);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QStringLiteral("%1  %2")
                             .arg(i, 3, 10, QLatin1Char('0'))
                             .arg(name.isEmpty() ? type : name));
        item->setText(1, type);
        if (voice.type != VOICE_KEYSPLIT && voice.type != VOICE_KEYSPLIT_ALL)
            item->setText(2, QStringLiteral("%1 %2 %3 %4")
                                 .arg(voice.attack)
                                 .arg(voice.decay)
                                 .arg(voice.sustain)
                                 .arg(voice.release));
        item->setData(0, Qt::UserRole, i);
        markUsedRow(item, m_usedVoices.contains(i));
    }
    populateEditor();
}

void VoicegroupBrowser::markUsedRow(QTreeWidgetItem *item, bool used)
{
    // A translucent accent tint over the item surface. indicator_checked_
    // background is the accent's fill role in every theme, and the accent's
    // saturation makes the tint read on the near-neutral item surface where
    // a selection_background tint washes out (the light presets' selection
    // cyan lands within 1.05:1 of the item surface). 30% keeps the row's
    // text above the 4.5:1 bar in all three presets. The brush freezes the
    // color that was current when the row was marked, so changeEvent()
    // re-marks every row on PaletteChange.
    QBrush brush;
    if (used) {
        QColor tint = themes::color(themes::Role::indicator_checked_background);
        tint.setAlphaF(0.30f);
        brush = tint;
    }
    // The tree's count, not the item's: an item without ADSR text (drumkit,
    // keysplit) has no data in column 2 yet, and item->columnCount() would
    // leave that cell untinted.
    for (int col = 0; col < m_tree->columnCount(); col++)
        item->setBackground(col, brush);
    item->setToolTip(0, used ? tr("Used by this song") : QString());
}

// Used-row tints are inked when they are marked; a theme change would
// otherwise leave them in the old theme's selection color.
void VoicegroupBrowser::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        for (int i = 0; i < m_tree->topLevelItemCount(); i++)
            markUsedRow(m_tree->topLevelItem(i), m_usedVoices.contains(i));
    }
}

void VoicegroupBrowser::setUsedVoices(const QSet<int> &used)
{
    if (used == m_usedVoices)
        return;
    m_usedVoices = used;
    for (int i = 0; i < m_tree->topLevelItemCount(); i++)
        markUsedRow(m_tree->topLevelItem(i), used.contains(i));
}

void VoicegroupBrowser::setVoicegroupChoices(const QStringList &args)
{
    if (args == m_vgChoices)
        return;
    m_vgChoices = args;
    m_updating = true;
    const QString text = m_vgCombo->currentText();
    m_vgCombo->clear();
    for (const QString &arg : args)
        m_vgCombo->addItem(SongRegistry::voicegroupDisplayName(arg));
    m_vgCombo->setCurrentText(text);
    m_updating = false;
}

void VoicegroupBrowser::setCurrentVoicegroupArg(const QString &arg)
{
    m_vgArg = arg;
    const QString display = SongRegistry::voicegroupDisplayName(arg);
    if (m_vgCombo->currentText() == display)
        return;
    m_updating = true;
    m_vgCombo->setCurrentText(display);
    m_updating = false;
}

void VoicegroupBrowser::setSource(VoicegroupSource *source, const QStringList &sampleSymbols,
                                  const QStringList &waveSymbols,
                                  const QList<QPair<QString, QString>> &keysplits,
                                  const QStringList &drumkits, const VgAdsrDefaults &adsrDefaults,
                                  const VgSynthCatalog &synths,
                                  const QHash<QString, VgSynthDesc> &pendingSynths,
                                  std::function<QString(const VgSynthDesc &)> mintSynth)
{
    if (source != m_source)
        m_adsrHistory.clear();
    m_source = source;
    m_adsrDefaults = adsrDefaults;
    m_waveSymbols = waveSymbols;
    m_drumkitChoices = drumkits;
    m_synths = synths;
    m_mintSynth = std::move(mintSynth);
    m_synthBySymbol.clear();
    for (const auto &def : m_synths.defs) {
        if (!m_synthBySymbol.contains(def.first)) // first definition wins
            m_synthBySymbol.insert(def.first, def.second);
    }
    for (auto it = pendingSynths.constBegin(); it != pendingSynths.constEnd(); ++it)
        m_synthBySymbol.insert(it.key(), it.value());
    m_keysplitTables.clear();
    m_sampleChoices.clear();
    m_keysplitChoices.clear();
    m_plainSamples.clear();
    m_phonemes.clear();
    for (const auto &pair : keysplits) {
        m_keysplitTables.insert(pair.first, pair.second);
        m_sampleChoices.append(pair.first);
        m_keysplitChoices.append(pair.first);
    }
    m_sampleChoices += sampleSymbols; // already sorted, phonemes last
    for (const QString &symbol : sampleSymbols) {
        (symbol.contains(QStringLiteral("Phoneme")) ? m_phonemes : m_plainSamples).append(symbol);
    }
    // populateEditor hands the picker the list its voice browses (the sample
    // partition, or the wave list for programmable-wave voices).
    populateEditor();
}

void VoicegroupBrowser::setSampleVariants(const QStringList &macroWords, const QStringList &cries)
{
    m_voiceMacroWords = macroWords;
    m_cries = cries;
}

void VoicegroupBrowser::setSampleInfoProvider(std::function<SamplePickInfo(const QString &)> fn)
{
    m_samplePicker->setInfoProvider(std::move(fn));
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

void VoicegroupBrowser::revealSlot(int slot)
{
    selectSlot(slot);
    if (QTreeWidgetItem *item = m_tree->topLevelItem(slot))
        m_tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void VoicegroupBrowser::pressedVoice(QTreeWidgetItem *item)
{
    releaseVoice();
    // itemPressed fires for every button; a right press selects the row for
    // its context menu and a middle one just selects — neither sounds.
    if (!item || m_pressButton != Qt::LeftButton)
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
    if (watched == m_tree->viewport() && event->type() == QEvent::MouseButtonPress)
        m_pressButton = static_cast<QMouseEvent *>(event)->button(); // before itemPressed
    if (watched == m_tree->viewport() && event->type() == QEvent::MouseButtonRelease)
        releaseVoice();
    // Copy/Paste on the focused voice list: claim the chord ahead of any
    // window-level shortcut, and act on the press before QAbstractItemView
    // copies the row's text itself.
    if (watched == m_tree &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const auto &keys = keymap::Registry::instance();
        const bool copy = keys.matches(keyEvent, QStringLiteral("voicegroup.copy"));
        if (copy || keys.matches(keyEvent, QStringLiteral("voicegroup.paste"))) {
            keyEvent->accept();
            if (event->type() == QEvent::KeyPress) {
                if (copy)
                    copyVoice(currentSlot());
                else
                    pasteVoice(currentSlot());
            }
            return true;
        }
        // With voicegroup.copy rebound, QAbstractItemView would still copy
        // the row's display text on the platform Copy chord: eat the press
        // (the override stays unclaimed, so a window shortcut still fires).
        if (event->type() == QEvent::KeyPress && keyEvent->matches(QKeySequence::Copy)) {
            keyEvent->accept();
            return true;
        }
    }
    // Leave plain Space unaccepted so the window-level play/pause shortcut
    // fires instead of the input inserting a space / toggling.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Space && keyEvent->modifiers() == Qt::NoModifier) {
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
    showRow(m_symbolLabel, m_symbolRow, visible && vgMacroHasSymbol(macro));
    showRow(m_sweepLabel, m_sweepRow, visible && macroIsSquare1(macro));
    showRow(m_dutyLabel, m_dutyCombo, visible && (macroIsSquare1(macro) || macroIsSquare2(macro)));
    showRow(m_periodLabel, m_periodCombo, visible && macroIsNoise(macro));
    showRow(m_synthWaveLabel, m_synthWaveCombo, visible && synth);
    // The pulse LFO row narrows further by waveform (populateEditor).
    showRow(m_synthParamsLabel, m_synthParamsRow, visible && synth);
    showRow(m_adsrLabel, m_adsrRow,
            visible && macro != VgMacro::Keysplit && !macroIsDrumkit(macro));
}

bool VoicegroupBrowser::synthDescFor(const VgVoice &voice, int slot, VgSynthDesc *desc) const
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
    const VgVoice *voice = (m_source && slot >= 0) ? m_source->voiceAt(slot) : nullptr;

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
    // variant we display but never offer. The reversed entry carries the
    // voice's own reverse spelling, else the one the project defines (and is
    // left out when the project's scan found neither).
    const int shownData = synth ? kSynthTypeData : int(typeComboMacro(voice->macro));
    m_typeCombo->clear();
    for (VgMacro listed : kSelectableMacros) {
        VgMacro macro = listed;
        if (listed == VgMacro::DirectSoundAlt)
            macro = vgMacroIsReverseSample(voice->macro) && !synth
                        ? voice->macro
                        : vgProjectReverseMacro(m_voiceMacroWords);
        const bool offered = vgMacroIsCompressed(macro)
                                 ? m_voiceMacroWords.contains(vgMacroName(macro))
                                 : !vgMacroIsReverseSample(macro) || int(macro) == shownData ||
                                       sampleMacroDefined(macro);
        if (offered) {
            m_typeCombo->addItem(vgMacroDisplayName(macro), int(macro));
            const QString tip = typeItemTip(macro);
            if (!tip.isEmpty())
                m_typeCombo->setItemData(m_typeCombo->count() - 1, tip, Qt::ToolTipRole);
        }
        if (listed == VgMacro::DirectSoundAlt && (m_synths.available() || synth))
            m_typeCombo->addItem(tr("Synth (Golden Sun)"), kSynthTypeData);
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
        m_symbolLabel->setText(synth     ? tr("Synth")
                               : wave    ? tr("Wave")
                               : drumkit ? tr("Drumkit")
                                         : tr("Sample"));
        // Sample-list voices (plain samples and keysplits) and wave voices
        // use the picker — browsing with audition matters for both; the
        // short drumkit/synth lists keep the combo.
        const bool pickerList = !synth && !drumkit;
        m_symbolCombo->setVisible(!pickerList);
        m_samplePicker->setVisible(pickerList);
        if (pickerList) {
            // Wave symbols read best verbatim; sample symbols drown in their
            // shared DirectSoundWaveData_ prefix.
            m_samplePicker->setDisplayFullSymbols(wave);
            if (wave)
                m_samplePicker->setChoices(QStringList(), m_waveSymbols, QStringList());
            else if (vgMacroIsCompressed(voice->macro))
                m_samplePicker->setChoices(QStringList(), m_cries, QStringList());
            else
                m_samplePicker->setChoices(m_keysplitChoices, m_plainSamples, m_phonemes);
            m_samplePicker->setCurrentSymbol(voice->symbol);
        } else {
            m_symbolCombo->clear();
            if (synth) {
                // The current symbol may be a zero-size .bin descriptor with
                // no set_synth entry; setCurrentText still shows it (editable
                // combo).
                for (const auto &def : m_synths.defs)
                    m_symbolCombo->addItem(def.first);
            } else {
                m_symbolCombo->addItems(m_drumkitChoices);
            }
            m_symbolCombo->setCurrentText(voice->symbol);
        }
        // Not on compressed voices: Sample Editor writes plain samples.
        const bool sampleVoice = !synth && !wave && !drumkit && macroIsDsFamily(voice->macro) &&
                                 !vgMacroIsCompressed(voice->macro);
        m_newSampleButton->setVisible(sampleVoice);
        m_editSampleButton->setVisible(sampleVoice);
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
    const int shownData = curSynth ? kSynthTypeData : int(typeComboMacro(cur->macro));
    const bool typeChanged = selData != shownData;

    if (selData == kSynthTypeData) {
        // A synth voice stays a voice_directsound line; the symbol alone
        // carries the waveform and pulse parameters, deduplicated across the
        // project's definitions (param-named entries are minted on demand).
        // (Never a compressed one: its symbol is read as a cry everywhere.)
        v.macro = !macroIsDsFamily(cur->macro)                   ? VgMacro::DirectSound
                  : cur->macro == VgMacro::DirectSoundCompressed ? VgMacro::DirectSound
                  : cur->macro == VgMacro::DirectSoundCompressedReverse
                      ? vgProjectReverseMacro(m_voiceMacroWords)
                      : cur->macro;
        v.keysplitTable.clear();
        VgSynthDesc desc;
        QString symbol;
        const QString comboSymbol = m_symbolCombo->currentText().trimmed();
        if (!curSynth) {
            // Just switched to Synth: the waveform/param fields are stale, so
            // adopt the project's first definition (or a plain 50% pulse).
            if (!m_synths.defs.isEmpty())
                desc = m_synths.defs.first().second;
        } else if (comboSymbol != cur->symbol && m_synthBySymbol.contains(comboSymbol)) {
            // Picked a definition: honor the symbol as chosen. Resolving it
            // back through the descriptor would land value-equal twins (any
            // two saws compare equal) on whichever one scans first.
            desc = m_synthBySymbol.value(comboSymbol);
            symbol = comboSymbol;
        } else {
            desc.waveform = m_synthWaveCombo->currentIndex();
            desc.baseDuty = m_synthDutySpin->value();
            desc.dutyStep = m_synthStepSpin->value();
            desc.modDepth = m_synthDepthSpin->value();
            desc.phase = m_synthPhaseSpin->value();
            // Saw/triangle definitions carry all-zero pulse params (their
            // macros take none), so switching the waveform to Pulse would
            // commit a duty-0 wave: constant DC, i.e. silence. Adopt the
            // 50%-square default instead, like the Sample-to-Synth path.
            if (desc.waveform == 0 && curDesc.waveform != 0)
                desc = VgSynthDesc{};
        }
        if (symbol.isEmpty()) {
            if (curSynth && desc == curDesc) {
                symbol = cur->symbol; // unchanged (or a duplicate definition)
            } else {
                symbol = synthSymbolFor(desc);
            }
        }
        if (symbol.isEmpty()) {
            populateEditor(); // no definition to point at — refuse the change
            return;
        }
        // Pending definitions are looked up but never listed: the dropdown
        // (m_synths.defs) only gains them once a save lands them on disk.
        m_synthBySymbol.insert(symbol, desc);
        v.symbol = symbol;
    } else if (vgMacroHasSymbol(VgMacro(selData))) {
        // Resolve the instrument symbol first: for sample-list types the
        // chosen symbol decides between a keysplit and a plain sample voice.
        v.macro = VgMacro(selData);
        const bool wave = macroIsWave(v.macro);
        const bool drumkit = macroIsDrumkit(v.macro);
        // The symbol widget only holds this family's list when the current
        // voice shares it (samples and keysplits share the picker's sample
        // list; waves have their own picker list; drumkits and synths each
        // have their own combo list).
        const bool comboShowsThisList = wave      ? macroIsWave(cur->macro)
                                        : drumkit ? macroIsDrumkit(cur->macro)
                                                  : macroUsesSampleList(cur->macro) && !curSynth;
        QString symbol;
        if (comboShowsThisList) {
            symbol = drumkit ? m_symbolCombo->currentText().trimmed()
                             : m_samplePicker->currentSymbol().trimmed();
        }
        // A deliberate Type change away from a keysplit or synth voice
        // overrides the stale symbol shown in the combo.
        if (!wave && !drumkit && typeChanged &&
            (m_keysplitTables.contains(symbol) || m_synthBySymbol.contains(symbol)))
            symbol.clear();
        // Compressed and plain sample voices browse different lists; a Type
        // change between them leaves a symbol from the other one behind.
        const bool compressed = vgMacroIsCompressed(v.macro);
        if (!wave && !drumkit && typeChanged && compressed != vgMacroIsCompressed(cur->macro) &&
            compressed != m_cries.contains(symbol))
            symbol.clear();
        if (symbol.isEmpty()) {
            if (wave)
                symbol = m_waveSymbols.value(0);
            else if (drumkit)
                symbol = m_drumkitChoices.value(0);
            else if (compressed)
                symbol = m_cries.value(0);
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
    const bool crossedFamily =
        newFamily != oldFamily &&
        (oldFamily < 0 || newFamily < 0 || vgMacroIsCgb(v.macro) != vgMacroIsCgb(cur->macro));
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
            if (vgMacroHasSymbol(v.macro) && v.symbol != cur->symbol &&
                VgAdsr{v.attack, v.decay, v.sustain, v.release} ==
                    vgDefaultAdsr(m_adsrDefaults, cur->macro, cur->symbol)) {
                const VgAdsr fresh = vgDefaultAdsr(m_adsrDefaults, v.macro, v.symbol);
                v.attack = fresh.attack;
                v.decay = fresh.decay;
                v.sustain = fresh.sustain;
                v.release = fresh.release;
            }
        }
        // Recompose NR10 from the three fields; bit 7 is unused by the sweep
        // unit, so keep the file's value for it.
        v.sweep = (cur->sweep & 0x80) | (m_sweepTimeSpin->value() << 4) |
                  (m_sweepDirCombo->currentIndex() << 3) | m_sweepShiftSpin->value();
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
    // A synth-to-synth change is a scalar poke even though the symbol
    // changed: only the descriptor bytes move, and the owner patches those
    // into the loaded tone directly (no reload — pending definitions aren't
    // on disk to reload from anyway). That shortcut needs the OUTGOING
    // symbol's descriptor to be resolvable from the definition catalogs,
    // though: a voice classified from raw tone bytes (a zero-size .bin
    // descriptor with no set_synth entry) has nothing to restore those bytes
    // from on undo, so it must take the reload path, where the loader
    // re-reads the .bin itself.
    bool structural = vgVoiceStructuralChange(*cur, v);
    if (curSynth && selData == kSynthTypeData && v.macro == cur->macro &&
        m_synthBySymbol.contains(cur->symbol))
        structural = false;
    emit voiceEditRequested(slot, v, structural);
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
                             ? tr("Synth (Golden Sun)")
                             : m4aVoiceTypeName(vgMacroVoiceType(voice->macro));
    const QString name = vgMacroHasSymbol(voice->macro) ? voice->symbol : QString();
    item->setText(0, QStringLiteral("%1  %2")
                         .arg(slot, 3, 10, QLatin1Char('0'))
                         .arg(name.isEmpty() ? type : name));
    item->setText(1, type);
    item->setText(2, adsrText(*voice));
}

QString VoicegroupBrowser::synthSymbolFor(const VgSynthDesc &desc) const
{
    QString symbol = m_synths.symbolFor(desc); // on-disk definitions
    if (symbol.isEmpty()) {                    // then pending ones
        for (auto it = m_synthBySymbol.constBegin(); it != m_synthBySymbol.constEnd(); ++it) {
            if (it.value() == desc) {
                symbol = it.key();
                break;
            }
        }
    }
    if (symbol.isEmpty() && m_mintSynth)
        symbol = m_mintSynth(desc);
    return symbol;
}

void VoicegroupBrowser::showVoiceMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item)
        return;
    releaseVoice();
    const int slot = item->data(0, Qt::UserRole).toInt();
    m_tree->setCurrentItem(item); // a keyboard-opened menu acts on the row too
    const VgVoice *voice = m_source ? m_source->voiceAt(slot) : nullptr;
    const bool editable = voice != nullptr;
    VgVoice clip;
    std::optional<VgSynthDesc> clipSynth;
    const bool hasClip = clipboardVoice(&clip, &clipSynth, nullptr);

    // The shortcuts are display-only hints here (the real bindings live in
    // eventFilter); mirroring the keymap keeps a rebind from lying.
    const auto &keys = keymap::Registry::instance();
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy"), this, [this, slot] { copyVoice(slot); });
    copy->setShortcut(keys.bindings(QStringLiteral("voicegroup.copy")).value(0));
    copy->setEnabled(editable);
    QAction *paste = menu.addAction(tr("Paste"), this, [this, slot] { pasteVoice(slot); });
    paste->setShortcut(keys.bindings(QStringLiteral("voicegroup.paste")).value(0));
    paste->setEnabled(editable && !m_viewOnly && hasClip);
    menu.addSeparator();
    QAction *clear = menu.addAction(tr("Clear"), this, [this, slot] { clearVoice(slot); });
    clear->setToolTip(tr("Replace this voice with the unused-slot placeholder."));
    clear->setEnabled(editable && !m_viewOnly && !sameVoiceLine(*voice, kPlaceholderVoice));
    const QList<QPair<QString, VgMacro>> variants =
        editable ? voiceVariants(*voice, slot) : QList<QPair<QString, VgMacro>>();
    if (!variants.isEmpty()) {
        menu.addSeparator();
        auto *group = new QActionGroup(&menu);
        group->setExclusive(true);
        for (const auto &[label, macro] : variants) {
            QAction *action = menu.addAction(
                label, this, [this, slot, macro = macro] { setVoiceVariant(slot, macro); });
            action->setCheckable(true);
            action->setChecked(macro == voice->macro || (vgMacroIsReverseSample(macro) &&
                                                         vgMacroIsReverseSample(voice->macro)));
            action->setEnabled(!m_viewOnly);
            action->setToolTip(typeItemTip(macro));
            group->addAction(action);
        }
    }
    menu.setToolTipsVisible(true);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

bool VoicegroupBrowser::sampleMacroDefined(VgMacro macro) const
{
    return !vgMacroIsDirectSound(macro) || m_voiceMacroWords.isEmpty() ||
           m_voiceMacroWords.contains(vgMacroName(macro));
}

QList<QPair<QString, VgMacro>> VoicegroupBrowser::voiceVariants(const VgVoice &voice,
                                                                int slot) const
{
    VgSynthDesc desc;
    if (synthDescFor(voice, slot, &desc))
        return {};
    const QString normal = tr("Normal");
    const QString detuned = tr("Detuned");
    // A reversed voice keeps its own spelling when the project defines it
    // and is offered the twin it does define otherwise (so re-picking the
    // checked Reversed repairs the line); Reversed on any other voice writes
    // the spelling this project defines.
    const VgMacro reverse = vgMacroIsReverseSample(voice.macro)
                                ? vgAdoptDefinedReverse(voice.macro, m_voiceMacroWords)
                                : vgProjectReverseMacro(m_voiceMacroWords);
    QList<QPair<QString, VgMacro>> all;
    switch (voice.macro) {
    case VgMacro::DirectSound:
    case VgMacro::DirectSoundNoResample:
    case VgMacro::DirectSoundAlt:
    case VgMacro::DirectSoundReverse:
        all = {{normal, VgMacro::DirectSound},
               {tr("No Resample"), VgMacro::DirectSoundNoResample},
               {tr("Reversed"), reverse}};
        break;
    case VgMacro::DirectSoundCompressed:
    case VgMacro::DirectSoundCompressedReverse:
        all = {{normal, VgMacro::DirectSoundCompressed},
               {tr("Reversed"), VgMacro::DirectSoundCompressedReverse}};
        break;
    // The detuned _alt PSG macros are vanilla (the scan covers only sample
    // macros): every project defines them.
    case VgMacro::Square1:
    case VgMacro::Square1Alt:
        all = {{normal, VgMacro::Square1}, {detuned, VgMacro::Square1Alt}};
        break;
    case VgMacro::Square2:
    case VgMacro::Square2Alt:
        all = {{normal, VgMacro::Square2}, {detuned, VgMacro::Square2Alt}};
        break;
    case VgMacro::ProgWave:
    case VgMacro::ProgWaveAlt:
        all = {{normal, VgMacro::ProgWave}, {detuned, VgMacro::ProgWaveAlt}};
        break;
    default: // noise (its _alt is inert), keysplit, drumkit
        return {};
    }
    QList<QPair<QString, VgMacro>> offered;
    for (const auto &variant : all) {
        if (variant.second == voice.macro || sampleMacroDefined(variant.second))
            offered.append(variant);
    }
    return offered.size() > 1 ? offered : QList<QPair<QString, VgMacro>>();
}

void VoicegroupBrowser::setVoiceVariant(int slot, VgMacro macro)
{
    const VgVoice *cur = m_source ? m_source->voiceAt(slot) : nullptr;
    if (!cur || m_viewOnly || cur->macro == macro)
        return;
    // Variants share an envelope family, so the symbol, key, pan, and ADSR
    // all carry over: only the macro word changes.
    VgVoice voice = *cur;
    voice.macro = macro;
    emit voiceEditRequested(slot, voice, vgVoiceStructuralChange(*cur, voice),
                            tr("set voice %1 variant").arg(slot));
}

QString VoicegroupBrowser::notEditableReason(int slot) const
{
    if (slot < 0)
        return tr("no voice is selected");
    if (!m_source)
        return tr("this voicegroup's source file couldn't be located");
    switch (m_source->kindAt(slot)) {
    case VgLineKind::ReadOnlyVoice:
        return tr("cry voices are read-only");
    case VgLineKind::Broken:
        return tr("this voice line couldn't be parsed");
    default:
        return tr("no voice is defined at this slot");
    }
}

void VoicegroupBrowser::copyVoice(int slot)
{
    const VgVoice *voice = m_source ? m_source->voiceAt(slot) : nullptr;
    if (!voice) {
        emit statusMessage(tr("Can't copy: %1.").arg(notEditableReason(slot)));
        return;
    }
    const QString line = QString::fromUtf8(VoicegroupSource::renderVoiceLine(*voice));
    QJsonObject payload{{QStringLiteral("line"), line}};
    VgSynthDesc desc;
    if (synthDescFor(*voice, slot, &desc)) {
        payload.insert(QStringLiteral("synth"),
                       QJsonObject{{QStringLiteral("waveform"), desc.waveform},
                                   {QStringLiteral("baseDuty"), desc.baseDuty},
                                   {QStringLiteral("dutyStep"), desc.dutyStep},
                                   {QStringLiteral("modDepth"), desc.modDepth},
                                   {QStringLiteral("phase"), desc.phase}});
    }
    auto *mime = new QMimeData;
    mime->setText(line);
    mime->setData(kVoiceMimeType, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime);
}

bool VoicegroupBrowser::clipboardVoice(VgVoice *voice, std::optional<VgSynthDesc> *synth,
                                       bool *overflow) const
{
    synth->reset();
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime)
        return false;
    if (mime->hasFormat(kVoiceMimeType)) {
        const QJsonObject payload = QJsonDocument::fromJson(mime->data(kVoiceMimeType)).object();
        if (VoicegroupSource::parseVoiceLine(
                payload.value(QStringLiteral("line")).toString().toUtf8(), voice, overflow)) {
            const QJsonValue synthValue = payload.value(QStringLiteral("synth"));
            if (synthValue.isObject()) {
                const QJsonObject o = synthValue.toObject();
                VgSynthDesc desc;
                desc.waveform = std::clamp(o.value(QStringLiteral("waveform")).toInt(), 0, 2);
                desc.baseDuty = o.value(QStringLiteral("baseDuty")).toInt(desc.baseDuty) & 0xFF;
                desc.dutyStep = o.value(QStringLiteral("dutyStep")).toInt() & 0xFF;
                desc.modDepth = o.value(QStringLiteral("modDepth")).toInt() & 0xFF;
                desc.phase = o.value(QStringLiteral("phase")).toInt() & 0xFF;
                *synth = desc;
            }
            return true;
        }
    }
    return mime->hasText() &&
           VoicegroupSource::parseVoiceLine(mime->text().toUtf8(), voice, overflow);
}

bool VoicegroupBrowser::resolvePastedVoice(VgVoice *voice, const std::optional<VgSynthDesc> &synth,
                                           QString *why)
{
    VgFieldRange range;
    if (vgVoiceOutOfRange(*voice, &range)) {
        *why = tr("Can't paste: %1 %2 is out of range (0-%3).")
                   .arg(range.field)
                   .arg(range.value)
                   .arg(range.max);
        return false;
    }
    // Sample macros are gated on what asm/macros defines (an empty list
    // means the scan found nothing to go on, so the line is trusted). A
    // reversed voice keeps its spelling when this project defines it and
    // otherwise lands as the reverse twin this project has.
    voice->macro = vgAdoptDefinedReverse(voice->macro, m_voiceMacroWords);
    if (!sampleMacroDefined(voice->macro)) {
        *why = tr("Can't paste: this project doesn't define %1.").arg(vgMacroName(voice->macro));
        return false;
    }
    if (!vgMacroHasSymbol(voice->macro))
        return true;

    const QString &symbol = voice->symbol;
    const auto refuse = [&](const QString &what) {
        *why = tr("Can't paste: %1 isn't %2 in this project.").arg(symbol, what);
        return false;
    };
    switch (voice->macro) {
    case VgMacro::Keysplit:
        if (!m_keysplitTables.contains(symbol))
            return refuse(tr("a keysplit instrument"));
        // A sub-voicegroup only works with its own table: adopt the one
        // this project pairs it with rather than refusing a mismatch.
        voice->keysplitTable = m_keysplitTables.value(symbol);
        return true;
    case VgMacro::KeysplitAll:
        return m_drumkitChoices.contains(symbol) || refuse(tr("a drumkit"));
    case VgMacro::ProgWave:
    case VgMacro::ProgWaveAlt:
        return m_waveSymbols.contains(symbol) || refuse(tr("a programmable wave"));
    case VgMacro::DirectSoundCompressed:
    case VgMacro::DirectSoundCompressedReverse:
        return m_cries.contains(symbol) || refuse(tr("a compressed (cry) sample"));
    default:
        break;
    }
    // A plain sample voice: a sample, a known synth definition, or a synth
    // only the copy's descriptor knows (pending in another project, or a
    // .bin descriptor with no set_synth entry), resolved the way commitEdit
    // resolves an edited one. A local synth of the same name but another
    // descriptor is a different instrument: the descriptor decides.
    const auto local = m_synthBySymbol.constFind(symbol);
    if (local != m_synthBySymbol.constEnd() && (!synth || local.value() == *synth))
        return true;
    if (local == m_synthBySymbol.constEnd() &&
        (m_plainSamples.contains(symbol) || m_phonemes.contains(symbol)))
        return true;
    if (!synth)
        return refuse(tr("a sample"));
    const QString resolved = synthSymbolFor(*synth);
    if (resolved.isEmpty()) {
        why->clear(); // the mint reported why itself
        return false;
    }
    m_synthBySymbol.insert(resolved, *synth);
    voice->symbol = resolved;
    return true;
}

void VoicegroupBrowser::pasteVoice(int slot)
{
    const VgVoice *cur = m_source ? m_source->voiceAt(slot) : nullptr;
    if (!cur) {
        emit statusMessage(tr("Can't paste: %1.").arg(notEditableReason(slot)));
        return;
    }
    if (m_viewOnly) {
        emit statusMessage(tr("Can't paste: this song is view-only."));
        return;
    }
    VgVoice voice;
    std::optional<VgSynthDesc> synth;
    bool overflow = false;
    if (!clipboardVoice(&voice, &synth, &overflow)) {
        emit statusMessage(tr("Nothing to paste: the clipboard doesn't hold a voicegroup voice "
                              "line."));
        return;
    }
    if (overflow) {
        emit statusMessage(tr("Can't paste: a number in the voice line is out of range."));
        return;
    }
    QString why;
    if (!resolvePastedVoice(&voice, synth, &why)) {
        if (!why.isEmpty())
            emit statusMessage(why);
        return;
    }
    if (sameVoiceLine(voice, *cur))
        return; // pasting what is already there is a no-op
    emit voiceEditRequested(slot, voice, vgVoiceStructuralChange(*cur, voice),
                            tr("paste voice %1").arg(slot));
}

void VoicegroupBrowser::clearVoice(int slot)
{
    const VgVoice *cur = m_source ? m_source->voiceAt(slot) : nullptr;
    if (!cur || m_viewOnly || sameVoiceLine(*cur, kPlaceholderVoice))
        return;
    emit voiceEditRequested(slot, kPlaceholderVoice,
                            vgVoiceStructuralChange(*cur, kPlaceholderVoice),
                            tr("clear voice %1").arg(slot));
}
