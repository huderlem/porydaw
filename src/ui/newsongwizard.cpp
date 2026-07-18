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
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "project/songregistry.h"

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
            tr("Division %1 · %2 chunk(s) → <b>%3 m4a track(s)</b> · peak "
               "%4 simultaneous note(s)")
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

// ---- The wizard -------------------------------------------------------------

NewSongWizard::NewSongWizard(DecompProject *project, QWidget *parent)
    : QWizard(parent), m_project(project)
{
    setWindowTitle(tr("New Song"));
    buildPages(QString());
}

NewSongWizard::NewSongWizard(DecompProject *project, SmfFile imported,
                             const QString &sourcePath, QWidget *parent)
    : QWizard(parent), m_project(project), m_importMode(true),
      m_imported(std::move(imported))
{
    setWindowTitle(tr("Import MIDI — %1").arg(QFileInfo(sourcePath).fileName()));
    // m_imported arrives coerced to format 1 (SmfFile::read), so the
    // analysis and written .mid both deal in one chunk per channel, like
    // every song porydaw opens.
    m_analysis = analyzeForImport(m_imported);
    if (m_imported.wasFormat0)
        m_analysis.warnings.prepend(
            tr("Format 0 file — imported as format 1 (one chunk per channel)."));
    buildPages(sourcePath);
}

void NewSongWizard::buildPages(const QString &sourcePath)
{
    setOption(QWizard::NoBackButtonOnStartPage);
    setMinimumSize(620, 460);

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
    if (m_analysisPage->rescaleSelected())
        rescaleDivision(&smf, m_sound->cfg().extendedClocks ? 48 : 24);
    return smf;
}
