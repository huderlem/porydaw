#pragma once

#include <QTreeWidget>
#include <QWidget>

extern "C" {
#include "voicegroup_loader.h"
}

class QLabel;

// The voicegroup browser (SPEC.md §6.1): a read-only list of the current
// song's 128 voicegroup entries — type, name, ADSR — with press-and-hold
// audition through the audio engine's second (preview) instance.
class VoicegroupBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit VoicegroupBrowser(QWidget *parent = nullptr);

    // vg may be nullptr (no song loaded). Not owned; the caller must clear it
    // (setVoicegroup(nullptr)) before the voicegroup is freed.
    void setVoicegroup(const LoadedVoiceGroup *vg, const QString &title = QString());

signals:
    // velocity 0 releases. Routed to AudioEngine::previewVoice.
    void auditionVoice(int voice, int key, int velocity);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void pressedVoice(QTreeWidgetItem *item);
    void releaseVoice();

    QLabel *m_title = nullptr;
    QTreeWidget *m_tree = nullptr;
    int m_soundingVoice = -1;
};
