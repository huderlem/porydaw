#include "voicegroupbrowser.h"

#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>

#include "ui/m4asemantics.h"

namespace {
constexpr int kAuditionKey = 60; // middle C; drumkits play that key's percussion
constexpr int kAuditionVelocity = 112;
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
}

void VoicegroupBrowser::setVoicegroup(const LoadedVoiceGroup *vg, const QString &title)
{
    releaseVoice();
    m_tree->clear();
    if (!vg) {
        m_title->setText(tr("No song loaded"));
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
    return QWidget::eventFilter(watched, event);
}
