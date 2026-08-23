#include "newsongwizard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "project/songregistry.h"
#include "ui/layout.h"

// "track 4" / "tracks 4 through 9" — ranges stay grammatical when they
// collapse to a single track.
static QString trackRangeText(int first, int last)
{
    return first == last ? QObject::tr("track %1").arg(first)
                         : QObject::tr("tracks %1 through %2").arg(first).arg(last);
}

// ---- Identity: label, constant, music player ------------------------------

// Song labels are lowercase; fold typed capitals (Shift, Caps Lock) into the
// convention instead of swallowing the keystroke.
class LowercaseNameValidator : public QRegularExpressionValidator
{
  public:
    using QRegularExpressionValidator::QRegularExpressionValidator;

    State validate(QString &input, int &pos) const override
    {
        input = input.toLower();
        return QRegularExpressionValidator::validate(input, pos);
    }
};

class IdentityPage : public QWizardPage
{
  public:
    IdentityPage(DecompProject *project, const QString &suggestedLabel) : m_project(project)
    {
        setTitle(tr("Song identity"));
        setSubTitle(tr("Names the .mid file, the song_table.inc entry, and the "
                       "songs.h constant."));

        auto *form = new QFormLayout(this);
        m_name = new QLineEdit(suggestedLabel, this);
        m_name->setPlaceholderText(QStringLiteral("mus_my_song"));
        static const QRegularExpression nameRe(QStringLiteral("[a-z_][a-z0-9_]*"));
        m_name->setValidator(new LowercaseNameValidator(nameRe, this));
        form->addRow(tr("&Name:"), m_name);

        m_nameHint = new QLabel(this);
        m_nameHint->setStyleSheet(QStringLiteral("color: #c05050;"));
        form->addRow(QString(), m_nameHint);

        m_constant = new QLineEdit(this);
        form->addRow(tr("&Constant:"), m_constant);

        m_player = new QComboBox(this);
        for (const MusicPlayer &p : SongRegistry::musicPlayers(project->root()))
            m_player->addItem(playerRoleName(p.name, true), p.name);
        m_player->setToolTip(tr("Select Background music for a song. Select Sound effect for a "
                                "sound. Also select it for a fanfare."));
        form->addRow(tr("&Player:"), m_player);

        connect(m_name, &QLineEdit::textChanged, this, [this](const QString &text) {
            if (!m_constantEdited)
                m_constant->setText(SongRegistry::constantForLabel(text));
            emit completeChanged();
        });
        connect(m_constant, &QLineEdit::textEdited, this, [this] {
            m_constantEdited = true;
            emit completeChanged();
        });
        if (!suggestedLabel.isEmpty())
            m_constant->setText(SongRegistry::constantForLabel(suggestedLabel));
    }

    bool isComplete() const override
    {
        m_nameHint->clear();
        const QString name = m_name->text();
        if (name.isEmpty() || m_constant->text().isEmpty())
            return false;
        for (const SongInfo &song : m_project->songs()) {
            if (song.label == name) {
                m_nameHint->setText(tr("A song named %1 already exists.").arg(name));
                return false;
            }
        }
        if (QFileInfo::exists(m_project->root() +
                              QStringLiteral("/sound/songs/midi/%1.mid").arg(name))) {
            m_nameHint->setText(tr("%1.mid already exists.").arg(name));
            return false;
        }
        return true;
    }

    QString label() const { return m_name->text(); }
    QString constant() const { return m_constant->text(); }
    QString player() const
    {
        const QString data = m_player->currentData().toString();
        return data.isEmpty() ? m_player->currentText() : data;
    }

    void selectPlayer(const QString &name)
    {
        const int index = m_player->findData(name);
        if (index >= 0)
            m_player->setCurrentIndex(index);
    }

    // context receives the connection, so a callback into another page dies
    // with that page rather than dangling.
    void onPlayerChanged(QObject *context, const std::function<void(const QString &)> &callback)
    {
        connect(m_player, &QComboBox::currentIndexChanged, context,
                [this, callback] { callback(player()); });
    }

  private:
    DecompProject *m_project;
    QLineEdit *m_name;
    QLineEdit *m_constant;
    QComboBox *m_player;
    QLabel *m_nameHint;
    bool m_constantEdited = false;
};

// ---- Sound: voicegroup + midi.cfg flags ------------------------------------

class SoundPage : public QWizardPage
{
  public:
    SoundPage(DecompProject *project, const IdentityPage *identity,
              const QStringList &voicegroupArgs)
        : m_identity(identity)
        , m_vgArgs(voicegroupArgs)
    {
        setTitle(tr("Sound settings"));
        setSubTitle(tr("The song's voicegroup and mid2agb flags — its entry in "
                       "midi.cfg (or songs.mk). All of this can be changed later "
                       "in Song Settings."));

        auto *form = new QFormLayout(this);
        m_voicegroup = new QComboBox(this);
        m_voicegroup->setEditable(true);
        // Creating per-file voicegroups needs the sound/voicegroups/ layout
        // (same constraint as the Voicegroup dock's New button).
        m_canCreateVoicegroup =
            QDir(project->root() + QStringLiteral("/sound/voicegroups")).exists();
        if (m_canCreateVoicegroup)
            m_voicegroup->addItem(newVoicegroupText());
        for (const QString &arg : m_vgArgs)
            m_voicegroup->addItem(SongRegistry::voicegroupDisplayName(arg));
        // Default to the first existing voicegroup, not the create entry.
        if (m_canCreateVoicegroup && m_voicegroup->count() > 1)
            m_voicegroup->setCurrentIndex(1);
        m_voicegroup->setToolTip(tr("The symbol is \"voicegroup_\" + this name (mid2agb -G)."));
        form->addRow(tr("&Voicegroup:"), m_voicegroup);

        m_volume = new QSpinBox(this);
        m_volume->setRange(0, 127);
        m_volume->setValue(100);
        form->addRow(tr("&Master volume (-V):"), m_volume);

        m_reverb = new QSpinBox(this);
        m_reverb->setRange(0, 127);
        m_reverb->setValue(SongCfg::kDefaultReverb);
        form->addRow(tr("&Reverb (-R):"), m_reverb);

        m_priority = new QSpinBox(this);
        m_priority->setRange(0, 127);
        form->addRow(tr("&Priority (-P):"), m_priority);

        m_exactGate = new QCheckBox(tr("Exact gate time (-E)"), this);
        m_exactGate->setChecked(true);
        form->addRow(QString(), m_exactGate);
        m_extendedClocks = new QCheckBox(tr("48 clocks per beat (-X)"), this);
        form->addRow(QString(), m_extendedClocks);
        m_noCompression = new QCheckBox(tr("Disable compression (-N)"), this);
        form->addRow(QString(), m_noCompression);
    }

    // The new voicegroup is named after the song: sound/voicegroups/<label>.inc,
    // symbol voicegroup_<label>, -G arg "_<label>".
    bool newVoicegroupSelected() const
    {
        return m_canCreateVoicegroup && m_voicegroup->currentText() == newVoicegroupText();
    }

    bool validatePage() override
    {
        if (newVoicegroupSelected() &&
            m_vgArgs.contains(QStringLiteral("_") + m_identity->label())) {
            QMessageBox::warning(this, tr("New Voicegroup"),
                                 tr("A voicegroup named voicegroup_%1 already exists — pick it "
                                    "from the list instead.")
                                     .arg(m_identity->label()));
            return false;
        }
        return true;
    }

    SongCfg cfg() const
    {
        SongCfg cfg;
        cfg.voicegroupArg = newVoicegroupSelected()
                                ? QStringLiteral("_") + m_identity->label()
                                : SongRegistry::voicegroupArgFromDisplay(
                                      m_voicegroup->currentText().trimmed(), m_vgArgs);
        cfg.masterVolume = m_volume->value();
        cfg.reverb = m_reverb->value();
        cfg.priority = m_priority->value();
        cfg.exactGate = m_exactGate->isChecked();
        cfg.extendedClocks = m_extendedClocks->isChecked();
        cfg.noCompression = m_noCompression->isChecked();
        cfg.rawFlags = SongRegistry::mergeCfgFlags(cfg);
        return cfg;
    }

  private:
    static QString newVoicegroupText() { return tr("(create a new voicegroup for this song)"); }

    const IdentityPage *m_identity;
    bool m_canCreateVoicegroup = false;
    QStringList m_vgArgs;
    QComboBox *m_voicegroup;
    QSpinBox *m_volume;
    QSpinBox *m_reverb;
    QSpinBox *m_priority;
    QCheckBox *m_exactGate;
    QCheckBox *m_extendedClocks;
    QCheckBox *m_noCompression;
};

// ---- Analysis: what the import will change ----------------------------------

class AnalysisPage : public QWizardPage
{
  public:
    AnalysisPage(const SmfFile &smf, const ImportAnalysis &analysis,
                 const QVector<MusicPlayer> &players, const QString &sourcePath,
                 IdentityPage *identity)
        : m_smf(smf)
        , m_analysis(analysis)
        , m_players(players)
        , m_identity(identity)
    {
        setTitle(tr("Check the MIDI file"));
        setSubTitle(QFileInfo(sourcePath).fileName());

        auto *layout = new QVBoxLayout(this);
        const int zeroSpace = ::layout::space(::layout::Space::Zero);
        const int inlineSpace = ::layout::space(::layout::Space::Two);
        const int verticalSpace = ::layout::space(::layout::Space::Three);
        const int pageMargin = ::layout::space(::layout::Space::Three);
        layout->setContentsMargins(pageMargin, pageMargin, pageMargin, pageMargin);
        layout->setSpacing(verticalSpace);
        auto *roleForm = new QFormLayout;
        roleForm->setContentsMargins(zeroSpace, zeroSpace, zeroSpace, zeroSpace);
        roleForm->setHorizontalSpacing(inlineSpace);
        roleForm->setVerticalSpacing(verticalSpace);
        m_player = new QComboBox(this);
        for (const MusicPlayer &player : m_players)
            m_player->addItem(playerRoleName(player.name, false), player.name);
        m_player->setToolTip(tr("Select Background music for a song. Select Sound effect for a "
                                "sound. Also select it for a fanfare."));
        roleForm->addRow(tr("The song will be used as:"), m_player);
        layout->addLayout(roleForm);
        layout->addSpacing(::layout::space(::layout::Space::One));

        m_fileTracks = new QLabel(this);
        layout->addWidget(m_fileTracks);
        m_gameTrackLimit = new QLabel(this);
        layout->addWidget(m_gameTrackLimit);

        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        layout->addWidget(m_status);

        m_summary = new QLabel(this);
        m_summary->setWordWrap(true);
        layout->addWidget(m_summary);
        m_trackAction = makeNotice(layout);
        m_polyphony = makeNotice(layout);
        m_defaultInstrument = makeNotice(layout);
        m_format = makeNotice(layout);
        m_controller = makeNotice(layout);

        if (m_smf.division % 24 != 0) {
            m_rescale = new QCheckBox(
                tr("Adjust note timing for the Game Boy Advance (recommended)"), this);
            m_rescale->setChecked(true);
            m_rescale->setToolTip(tr("Use this adjustment to make the note timing in Porydaw agree "
                                     "with the note timing in the game."));
            layout->addWidget(m_rescale);
        }

        if (!m_analysis.ccs.empty()) {
            auto *ccToggle = new QToolButton(this);
            ccToggle->setText(tr("CC commands"));
            ccToggle->setCheckable(true);
            ccToggle->setAutoRaise(true);
            ccToggle->setArrowType(Qt::RightArrow);
            ccToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            layout->addWidget(ccToggle);

            auto *ccBody = new QWidget(this);
            auto *ccLayout = new QVBoxLayout(ccBody);
            ccLayout->setContentsMargins(::layout::space(::layout::Space::Eight), zeroSpace,
                                         zeroSpace, zeroSpace);
            ccLayout->setSpacing(verticalSpace);
            auto *tree = new QTreeWidget(ccBody);
            tree->setColumnCount(4);
            tree->setHeaderLabels(
                {tr("Controller"), tr("Function"), tr("Events"), tr("In the game")});
            tree->setRootIsDecorated(false);
            tree->setUniformRowHeights(true);
            tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
            for (const ImportCcUsage &cc : m_analysis.ccs) {
                auto *item = new QTreeWidgetItem(tree);
                item->setText(0, QStringLiteral("CC %1").arg(cc.cc));
                item->setText(1, cc.label);
                item->setText(2, QString::number(cc.count));
                item->setText(3, cc.audible ? tr("Yes") : tr("No"));
                if (!cc.audible)
                    item->setForeground(3, QBrush(QColor(0xc0, 0x80, 0x30)));
            }
            ccLayout->addWidget(tree);
            ccBody->setVisible(false);
            layout->addWidget(ccBody);
            connect(ccToggle, &QToolButton::toggled, this, [ccToggle, ccBody](bool on) {
                ccBody->setVisible(on);
                ccToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            });
        }
        layout->addStretch(1);
        connect(m_player, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
        m_identity->onPlayerChanged(this, [this](const QString &name) { selectPlayer(name); });
        refresh();
    }

    bool rescaleSelected() const { return m_rescale && m_rescale->isChecked(); }

  private:
    QLabel *makeNotice(QVBoxLayout *layout)
    {
        auto *notice = new QLabel(this);
        notice->setWordWrap(true);
        notice->setStyleSheet(QStringLiteral("color: #c08030;"));
        layout->addWidget(notice);
        return notice;
    }

    void selectPlayer(const QString &name)
    {
        const int index = m_player->findData(name);
        if (index >= 0)
            m_player->setCurrentIndex(index);
    }

    void setNotice(QLabel *label, const QString &text)
    {
        label->setText(text);
        label->setVisible(!text.isEmpty());
    }

    void refresh()
    {
        const int index = m_player->currentIndex();
        if (index < 0 || index >= m_players.size())
            return;
        const MusicPlayer &player = m_players.at(index);
        const int trackLimit = player.trackCount < 0 ? 16 : std::min(player.trackCount, 16);
        m_analysis = analyzeForImport(m_smf, trackLimit, player.name);
        m_identity->selectPlayer(player.name);

        const int sourceTracks = m_analysis.mappedTracks + m_analysis.droppedTracks;
        m_fileTracks->setText(tr("This file has %1.").arg(trackCountPhrase(sourceTracks)));
        m_gameTrackLimit->setText(
            tr("The maximum number of tracks in the game is %1.").arg(trackLimit));

        QStringList status;
        if (m_analysis.droppedTracks > 0) {
            status.append(tr("Porydaw will not import %1.")
                              .arg(trackRangeText(m_analysis.mappedTracks + 1, sourceTracks)));
        }
        if (status.isEmpty() && m_analysis.silentTracks == 0)
            status.append(tr("Porydaw can import this MIDI file."));
        m_status->setText(QStringLiteral("<b>%1</b>").arg(status.join(QStringLiteral("<br>"))));
        m_status->setVisible(!status.isEmpty());

        QStringList actions;
        if (m_analysis.droppedTracks > 0) {
            actions.append(
                tr("To import all tracks, use a maximum of 16 tracks in the MIDI file."));
        }
        if (m_analysis.silentTracks > 0) {
            actions.append(tr("To play all tracks in the game, use a maximum of %1 in the "
                              "MIDI file.")
                               .arg(trackCountPhrase(trackLimit)));
            actions.append(
                m_analysis.silentTracks == 1
                    ? tr("After import, move track %1 above track %2 to play it.")
                          .arg(m_analysis.mappedTracks)
                          .arg(trackLimit)
                    : tr("After import, move tracks %1 through %2 above track %3 to play them.")
                          .arg(trackLimit + 1)
                          .arg(m_analysis.mappedTracks)
                          .arg(trackLimit));
        }
        setNotice(m_trackAction, actions.join(QLatin1Char(' ')));

        if (m_analysis.droppedTracks > 0) {
            QString text = tr("Porydaw will import %1 of %2 tracks. It will not import %3.")
                               .arg(m_analysis.mappedTracks)
                               .arg(sourceTracks)
                               .arg(trackCountPhrase(m_analysis.droppedTracks));
            if (m_analysis.silentTracks > 0) {
                text += tr(" The game will mute %1.")
                            .arg(trackRangeText(trackLimit + 1, m_analysis.mappedTracks));
            }
            m_summary->setText(text);
        } else if (m_analysis.silentTracks > 0) {
            m_summary->setText(tr("Porydaw will import all %1 tracks, but the game will mute %2.")
                                   .arg(m_analysis.mappedTracks)
                                   .arg(trackRangeText(trackLimit + 1, m_analysis.mappedTracks)));
        } else if (m_analysis.mappedTracks == 1) {
            m_summary->setText(tr("Porydaw will import the file's only track."));
        } else {
            m_summary->setText(
                tr("Porydaw will import all %1 tracks.").arg(m_analysis.mappedTracks));
        }
        setNotice(m_polyphony, m_analysis.peakConcurrentNotes > m_analysis.sampleNoteLimit
                                   ? concurrencyNoticeText(m_analysis.peakConcurrentNotes,
                                                           m_analysis.sampleNoteLimit)
                                   : QString());

        const bool notesBeforeInstrument = std::any_of(
            m_analysis.tracks.cbegin(), m_analysis.tracks.cend(), [](const ImportTrackInfo &track) {
                return track.noteCount > 0 && track.notesBeforeProgram;
            });
        setNotice(m_defaultInstrument,
                  notesBeforeInstrument ? instrumentFallbackNoticeText() : QString());
        setNotice(m_format, m_smf.wasFormat0
                                ? tr("This MIDI file contains all channels in one track. Porydaw "
                                     "will put each channel in a different track.")
                                : QString());
        const bool hasSilentController =
            std::any_of(m_analysis.ccs.cbegin(), m_analysis.ccs.cend(),
                        [](const ImportCcUsage &cc) { return !cc.audible; });
        setNotice(m_controller, hasSilentController
                                    ? tr("Some CC commands do not change the sound in the game.")
                                    : QString());
    }

    const SmfFile &m_smf;
    ImportAnalysis m_analysis;
    QVector<MusicPlayer> m_players;
    IdentityPage *m_identity;
    QComboBox *m_player;
    QLabel *m_fileTracks;
    QLabel *m_gameTrackLimit;
    QLabel *m_status;
    QLabel *m_trackAction;
    QLabel *m_summary;
    QLabel *m_polyphony;
    QLabel *m_defaultInstrument;
    QLabel *m_format;
    QLabel *m_controller;
    QCheckBox *m_rescale = nullptr;
};

// ---- The wizard -------------------------------------------------------------

NewSongWizard::NewSongWizard(DecompProject *project, const QStringList &voicegroupArgs,
                             QWidget *parent)
    : QWizard(parent)
    , m_project(project)
{
    setWindowTitle(tr("New Song"));
    buildPages(QString(), voicegroupArgs);
}

NewSongWizard::NewSongWizard(DecompProject *project, SmfFile imported, const QString &sourcePath,
                             const QStringList &voicegroupArgs, QWidget *parent)
    : QWizard(parent)
    , m_project(project)
    , m_importMode(true)
    , m_imported(std::move(imported))
{
    setWindowTitle(tr("Import MIDI — %1").arg(QFileInfo(sourcePath).fileName()));
    const QVector<MusicPlayer> players = SongRegistry::musicPlayers(project->root());
    const MusicPlayer &defaultPlayer = players.first();
    m_analysis = analyzeForImport(m_imported, defaultPlayer.trackCount, defaultPlayer.name);
    buildPages(sourcePath, voicegroupArgs);
}

void NewSongWizard::buildPages(const QString &sourcePath, const QStringList &voicegroupArgs)
{
    setOption(QWizard::NoBackButtonOnStartPage);
    setMinimumSize(::layout::fontPx(m_importMode ? 60 : 52),
                   ::layout::fontPx(m_importMode ? 44 : 38));

    QString suggested;
    if (m_importMode) {
        // "Cool Song.mid" -> "mus_cool_song"
        suggested = QFileInfo(sourcePath).completeBaseName().toLower();
        suggested.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
        suggested.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
        if (!suggested.startsWith(QStringLiteral("mus_")) &&
            !suggested.startsWith(QStringLiteral("se_")))
            suggested.prepend(QStringLiteral("mus_"));
    }

    m_identity = new IdentityPage(m_project, suggested);
    if (m_importMode) {
        const QVector<MusicPlayer> players = SongRegistry::musicPlayers(m_project->root());
        m_analysisPage = new AnalysisPage(m_imported, m_analysis, players, sourcePath, m_identity);
        addPage(m_analysisPage);
    }
    addPage(m_identity);
    m_sound = new SoundPage(m_project, m_identity, voicegroupArgs);
    addPage(m_sound);
}

QString NewSongWizard::label() const
{
    return m_identity->label();
}

QString NewSongWizard::constant() const
{
    return m_identity->constant();
}

QString NewSongWizard::player() const
{
    return m_identity->player();
}

SongCfg NewSongWizard::cfg() const
{
    return m_sound->cfg();
}

QString NewSongWizard::newVoicegroupName() const
{
    return m_sound->newVoicegroupSelected() ? m_identity->label() : QString();
}

SmfFile NewSongWizard::songFile() const
{
    if (!m_importMode)
        return SongRegistry::blankSong();
    SmfFile smf = m_imported;
    // Tempo first: the game reads tempo from the first chunk only, so a
    // foreign file's later-chunk tempo map must move there to play at all.
    // Before the dedup, which then collapses the same-tick losers the move
    // brings together.
    moveTempoMetasToFirstChunk(&smf);
    // Before the rescale, so only duplicates present in the source collapse —
    // tick collisions the floor rescale itself creates were distinct points
    // the author drew, and they play the same either way.
    removeRedundantSetterEvents(&smf);
    if (m_analysisPage->rescaleSelected())
        rescaleDivision(&smf, m_sound->cfg().extendedClocks ? 48 : 24);
    return smf;
}
