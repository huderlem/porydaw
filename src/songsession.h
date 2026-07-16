#pragma once

#include <QDateTime>
#include <QString>

#include <map>
#include <memory>

#include "core/miditimeline.h"
#include "core/songdocument.h"
#include "project/voicegroupsource.h"
#include "ui/songview.h"

extern "C" {
#include "voicegroup_loader.h"
}

// One open song tab. Each tab is a complete, independent editing session:
// its own document (with its own undo stack — voicegroup edits ride it too),
// its own parse of the voicegroup source, and its own view. The session owns
// the built timeline and the loaded voicegroup; the audio engine only
// borrows the active tab's, so switching tabs never invalidates what an
// inactive tab's view is drawing.
//
// Two tabs sharing a -G voicegroup are deliberately independent copies:
// unsaved voice edits stay inside their tab, and a clean tab whose .inc was
// re-saved from another tab reloads it on activation (vgFileTime below).
// A session-owned Golden Sun synth descriptor: a zero-size WaveData whose
// bytes porydaw fills itself, for synth voices whose definition isn't on
// disk (pending param edits persist only on save) or whose loader-owned
// WaveData is shared and must not be mutated. ToneData.wav points here;
// bytes are patched in place so live tweaks are heard without a reload.
struct SynthToneBuf {
    WaveData wd;
    uint8_t bytes[17];
};

struct SongSession {
    SongDocument doc;
    std::unique_ptr<VoicegroupSource> vgSource;
    std::unique_ptr<MidiTimeline> timeline;
    LoadedVoiceGroup *voicegroup = nullptr;
    // Keyed by slot; entries outlive any one LoadedVoiceGroup (engine track
    // caches hold ToneData copies pointing here) and are re-installed into a
    // freshly loaded voicegroup by MainWindow::applyPendingSynthTones.
    // std::map: Qt 6.2's QHash can't hold move-only values.
    std::map<int, std::unique_ptr<SynthToneBuf>> synthTones;
    SongView *view = nullptr; // tab page; deleted here, before the tab widget
    int songId = -1;
    // Engine-applied cfg values, to react only to real changes on edits.
    QString appliedVoicegroupArg;
    int appliedVolume = 127;
    int appliedReverb = -1;
    // On-disk mtime of the voicegroup source at open/save time; a clean tab
    // whose file changed underneath (saved from another tab) reloads it when
    // the tab is activated.
    QDateTime vgFileTime;

    // The tab's unsaved-changes state: song and voicegroup edits are one
    // document to the user, so every dirty check (tab title, window title,
    // close prompts) must combine both.
    bool isDirty() const { return doc.isDirty() || (vgSource && vgSource->dirty()); }

    ~SongSession()
    {
        if (view) {
            // The view draws from timeline/voicegroup; detach before they go.
            view->setDocument(nullptr);
            view->setSong(nullptr, nullptr);
            delete view;
        }
        if (voicegroup)
            voicegroup_free(voicegroup);
    }
};
