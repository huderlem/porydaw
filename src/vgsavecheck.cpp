#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <cstdio>

#include "mainwindow.h"
#include "project/songregistry.h"
#include "ui/voicegroupbrowser.h"

extern "C" {
#include "voicegroup_loader.h"
}

// --vgsavecheck <projectRoot> <song>: unified song+voicegroup undo/save
// check. Song and voicegroup edits share one undo stack and one save, so
// this drives MainWindow itself: a voice edit dirties the song, undo
// restores the byte-exact on-disk state, Save writes the .mid and the
// voicegroup .inc together, and an undone edit saved again round-trips the
// .inc byte-identically. Also proves a -G voicegroup switch keeps unsaved
// voice edits in the undo history (undoing the switch replays them).
// QSettings is redirected into a temp dir. Writes into the project: run
// against a scratch copy.

namespace {

QByteArray readFileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

} // namespace

bool MainWindow::runVgSaveCheck(const QString &projectRoot, const QString &songLabel)
{
    m_persistSession = false;
    if (!m_audioOk) {
        std::fprintf(stderr, "vgsavecheck: no audio device available\n");
        return false;
    }
    QString error;
    if (!m_project.open(projectRoot, &error)) {
        std::fprintf(stderr, "vgsavecheck: %s\n", qUtf8Printable(error));
        return false;
    }
    const SongInfo *target = nullptr;
    for (const SongInfo &song : m_project.songs()) {
        if (song.label == songLabel && song.isPlayable())
            target = &song;
    }
    if (!target) {
        std::fprintf(stderr, "vgsavecheck: song '%s' not found or has no MIDI source\n",
                     qUtf8Printable(songLabel));
        return false;
    }
    loadSong(*target);
    if (!m_audio.songLoaded() || m_loadedSongId < 0) {
        std::fprintf(stderr, "vgsavecheck: song failed to load\n");
        return false;
    }
    if (!m_vgSource) {
        std::fprintf(stderr, "vgsavecheck: no editable voicegroup source\n");
        return false;
    }

    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "vgsavecheck: FAIL: %s\n", what);
            failures++;
        }
        return ok;
    };

    // A DirectSound-family voice to edit (scalar fields + a sample symbol).
    int dsSlot = -1;
    for (int i = 0; i < VOICEGROUP_SIZE && dsSlot < 0; i++) {
        const VgVoice *v = m_vgSource->voiceAt(i);
        if (v
            && (v->macro == VgMacro::DirectSound
                || v->macro == VgMacro::DirectSoundNoResample
                || v->macro == VgMacro::DirectSoundAlt))
            dsSlot = i;
    }
    if (dsSlot < 0) {
        std::fprintf(stderr, "vgsavecheck: voicegroup has no sample voices\n");
        return false;
    }

    const QString vgPath = m_vgSource->filePath();
    const QString vgLoadName = m_vgSource->loadName();
    const QByteArray vgBytesOriginal = readFileBytes(vgPath);
    const QByteArray midBytesOriginal = readFileBytes(m_doc.midPath());
    const VgVoice original = *m_vgSource->voiceAt(dsSlot);

    // 1. A voice edit dirties the (one, unified) session.
    VgVoice edited = original;
    edited.release = original.release == 25 ? 26 : 25;
    onVoiceEditRequested(dsSlot, edited, false);
    check(m_doc.isDirty(), "voice edit did not dirty the song's undo stack");
    check(m_vgSource->dirty(), "voice edit did not dirty the source");
    check(isWindowModified(), "voice edit did not mark the window modified");
    check(m_audio.voicegroup()->voices[dsSlot].release == uint8_t(edited.release),
          "voice edit did not reach the audio engine");

    // 2. Undo restores the byte-exact on-disk state, nothing written.
    m_doc.undoStack()->undo();
    check(!m_doc.isDirty() && !m_vgSource->dirty(),
          "undo did not return the session to clean");
    check(!isWindowModified(), "undo left the window marked modified");
    check(readFileBytes(vgPath) == vgBytesOriginal,
          "voicegroup file changed without a save");

    // 3. Redo the voice edit, add a note edit, save once: both files written.
    m_doc.undoStack()->redo();
    int track = -1;
    for (int t = 0; t < m_doc.engineTrackCount() && track < 0; t++) {
        if (!m_doc.notesForTrack(t).empty())
            track = t;
    }
    if (track < 0) {
        std::fprintf(stderr, "vgsavecheck: song has no notes\n");
        return false;
    }
    uint64_t base = 0;
    for (const SmfTrack &tr : m_doc.smf().tracks)
        base = std::max(base, tr.endTick);
    base += 96;
    m_doc.addNote(track, base, 72, 24, 93);
    check(saveLoadedSong(), "unified save failed");
    check(!m_doc.isDirty() && !m_vgSource->dirty(), "still dirty after save");
    check(readFileBytes(vgPath) != vgBytesOriginal,
          "save did not write the voicegroup file");
    check(readFileBytes(m_doc.midPath()) != midBytesOriginal,
          "save did not write the .mid");
    {
        VoicegroupSource fresh;
        check(fresh.open(projectRoot, m_doc.cfg().voicegroupArg, &error)
                  && fresh.voiceAt(dsSlot)
                  && fresh.voiceAt(dsSlot)->release == edited.release,
              "saved voice edit not present in a fresh parse");
    }

    // 4. Undo both edits and save again: the voicegroup .inc must come back
    // byte-identical to the original (byte-conservative round trip).
    m_doc.undoStack()->undo(); // the note
    m_doc.undoStack()->undo(); // the voice edit
    check(m_doc.isDirty() && m_vgSource->dirty(),
          "undo past the save point did not re-dirty the session");
    check(saveLoadedSong(), "second unified save failed");
    check(readFileBytes(vgPath) == vgBytesOriginal,
          "undone voice edit did not round-trip the .inc byte-identically");

    // 5. A -G voicegroup switch carries unsaved voice edits in the undo
    // history: undoing the switch replays them into the reopened source.
    QString otherArg;
    for (const QString &arg : SongRegistry::voicegroupArgs(m_project.root())) {
        if (arg == m_doc.cfg().voicegroupArg)
            continue;
        SongCfg probe = m_doc.cfg();
        probe.voicegroupArg = arg;
        QString tried;
        if (LoadedVoiceGroup *vg = loadVoicegroupFor(probe, &tried)) {
            voicegroup_free(vg);
            otherArg = arg;
            break;
        }
    }
    if (otherArg.isEmpty()) {
        std::printf("vgsavecheck: note: no second voicegroup found, "
                    "-G switch replay skipped\n");
    } else {
        VgVoice edited2 = original;
        edited2.release = original.release == 25 ? 26 : 25;
        onVoiceEditRequested(dsSlot, edited2, false);
        SongCfg cfg = m_doc.cfg();
        cfg.voicegroupArg = otherArg;
        m_doc.setCfg(cfg);
        check(m_vgSource && m_vgSource->loadName() != vgLoadName,
              "-G switch did not swap the voicegroup source");
        m_doc.undoStack()->undo(); // the -G switch
        check(m_vgSource && m_vgSource->loadName() == vgLoadName,
              "undoing the -G switch did not reopen the old voicegroup");
        check(m_vgSource && m_vgSource->voiceAt(dsSlot)
                  && m_vgSource->voiceAt(dsSlot)->release == edited2.release
                  && m_vgSource->dirty(),
              "undoing the -G switch did not replay the unsaved voice edit");
        m_doc.undoStack()->undo(); // the voice edit
        check(m_vgSource && !m_vgSource->dirty() && !m_doc.isDirty(),
              "undoing the replayed voice edit did not return to clean");
        check(readFileBytes(vgPath) == vgBytesOriginal,
              "voicegroup file changed during the -G switch round trip");
    }

    // 6. The dock's editor widgets feed the same pipeline: spinning the
    // Release box must push an undo command, and undoing it must refresh
    // the box back to the original value.
    {
        m_vgBrowser->selectSlot(dsSlot);
        QSpinBox *releaseSpin = nullptr;
        for (QSpinBox *spin : m_vgBrowser->findChildren<QSpinBox *>()) {
            if (spin->toolTip() == tr("Release"))
                releaseSpin = spin;
        }
        if (check(releaseSpin != nullptr, "no Release spin box in the editor")) {
            const int uiValue = original.release == 25 ? 26 : 25;
            releaseSpin->setValue(uiValue);
            check(m_doc.isDirty() && m_vgSource->dirty()
                      && m_vgSource->voiceAt(dsSlot)->release == uiValue,
                  "editing the Release spin box did not push an undo command");
            m_doc.undoStack()->undo();
            check(!m_doc.isDirty() && !m_vgSource->dirty()
                      && releaseSpin->value() == original.release,
                  "undo did not refresh the Release spin box");
        }
    }

    std::printf("vgsavecheck: %s (%d failures)\n", failures ? "FAIL" : "PASS",
                failures);
    return failures == 0;
}

int runVgSaveCheck(const QString &projectRoot, const QString &songLabel)
{
    // Redirected settings: the user's real session is never touched.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "vgsavecheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());

    MainWindow window;
    return window.runVgSaveCheck(projectRoot, songLabel) ? 0 : 1;
}
