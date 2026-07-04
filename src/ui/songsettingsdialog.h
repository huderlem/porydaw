#pragma once

#include <QDialog>

#include "project/decompproject.h"

class QCheckBox;
class QLineEdit;
class QSpinBox;

// Song Settings (SPEC.md §6.1): the song's mid2agb flags from its midi.cfg
// line, presented as friendly controls. Committed via SongDocument::setCfg so
// the change is undoable; written back only to the song's midi.cfg line.
class SongSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SongSettingsDialog(const SongCfg &cfg, const QString &songLabel,
                       QWidget *parent = nullptr);

    // The edited settings; rawFlags are carried over untouched (save
    // reconciles them with these values).
    SongCfg cfg() const;

private:
    SongCfg m_original;
    QLineEdit *m_voicegroup;
    QSpinBox *m_volume;
    QCheckBox *m_reverbOn;
    QSpinBox *m_reverb;
    QSpinBox *m_priority;
    QCheckBox *m_exactGate;
    QCheckBox *m_extendedClocks;
    QCheckBox *m_noCompression;
};
