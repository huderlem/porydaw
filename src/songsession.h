#pragma once

#include <QDateTime>
#include <QString>
#include <QTemporaryDir>

class QAbstractButton;

#include <map>
#include <memory>

#include "core/miditimeline.h"
#include "core/songdocument.h"
#include "project/songbundle.h"
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

// What a draft session (docs/draft-songs/PLAN.md) still has to write: a
// song made by Import MIDI or New Song lives only in memory until its first
// save commits it — the .mid and its flags, then the registration.
struct SongDraft {
    QString constant; // registration: songs.h constant
    QString player;   // registration: song-table music player
    // The wizard's new voicegroup, created at commit. Always empty for now:
    // a song asking for one still takes the write-through path.
    QString newVoicegroup;
};

struct SongSession {
    // Where this tab's song lives: every per-session read (voicegroup load
    // and source, preview files, view sidecar, synth and sample lookups)
    // resolves under this, not under MainWindow's project — set from the
    // project root at creation, or the extraction dir for a bundle tab.
    QString root;
    // A song-bundle tab (docs/song-bundle/PLAN.md §3.6): a read-only song
    // opened from a .porysong file or bundle folder. Its document is locked,
    // it belongs to no project (songId stays -1, it survives project
    // switches, it is never persisted), and root is bundleDir's path — or
    // the folder itself for a folder bundle, where bundleDir is null.
    bool bundle = false;
    QString bundlePath; // canonical path of the opened file/folder
    std::unique_ptr<QTemporaryDir> bundleDir;
    BundleManifest manifest;
    QAbstractButton *bundleImportButton = nullptr; // in the view's banner
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
    int songId = -1;          // stays -1 while a draft
    // Set while the session is a draft: its document's midPath is where the
    // song WILL be saved, and nothing of it is in the project yet. Cleared
    // by the commit (MainWindow::commitDraft).
    std::unique_ptr<SongDraft> draft;
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
    // close prompts) must combine both. A draft is unsaved by definition,
    // edited or not: closing it loses the song.
    bool isDirty() const
    {
        return !bundle && (draft || doc.isDirty() || (vgSource && vgSource->dirty()));
    }
    bool isDraft() const { return draft != nullptr; }

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
