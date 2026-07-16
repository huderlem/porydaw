#include "newsongwizard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "audio/audioengine.h"
#include "project/songregistry.h"
#include "ui/layout.h"
#include "ui/m4asemantics.h"

// ---- Identity: label, constant, music player ------------------------------

class IdentityPage : public QWizardPage
{
public:
    IdentityPage(DecompProject *project, const QString &suggestedLabel)
        : m_project(project)
    {
        setTitle(tr("Song identity"));
        setSubTitle(tr("Names the .mid file, the song_table.inc entry, and the "
                       "songs.h constant."));

        auto *form = new QFormLayout(this);
        m_name = new QLineEdit(suggestedLabel, this);
        m_name->setPlaceholderText(QStringLiteral("mus_my_song"));
        static const QRegularExpression nameRe(QStringLiteral("[a-z_][a-z0-9_]*"));
        m_name->setValidator(new QRegularExpressionValidator(nameRe, this));
        form->addRow(tr("&Name:"), m_name);

        m_nameHint = new QLabel(this);
        m_nameHint->setStyleSheet(QStringLiteral("color: #c05050;"));
        form->addRow(QString(), m_nameHint);

        m_constant = new QLineEdit(this);
        form->addRow(tr("&Constant:"), m_constant);

        m_player = new QComboBox(this);
        for (const MusicPlayer &p : SongRegistry::musicPlayers(project->root()))
            m_player->addItem(p.name);
        m_player->setToolTip(tr("BGM for music; SE players for sound effects and "
                                "fanfares that interrupt music."));
        form->addRow(tr("&Player:"), m_player);

        connect(m_name, &QLineEdit::textChanged, this, [this](const QString &text) {
            if (!m_constantEdited)
                m_constant->setText(SongRegistry::constantForLabel(text));
            emit completeChanged();
        });
        connect(m_constant, &QLineEdit::textEdited, this,
                [this] { m_constantEdited = true; emit completeChanged(); });
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
        if (QFileInfo::exists(m_project->root()
                              + QStringLiteral("/sound/songs/midi/%1.mid").arg(name))) {
            m_nameHint->setText(tr("%1.mid already exists.").arg(name));
            return false;
        }
        return true;
    }

    QString label() const { return m_name->text(); }
    QString constant() const { return m_constant->text(); }
    QString player() const { return m_player->currentText(); }

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
    SoundPage(DecompProject *project, const IdentityPage *identity)
        : m_project(project), m_identity(identity)
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
        m_voicegroup->addItems(SongRegistry::voicegroupArgs(project->root()));
        // Default to the first existing voicegroup, not the create entry.
        if (m_canCreateVoicegroup && m_voicegroup->count() > 1)
            m_voicegroup->setCurrentIndex(1);
        m_voicegroup->setToolTip(
            tr("mid2agb -G: appended to \"voicegroup\" to form the symbol."));
        form->addRow(tr("&Voicegroup (-G):"), m_voicegroup);

        m_volume = new QSpinBox(this);
        m_volume->setRange(0, 127);
        m_volume->setValue(100);
        form->addRow(tr("&Master volume (-V):"), m_volume);

        auto *reverbRow = new QGridLayout;
        m_reverbOn = new QCheckBox(tr("Override"), this);
        m_reverbOn->setChecked(true);
        m_reverb = new QSpinBox(this);
        m_reverb->setRange(0, 127);
        m_reverb->setValue(50);
        connect(m_reverbOn, &QCheckBox::toggled, m_reverb, &QSpinBox::setEnabled);
        reverbRow->addWidget(m_reverbOn, 0, 0);
        reverbRow->addWidget(m_reverb, 0, 1);
        form->addRow(tr("&Reverb (-R):"), reverbRow);

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
        if (newVoicegroupSelected()
            && SongRegistry::voicegroupArgs(m_project->root())
                   .contains(QStringLiteral("_") + m_identity->label())) {
            QMessageBox::warning(
                this, tr("New Voicegroup"),
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
                                : m_voicegroup->currentText().trimmed();
        cfg.masterVolume = m_volume->value();
        cfg.reverb = m_reverbOn->isChecked() ? m_reverb->value() : -1;
        cfg.priority = m_priority->value();
        cfg.exactGate = m_exactGate->isChecked();
        cfg.extendedClocks = m_extendedClocks->isChecked();
        cfg.noCompression = m_noCompression->isChecked();
        cfg.rawFlags = SongRegistry::mergeCfgFlags(cfg);
        return cfg;
    }

private:
    static QString newVoicegroupText()
    {
        return tr("(create a new voicegroup for this song)");
    }

    DecompProject *m_project;
    const IdentityPage *m_identity;
    bool m_canCreateVoicegroup = false;
    QComboBox *m_voicegroup;
    QSpinBox *m_volume;
    QCheckBox *m_reverbOn;
    QSpinBox *m_reverb;
    QSpinBox *m_priority;
    QCheckBox *m_exactGate;
    QCheckBox *m_extendedClocks;
    QCheckBox *m_noCompression;
};

// ---- Analysis: what's in the external file ---------------------------------

class AnalysisPage : public QWizardPage
{
public:
    AnalysisPage(const ImportAnalysis &a, const QString &sourcePath)
    {
        setTitle(tr("MIDI analysis"));
        setSubTitle(QFileInfo(sourcePath).fileName());

        auto *layout = new QVBoxLayout(this);
        auto *summary = new QLabel(
            tr("Format %1, division %2 · %3 chunk(s) → <b>%4 m4a track(s)</b> · peak "
               "%5 simultaneous note(s)")
                .arg(a.format)
                .arg(a.division)
                .arg(a.smfTrackCount)
                .arg(a.mappedTracks)
                .arg(a.peakConcurrentNotes),
            this);
        summary->setWordWrap(true);
        layout->addWidget(summary);

        for (const QString &warning : a.warnings) {
            auto *w = new QLabel(QStringLiteral("⚠ ") + warning, this);
            w->setWordWrap(true);
            w->setStyleSheet(QStringLiteral("color: #c08030;"));
            layout->addWidget(w);
        }

        if (a.division % 24 != 0) {
            m_rescale = new QCheckBox(
                tr("Rescale timing to the m4a clock grid (24 clocks per beat, "
                   "48 with -X)"),
                this);
            m_rescale->setChecked(true);
            m_rescale->setToolTip(
                tr("Rewrites every event tick with the same rounding mid2agb "
                   "applies, so the editor grid matches playback exactly. "
                   "Uncheck to keep the file's original ticks."));
            layout->addWidget(m_rescale);
        }

        auto *tree = new QTreeWidget(this);
        tree->setColumnCount(3);
        tree->setHeaderLabels({tr("Controller"), tr("m4a meaning"), tr("Events")});
        tree->setRootIsDecorated(false);
        tree->setUniformRowHeights(true);
        tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        for (const ImportCcUsage &cc : a.ccs) {
            auto *item = new QTreeWidgetItem(tree);
            item->setText(0, QStringLiteral("CC %1").arg(cc.cc));
            item->setText(1, cc.audible ? cc.label
                                        : tr("%1 (kept, but silent in-game)").arg(cc.label));
            item->setText(2, QString::number(cc.count));
            if (!cc.audible)
                item->setForeground(1, QBrush(QColor(0xc0, 0x80, 0x30)));
        }
        layout->addWidget(new QLabel(tr("Controllers used:"), this));
        layout->addWidget(tree, 1);
    }

    bool rescaleSelected() const { return m_rescale && m_rescale->isChecked(); }

private:
    QCheckBox *m_rescale = nullptr;
};

// ---- Mapping: programs vs. the chosen voicegroup ---------------------------

class MappingPage : public QWizardPage
{
public:
    MappingPage(DecompProject *project, AudioEngine *audio, const ImportAnalysis &a,
                const SoundPage *sound)
        : m_project(project), m_audio(audio), m_analysis(a), m_sound(sound)
    {
        setTitle(tr("Instrument mapping"));
        setSubTitle(tr("Each program the file uses, against the chosen voicegroup. "
                       "Remap any program to a different voice; hold Play to "
                       "audition."));

        auto *layout = new QVBoxLayout(this);
        m_vgLabel = new QLabel(this);
        m_vgLabel->setWordWrap(true);
        layout->addWidget(m_vgLabel);

        m_tree = new QTreeWidget(this);
        m_tree->setColumnCount(4);
        m_tree->setHeaderLabels({tr("Track"), tr("Program"), tr("Voice"), QString()});
        m_tree->setRootIsDecorated(false);
        m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        layout->addWidget(m_tree, 1);
    }

    ~MappingPage() override { releaseVoicegroup(); }

    void initializePage() override
    {
        releaseVoicegroup();
        m_rows.clear();
        m_tree->clear();

        const SongCfg cfg = m_sound->cfg();
        if (!m_sound->newVoicegroupSelected()) {
            const QByteArray root = m_project->root().toLocal8Bit();
            for (const QString &name : DecompProject::voicegroupCandidates(cfg)) {
                m_vg = voicegroup_load(root.constData(), name.toLocal8Bit().constData(),
                                       nullptr);
                if (m_vg)
                    break;
            }
        }
        if (m_vg) {
            m_audio->setPreviewVoicegroup(m_vg);
            m_vgLabel->setText(tr("Voicegroup <b>voicegroup%1</b>:").arg(cfg.voicegroupArg));
        } else if (m_sound->newVoicegroupSelected()) {
            m_vgLabel->setText(
                tr("A new voicegroup <b>voicegroup%1</b> is created with the song — "
                   "programs are kept as-is; configure its voices in the Voicegroup "
                   "dock afterwards.")
                    .arg(cfg.voicegroupArg));
        } else {
            m_vgLabel->setText(
                tr("⚠ Voicegroup \"voicegroup%1\" could not be loaded — voice names "
                   "and audition are unavailable, but the mapping still applies.")
                    .arg(cfg.voicegroupArg));
        }

        for (int et = 0; et < int(m_analysis.tracks.size()); et++) {
            const ImportTrackInfo &track = m_analysis.tracks[et];
            for (uint8_t program : track.programs) {
                auto *item = new QTreeWidgetItem(m_tree);
                QString trackName = tr("Track %1").arg(et + 1);
                if (!track.name.isEmpty())
                    trackName += QStringLiteral(" (%1)").arg(track.name);
                item->setText(0, trackName);
                item->setText(1, QString::number(program));

                auto *combo = new QComboBox(m_tree);
                for (int v = 0; v < VOICEGROUP_SIZE; v++)
                    combo->addItem(QStringLiteral("%1  %2").arg(v, 3, 10, QLatin1Char('0'))
                                       .arg(voiceName(v)));
                combo->setCurrentIndex(program);
                m_tree->setItemWidget(item, 2, combo);

                auto *play = new QPushButton(tr("Play"), m_tree);
                play->setEnabled(m_vg != nullptr);
                connect(play, &QPushButton::pressed, this, [this, combo] {
                    m_audio->previewVoice(uint8_t(combo->currentIndex()), 60, 112);
                });
                connect(play, &QPushButton::released, this, [this, combo] {
                    m_audio->previewVoice(uint8_t(combo->currentIndex()), 60, 0);
                });
                m_tree->setItemWidget(item, 3, play);

                m_rows.push_back({track.smfTrack, track.channel, program, combo});
            }
        }
        if (m_rows.empty())
            m_vgLabel->setText(m_vgLabel->text()
                               + tr("<br>The file has no program changes; every note "
                                    "plays voice 0."));
    }

    void cleanupPage() override { releaseVoicegroup(); }

    std::vector<ProgramRemap> remaps() const
    {
        std::vector<ProgramRemap> result;
        for (const Row &row : m_rows) {
            if (row.combo->currentIndex() != row.program)
                result.push_back({row.smfTrack, row.channel, row.program,
                                  uint8_t(row.combo->currentIndex())});
        }
        return result;
    }

    // Releases only the voicegroup/preview; the mapping rows stay readable —
    // songFile() collects the remaps after the wizard has finished.
    void releaseVoicegroup()
    {
        if (!m_vg)
            return;
        m_audio->setPreviewVoicegroup(nullptr);
        voicegroup_free(m_vg);
        m_vg = nullptr;
    }

private:
    QString voiceName(int v) const
    {
        if (!m_vg)
            return tr("Voice %1").arg(v);
        const QString name = QString::fromUtf8(m_vg->voiceNames[v]).trimmed();
        const QString type = m4aVoiceTypeName(m_vg->voices[v].type);
        return name.isEmpty() ? type : QStringLiteral("%1 (%2)").arg(name, type);
    }

    struct Row {
        int smfTrack;
        uint8_t channel;
        uint8_t program;
        QComboBox *combo;
    };

    DecompProject *m_project;
    AudioEngine *m_audio;
    ImportAnalysis m_analysis;
    const SoundPage *m_sound;
    LoadedVoiceGroup *m_vg = nullptr;
    QLabel *m_vgLabel;
    QTreeWidget *m_tree;
    std::vector<Row> m_rows;
};

// ---- The wizard -------------------------------------------------------------

NewSongWizard::NewSongWizard(DecompProject *project, AudioEngine *audio, QWidget *parent)
    : QWizard(parent), m_project(project), m_audio(audio)
{
    setWindowTitle(tr("New Song"));
    buildPages(QString());
}

NewSongWizard::NewSongWizard(DecompProject *project, AudioEngine *audio, SmfFile imported,
                             const QString &sourcePath, QWidget *parent)
    : QWizard(parent), m_project(project), m_audio(audio), m_importMode(true),
      m_imported(std::move(imported))
{
    setWindowTitle(tr("Import MIDI — %1").arg(QFileInfo(sourcePath).fileName()));
    m_analysis = analyzeForImport(m_imported);
    buildPages(sourcePath);
}

NewSongWizard::~NewSongWizard()
{
    if (m_mapping)
        m_mapping->releaseVoicegroup();
}

void NewSongWizard::buildPages(const QString &sourcePath)
{
    setOption(QWizard::NoBackButtonOnStartPage);
    setMinimumSize(::layout::fontPx(52), ::layout::fontPx(38));

    QString suggested;
    if (m_importMode) {
        // "Cool Song.mid" -> "mus_cool_song"
        suggested = QFileInfo(sourcePath).completeBaseName().toLower();
        suggested.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                          QStringLiteral("_"));
        suggested.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
        if (!suggested.startsWith(QStringLiteral("mus_"))
            && !suggested.startsWith(QStringLiteral("se_")))
            suggested.prepend(QStringLiteral("mus_"));
        m_analysisPage = new AnalysisPage(m_analysis, sourcePath);
        addPage(m_analysisPage);
    }

    m_identity = new IdentityPage(m_project, suggested);
    addPage(m_identity);
    m_sound = new SoundPage(m_project, m_identity);
    addPage(m_sound);

    if (m_importMode) {
        m_mapping = new MappingPage(m_project, m_audio, m_analysis, m_sound);
        addPage(m_mapping);
    }

    // The preview voicegroup must be released before MainWindow acts on the
    // result (it loads the new song, which resets the preview engine).
    connect(this, &QDialog::finished, this, [this](int) {
        if (m_mapping)
            m_mapping->releaseVoicegroup();
    });
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
    applyProgramRemaps(&smf, m_mapping->remaps());
    if (m_analysisPage->rescaleSelected())
        rescaleDivision(&smf, m_sound->cfg().extendedClocks ? 48 : 24);
    return smf;
}
