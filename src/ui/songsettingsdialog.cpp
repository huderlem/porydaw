#include "songsettingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

SongSettingsDialog::SongSettingsDialog(const SongCfg &cfg, const QString &songLabel,
                                       const QStringList &voicegroupArgs, QWidget *parent)
    : QDialog(parent), m_original(cfg)
{
    setWindowTitle(tr("Song Settings — %1").arg(songLabel));

    auto *form = new QFormLayout;

    m_voicegroup = new QComboBox(this);
    m_voicegroup->setEditable(true);
    m_voicegroup->addItems(voicegroupArgs);
    m_voicegroup->setCurrentText(cfg.voicegroupArg);
    m_voicegroup->lineEdit()->setPlaceholderText(QStringLiteral("_dummy"));
    m_voicegroup->setToolTip(tr("mid2agb -G: appended to \"voicegroup\" to form the symbol, "
                                "e.g. \"_abandoned_ship\" → voicegroup_abandoned_ship."));
    form->addRow(tr("&Voicegroup (-G):"), m_voicegroup);

    m_volume = new QSpinBox(this);
    m_volume->setRange(0, 127);
    m_volume->setValue(cfg.masterVolume);
    m_volume->setToolTip(tr("mid2agb -V: scales every track volume (VOL × master ÷ 128)."));
    form->addRow(tr("&Master volume (-V):"), m_volume);

    auto *reverbRow = new QHBoxLayout;
    m_reverbOn = new QCheckBox(tr("Override"), this);
    m_reverbOn->setChecked(cfg.reverb >= 0);
    m_reverb = new QSpinBox(this);
    m_reverb->setRange(0, 127);
    m_reverb->setValue(cfg.reverb >= 0 ? cfg.reverb : 50);
    m_reverb->setEnabled(cfg.reverb >= 0);
    connect(m_reverbOn, &QCheckBox::toggled, m_reverb, &QSpinBox::setEnabled);
    reverbRow->addWidget(m_reverbOn);
    reverbRow->addWidget(m_reverb, 1);
    auto *reverbHolder = new QWidget(this);
    reverbHolder->setLayout(reverbRow);
    reverbHolder->setToolTip(
        tr("mid2agb -R: song reverb level; unchecked leaves the player default."));
    form->addRow(tr("&Reverb (-R):"), reverbHolder);

    m_priority = new QSpinBox(this);
    m_priority->setRange(0, 127);
    m_priority->setValue(cfg.priority);
    m_priority->setToolTip(tr("mid2agb -P: player priority (fanfares interrupt music)."));
    form->addRow(tr("&Priority (-P):"), m_priority);

    m_exactGate = new QCheckBox(tr("Exact gate time (-E)"), this);
    m_exactGate->setChecked(cfg.exactGate);
    m_exactGate->setToolTip(
        tr("Keep note lengths exact instead of snapping through mid2agb's duration table."));

    m_extendedClocks = new QCheckBox(tr("48 clocks per beat (-X)"), this);
    m_extendedClocks->setChecked(cfg.extendedClocks);
    m_extendedClocks->setToolTip(tr("Doubles timing resolution (default is 24 clocks/beat)."));

    m_noCompression = new QCheckBox(tr("Disable compression (-N)"), this);
    m_noCompression->setChecked(cfg.noCompression);
    m_noCompression->setToolTip(tr("Skip mid2agb's repeated-pattern compression."));

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_exactGate);
    layout->addWidget(m_extendedClocks);
    layout->addWidget(m_noCompression);

    auto *note = new QLabel(
        tr("Saved to this song's mid2agb flags (midi.cfg or songs.mk)."), this);
    note->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(note);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

SongCfg SongSettingsDialog::cfg() const
{
    SongCfg cfg = m_original; // keeps rawFlags and unknown options
    cfg.voicegroupArg = m_voicegroup->currentText().trimmed();
    cfg.masterVolume = m_volume->value();
    cfg.reverb = m_reverbOn->isChecked() ? m_reverb->value() : -1;
    cfg.priority = m_priority->value();
    cfg.exactGate = m_exactGate->isChecked();
    cfg.extendedClocks = m_extendedClocks->isChecked();
    cfg.noCompression = m_noCompression->isChecked();
    return cfg;
}
