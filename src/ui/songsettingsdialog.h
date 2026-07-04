#pragma once

#include <QDialog>

#include "project/decompproject.h"

class QCheckBox;
class QComboBox;
class QSpinBox;

// Song Settings (SPEC.md §6.1): the song's mid2agb flags from its midi.cfg
// line, presented as friendly controls. Committed via SongDocument::setCfg so
// the change is undoable; written back only to the song's midi.cfg line.
class SongSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    // voicegroupArgs: the project's -G choices (SongRegistry::voicegroupArgs);
    // the combo stays editable so unknown/new symbols still work.
    SongSettingsDialog(const SongCfg &cfg, const QString &songLabel,
                       const QStringList &voicegroupArgs, QWidget *parent = nullptr);

    // The edited settings; rawFlags are carried over untouched (save
    // reconciles them with these values).
    SongCfg cfg() const;

private:
    SongCfg m_original;
    QComboBox *m_voicegroup;
    QSpinBox *m_volume;
    QCheckBox *m_reverbOn;
    QSpinBox *m_reverb;
    QSpinBox *m_priority;
    QCheckBox *m_exactGate;
    QCheckBox *m_extendedClocks;
    QCheckBox *m_noCompression;
};
