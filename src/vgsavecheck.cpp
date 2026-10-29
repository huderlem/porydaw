#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>
#include <cstdio>

#include "ui/keymap.h"
#include "ui/songview.h"
#include "ui/theme/themeruntime.h"

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

bool MainWindow::runVgSaveCheck(const QString &projectRoot, const QString &songLabel,
                                const QString &screenshotPath)
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
    SongSession *tab = activeSession();
    if (!m_audio.songLoaded() || !tab || tab->songId < 0) {
        std::fprintf(stderr, "vgsavecheck: song failed to load\n");
        return false;
    }
    if (!tab->vgSource) {
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

    // Per-session paths resolve under the tab's own root; a project tab's
    // is the project's, and the voicegroup source was opened beneath it.
    check(tab->root == m_project.root(), "session root is the project root");
    check(tab->vgSource->filePath().startsWith(tab->root + QLatin1Char('/')),
          "voicegroup source opened under the session root");

    // A DirectSound-family voice to edit (scalar fields + a sample symbol).
    int dsSlot = -1;
    for (int i = 0; i < VOICEGROUP_SIZE && dsSlot < 0; i++) {
        const VgVoice *v = tab->vgSource->voiceAt(i);
        if (v && vgMacroIsDirectSound(v->macro))
            dsSlot = i;
    }
    if (dsSlot < 0) {
        std::fprintf(stderr, "vgsavecheck: voicegroup has no sample voices\n");
        return false;
    }

    const QString vgPath = tab->vgSource->filePath();
    const QString vgLoadName = tab->vgSource->loadName();
    const QByteArray vgBytesOriginal = readFileBytes(vgPath);
    const QByteArray midBytesOriginal = readFileBytes(tab->doc.midPath());
    const VgVoice original = *tab->vgSource->voiceAt(dsSlot);

    // 1. A voice edit dirties the (one, unified) session.
    VgVoice edited = original;
    edited.release = original.release == 25 ? 26 : 25;
    onVoiceEditRequested(dsSlot, edited, false);
    check(tab->doc.isDirty(), "voice edit did not dirty the song's undo stack");
    check(tab->vgSource->dirty(), "voice edit did not dirty the source");
    check(isWindowModified(), "voice edit did not mark the window modified");
    check(m_audio.voicegroup()->voices[dsSlot].release == uint8_t(edited.release),
          "voice edit did not reach the audio engine");

    // 2. Undo restores the byte-exact on-disk state, nothing written.
    tab->doc.undoStack()->undo();
    check(!tab->doc.isDirty() && !tab->vgSource->dirty(),
          "undo did not return the session to clean");
    check(!isWindowModified(), "undo left the window marked modified");
    check(readFileBytes(vgPath) == vgBytesOriginal, "voicegroup file changed without a save");

    // 3. Redo the voice edit, add a note edit, save once: both files written.
    tab->doc.undoStack()->redo();
    int track = -1;
    for (int t = 0; t < tab->doc.engineTrackCount() && track < 0; t++) {
        if (!tab->doc.notesForTrack(t).empty())
            track = t;
    }
    if (track < 0) {
        std::fprintf(stderr, "vgsavecheck: song has no notes\n");
        return false;
    }
    uint64_t base = 0;
    for (const SmfTrack &tr : tab->doc.smf().tracks)
        base = std::max(base, tr.endTick);
    base += 96;
    tab->doc.addNote(track, base, 72, 24, 93);
    check(saveSession(*tab), "unified save failed");
    check(!tab->doc.isDirty() && !tab->vgSource->dirty(), "still dirty after save");
    check(readFileBytes(vgPath) != vgBytesOriginal, "save did not write the voicegroup file");
    check(readFileBytes(tab->doc.midPath()) != midBytesOriginal, "save did not write the .mid");
    {
        VoicegroupSource fresh;
        check(fresh.open(projectRoot, tab->doc.cfg().voicegroupArg, &error) &&
                  fresh.voiceAt(dsSlot) && fresh.voiceAt(dsSlot)->release == edited.release,
              "saved voice edit not present in a fresh parse");
    }

    // 4. Undo both edits and save again: the voicegroup .inc must come back
    // byte-identical to the original (byte-conservative round trip).
    tab->doc.undoStack()->undo(); // the note
    tab->doc.undoStack()->undo(); // the voice edit
    check(tab->doc.isDirty() && tab->vgSource->dirty(),
          "undo past the save point did not re-dirty the session");
    check(saveSession(*tab), "second unified save failed");
    check(readFileBytes(vgPath) == vgBytesOriginal,
          "undone voice edit did not round-trip the .inc byte-identically");

    // 5. A -G voicegroup switch carries unsaved voice edits in the undo
    // history: undoing the switch replays them into the reopened source.
    QString otherArg;
    for (const QString &arg : SongRegistry::voicegroupArgs(m_project.root())) {
        if (arg == tab->doc.cfg().voicegroupArg)
            continue;
        SongCfg probe = tab->doc.cfg();
        probe.voicegroupArg = arg;
        QString tried;
        if (LoadedVoiceGroup *vg = loadVoicegroupFor(m_project.root(), probe, &tried)) {
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
        SongCfg cfg = tab->doc.cfg();
        cfg.voicegroupArg = otherArg;
        tab->doc.setCfg(cfg);
        check(tab->vgSource && tab->vgSource->loadName() != vgLoadName,
              "-G switch did not swap the voicegroup source");
        tab->doc.undoStack()->undo(); // the -G switch
        check(tab->vgSource && tab->vgSource->loadName() == vgLoadName,
              "undoing the -G switch did not reopen the old voicegroup");
        check(tab->vgSource && tab->vgSource->voiceAt(dsSlot) &&
                  tab->vgSource->voiceAt(dsSlot)->release == edited2.release &&
                  tab->vgSource->dirty(),
              "undoing the -G switch did not replay the unsaved voice edit");
        tab->doc.undoStack()->undo(); // the voice edit
        check(tab->vgSource && !tab->vgSource->dirty() && !tab->doc.isDirty(),
              "undoing the replayed voice edit did not return to clean");
        check(readFileBytes(vgPath) == vgBytesOriginal,
              "voicegroup file changed during the -G switch round trip");

        // 5b. The dock's voicegroup selector drives the same switch as an
        // undoable cfg edit, and undo refreshes the selector's text. The
        // selector shows args in display form: the leading underscore folds
        // into the fixed "voicegroup_" prefix.
        QComboBox *vgCombo = m_vgBrowser->findChild<QComboBox *>(QStringLiteral("vgArgCombo"));
        if (check(vgCombo != nullptr, "no voicegroup selector in the dock")) {
            const QString originalArg = tab->doc.cfg().voicegroupArg;
            const QString shown = SongRegistry::voicegroupDisplayName(
                originalArg.isEmpty() ? QStringLiteral("_dummy") : originalArg);
            check(vgCombo->isEnabled() && vgCombo->currentText() == shown,
                  "dock selector does not show the song's voicegroup");
            check(vgCombo->findText(SongRegistry::voicegroupDisplayName(otherArg)) >= 0,
                  "dock selector is missing a known voicegroup arg");
            vgCombo->setCurrentText(SongRegistry::voicegroupDisplayName(otherArg));
            QMetaObject::invokeMethod(vgCombo, "activated", Qt::DirectConnection, Q_ARG(int, 0));
            check(tab->doc.cfg().voicegroupArg == otherArg && tab->doc.isDirty(),
                  "dock selector did not commit an undoable -G switch");
            check(tab->vgSource && tab->vgSource->loadName() != vgLoadName,
                  "dock selector switch did not swap the voicegroup source");
            tab->doc.undoStack()->undo(); // the selector's -G switch
            check(tab->doc.cfg().voicegroupArg == originalArg && !tab->doc.isDirty(),
                  "undoing the dock selector switch did not restore the cfg");
            check(vgCombo->currentText() == shown, "undo did not refresh the dock selector's text");
            check(tab->vgSource && tab->vgSource->loadName() == vgLoadName,
                  "undoing the dock selector switch did not reopen the old "
                  "voicegroup");
        }
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
            check(tab->doc.isDirty() && tab->vgSource->dirty() &&
                      tab->vgSource->voiceAt(dsSlot)->release == uiValue,
                  "editing the Release spin box did not push an undo command");
            tab->doc.undoStack()->undo();
            check(!tab->doc.isDirty() && !tab->vgSource->dirty() &&
                      releaseSpin->value() == original.release,
                  "undo did not refresh the Release spin box");
        }
    }

    // 6b. Selecting voices of different families must not change the dock's
    // minimum width: the editor adapts to the dock width the user set, never
    // pushes the dock open (a DS voice's three-digit ADSR spins used to
    // widen it relative to a CGB voice's one-digit ones).
    {
        int cgbSlot = -1;
        for (int i = 0; i < VOICEGROUP_SIZE && cgbSlot < 0; i++) {
            const VgVoice *v = tab->vgSource->voiceAt(i);
            if (v && vgMacroIsCgb(v->macro))
                cgbSlot = i;
        }
        if (cgbSlot >= 0) {
            // The window must be shown for the measurement: hidden widgets
            // are empty layout items and contribute no minimum.
            show();
            QCoreApplication::processEvents();
            m_vgBrowser->selectSlot(cgbSlot);
            QCoreApplication::processEvents();
            const int cgbMin = m_vgBrowser->minimumSizeHint().width();
            m_vgBrowser->selectSlot(dsSlot);
            QCoreApplication::processEvents();
            check(m_vgBrowser->minimumSizeHint().width() == cgbMin,
                  "voice selection changed the voicegroup dock's minimum width");
        } else {
            std::printf("vgsavecheck: note: no CGB voice found, "
                        "dock-width check skipped\n");
        }
    }

    // 7. Golden Sun synth param edits: minted definitions live in memory
    // only (no disk write, not in the definition dropdown) until the
    // voicegroup saves — and the save writes exactly the definitions the
    // saved state references, never the abandoned intermediate tweaks.
    {
        // Give the scratch project synth support: the macros (gate) and one
        // definition, appended so a project that already has them is fine.
        const QString synthPath =
            projectRoot + QStringLiteral("/sound/direct_sound_synth_data.inc");
        QDir().mkpath(projectRoot + QStringLiteral("/asm/macros"));
        bool wrote = false;
        {
            QFile macros(projectRoot + QStringLiteral("/asm/macros/vgsavecheck_synth.inc"));
            wrote = macros.open(QIODevice::WriteOnly) &&
                    macros.write("\t.macro set_synth_pulse base_duty=0x80, "
                                 "duty_step=0x00, mod_depth=0x00, duty_phase=0x00\n"
                                 "\t.endm\n"
                                 "\t.macro set_synth_saw\n\t.endm\n"
                                 "\t.macro set_synth_triangle\n\t.endm\n") > 0;
        }
        {
            QFile data(synthPath);
            wrote = wrote && data.open(QIODevice::WriteOnly | QIODevice::Append) &&
                    data.write("\n\t.align 2\nVgSaveCheckSaw::\n\tset_synth_saw\n") > 0;
        }
        if (!check(wrote, "cannot write synth support files"))
            return false;
        invalidateVgCatalog();
        updateVoicegroupBrowser(); // hand the browser the new catalog
        const VgSynthCatalog setupCatalog = VoicegroupSource::synthInstruments(projectRoot);
        const int defsAfterSetup = setupCatalog.defs.size();
        const QByteArray synthBytesSetup = readFileBytes(synthPath);
        const int indexBeforeSynth = tab->doc.undoStack()->index();

        // A sample voice that is NOT already a synth (a synth-heavy
        // voicegroup's first DirectSound voice may be one, and switching it
        // to Synth would rightly be a no-op).
        int synthSlot = -1;
        for (int i = 0; i < VOICEGROUP_SIZE && synthSlot < 0; i++) {
            const VgVoice *v = tab->vgSource->voiceAt(i);
            if (v && vgMacroIsDirectSound(v->macro) && !vgMacroIsCompressed(v->macro) &&
                !setupCatalog.find(v->symbol))
                synthSlot = i;
        }
        if (synthSlot < 0) {
            std::printf("vgsavecheck: note: every sample voice is already a "
                        "synth, synth section skipped\n");
            std::printf("vgsavecheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
            return failures == 0;
        }
        const VgVoice synthOriginal = *tab->vgSource->voiceAt(synthSlot);

        m_vgBrowser->selectSlot(synthSlot);
        QComboBox *typeCombo = nullptr, *symbolCombo = nullptr, *waveCombo = nullptr;
        for (QComboBox *combo : m_vgBrowser->findChildren<QComboBox *>()) {
            for (int i = 0; i < combo->count(); i++) {
                if (combo->itemText(i) == tr("Synth (Golden Sun)"))
                    typeCombo = combo;
                if (combo->itemText(i) == QStringLiteral("Sawtooth"))
                    waveCombo = combo;
            }
            // The dock's voicegroup selector is editable too; the symbol
            // combo is the editable one inside the editor panel.
            if (combo->isEditable() && combo->objectName() != QLatin1String("vgArgCombo"))
                symbolCombo = combo;
        }
        QSpinBox *dutySpin = nullptr, *stepSpin = nullptr, *depthSpin = nullptr,
                 *phaseSpin = nullptr;
        for (QSpinBox *spin : m_vgBrowser->findChildren<QSpinBox *>()) {
            if (spin->toolTip().startsWith(QStringLiteral("Base duty")))
                dutySpin = spin;
            else if (spin->toolTip().startsWith(QStringLiteral("Duty LFO step")))
                stepSpin = spin;
            else if (spin->toolTip().startsWith(QStringLiteral("Modulation")))
                depthSpin = spin;
            else if (spin->toolTip().startsWith(QStringLiteral("Duty LFO phase")))
                phaseSpin = spin;
        }
        if (check(typeCombo && symbolCombo && waveCombo && dutySpin && stepSpin && depthSpin &&
                      phaseSpin,
                  "synth editor widgets not found")) {
            const auto activate = [](QComboBox *combo, int index) {
                combo->setCurrentIndex(index);
                QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection,
                                          Q_ARG(int, index));
            };
            int synthIndex = -1;
            for (int i = 0; i < typeCombo->count(); i++) {
                if (typeCombo->itemText(i) == tr("Synth (Golden Sun)"))
                    synthIndex = i;
            }
            activate(typeCombo, synthIndex);
            check(tab->vgSource->voiceAt(synthSlot)->symbol != synthOriginal.symbol,
                  "switching the voice to Synth did not take");
            // Dial a known pulse through several commits (each one mints).
            activate(waveCombo, 0);
            dutySpin->setValue(0x21);
            stepSpin->setValue(0x43);
            depthSpin->setValue(0x65);
            phaseSpin->setValue(0x87);
            const QString wantSymbol = vgSynthSymbolName(VgSynthDesc{0, 0x21, 0x43, 0x65, 0x87});
            check(tab->vgSource->voiceAt(synthSlot)->symbol == wantSymbol,
                  "param edits did not land on the param-named symbol");
            check(readFileBytes(synthPath) == synthBytesSetup,
                  "a param edit wrote to the synth data file before save");
            bool listed = false;
            for (int i = 0; i < symbolCombo->count(); i++)
                listed = listed || symbolCombo->itemText(i) == wantSymbol;
            check(!listed, "an unsaved definition appeared in the dropdown");
            const ToneData &td = m_audio.voicegroup()->voices[synthSlot];
            check(td.wav && td.wav->size == 0 && uint8_t(td.wav->data[1]) == 0 &&
                      uint8_t(td.wav->data[2]) == 0x21 && uint8_t(td.wav->data[5]) == 0x87,
                  "param edits were not patched into the loaded tone");
            // The scalar path renames the voice too (param-named symbols):
            // voiceNames feeds track labels and the browser tree.
            check(QString::fromUtf8(m_audio.voicegroup()->voiceNames[synthSlot]) == wantSymbol,
                  "param edits did not sync the loaded voice name");

            // Save: exactly one definition (the referenced one) is written,
            // it now shows up in the dropdown, and the synth data file is
            // wired into the build (sound_data.s .include).
            check(saveSession(*tab), "synth save failed");
            check(readFileBytes(synthPath).contains(wantSymbol.toUtf8() + "::"),
                  "save did not write the referenced synth definition");
            bool wired = false;
            for (const QString &dir : {projectRoot + QStringLiteral("/data"), projectRoot}) {
                QDirIterator wiredIt(dir, {QStringLiteral("*.s")}, QDir::Files);
                while (wiredIt.hasNext() && !wired)
                    wired = readFileBytes(wiredIt.next()).contains("direct_sound_synth_data.inc");
            }
            check(wired, "save did not wire the synth data file into the build");
            check(VoicegroupSource::synthInstruments(projectRoot).defs.size() == defsAfterSetup + 1,
                  "save wrote more than the one referenced definition");
            listed = false;
            for (int i = 0; i < symbolCombo->count(); i++)
                listed = listed || symbolCombo->itemText(i) == wantSymbol;
            check(listed, "the saved definition did not appear in the dropdown");

            // Waveform flips: to Saw the edit dedupes onto the setup's
            // on-disk definition; back to Pulse it must adopt the 50%-square
            // default, not commit the saw's zeroed (silent, duty-0) params.
            const int indexBeforeFlips = tab->doc.undoStack()->index();
            activate(waveCombo, 1);
            const QString sawSymbol = tab->vgSource->voiceAt(synthSlot)->symbol;
            // find() points into the catalog: it must outlive the pointer.
            const VgSynthCatalog sawCatalog = VoicegroupSource::synthInstruments(projectRoot);
            const VgSynthDesc *sawDef = sawCatalog.find(sawSymbol);
            check(sawDef && sawDef->waveform == 1,
                  "waveform flip to saw did not dedupe onto an on-disk saw");
            activate(waveCombo, 0);
            // Deduped onto an on-disk 50% pulse when the project has one,
            // minted under the param name otherwise — never the duty-0 name.
            const QString pulseSymbol = tab->vgSource->voiceAt(synthSlot)->symbol;
            const VgSynthCatalog pulseCatalog = VoicegroupSource::synthInstruments(projectRoot);
            const VgSynthDesc *pulseDef = pulseCatalog.find(pulseSymbol);
            check((pulseDef && *pulseDef == VgSynthDesc{}) ||
                      pulseSymbol == vgSynthSymbolName(VgSynthDesc{}),
                  "waveform flip back to pulse did not adopt the 50% default");
            while (tab->doc.undoStack()->index() > indexBeforeFlips)
                tab->doc.undoStack()->undo();
            check(tab->vgSource->voiceAt(synthSlot)->symbol == wantSymbol,
                  "undoing the waveform flips did not restore the voice");

            // Back to the original voice; the .inc round-trips, and no
            // further definitions are written.
            const QByteArray synthBytesSaved = readFileBytes(synthPath);
            while (tab->doc.undoStack()->index() > indexBeforeSynth)
                tab->doc.undoStack()->undo();
            check(saveSession(*tab), "post-undo save failed");
            check(readFileBytes(vgPath) == vgBytesOriginal,
                  "undone synth edits did not round-trip the .inc");
            check(readFileBytes(synthPath) == synthBytesSaved,
                  "the post-undo save wrote synth definitions");
        }
    }

    // Jump-from-context reveal and used-voice marks: rows for programs the
    // song references render with a highlighted background, revealTrackVoice
    // raises the dock and selects the track's current program, and document
    // edits that add or remove voice changes keep the marks in sync.
    {
        QTreeWidget *tree = m_vgBrowser->findChild<QTreeWidget *>();
        if (check(tree != nullptr, "voicegroup browser has no tree")) {
            // An unmarked row keeps the default (no-fill) brush, so a set
            // background brush is the mark.
            const auto marked = [](const QTreeWidgetItem *item) {
                return item->background(0).style() != Qt::NoBrush;
            };
            const QSet<int> used = tab->view->usedVoices();
            check(!used.isEmpty(), "song reports no used voices");
            int unused = -1;
            for (int i = 0; i < VOICEGROUP_SIZE && unused < 0; i++) {
                if (!used.contains(i))
                    unused = i;
            }
            bool marksOk = true;
            for (int p : used) {
                const QTreeWidgetItem *item = tree->topLevelItem(p);
                marksOk = marksOk && item && marked(item);
            }
            check(marksOk, "used voices are not highlighted in the dock");
            check(unused < 0 || !marked(tree->topLevelItem(unused)),
                  "an unused voice is highlighted");

            // Track-header path: the dock reappears and the track's current
            // program becomes the selected slot.
            m_vgDock->hide();
            const int prog = tab->view->currentProgram(track);
            check(prog >= 0, "track under test has no program");
            tab->view->revealTrackVoice(track);
            check(!m_vgDock->isHidden(), "reveal did not show the voicegroup dock");
            check(m_vgBrowser->currentSlot() == prog, "reveal did not select the track's program");

            if (unused >= 0) {
                // Explicit-program path (the event list's context menu).
                tab->view->revealVoice(unused);
                check(m_vgBrowser->currentSlot() == unused,
                      "revealVoice did not select the requested slot");

                // A new voice change gains the mark; undoing it clears it.
                tab->doc.addLanePoint(track, DOC_CC_VOICE, 480, unused);
                check(marked(tree->topLevelItem(unused)),
                      "a new voice change did not gain the used mark");
                tab->doc.undoStack()->undo();
                check(!marked(tree->topLevelItem(unused)),
                      "undoing the voice change did not clear the used mark");
            }
        }
    }

    // 9. A structural commit fired from inside a track header's own mouse
    // press must not free that header row mid-event: with a changed arg
    // typed into the voicegroup selector's line edit, clicking a header
    // focuses the roll (selectTrack), which fires editingFinished — an
    // undoable -G switch whose voicegroup swap rebuilds the header panel
    // while the clicked row's mousePressEvent is still on the stack. The
    // row has to survive its own press (deferred deletion), or this is a
    // use-after-free crash. (The sample symbol box that originally hit this
    // is now the picker, which commits from its popup instead of on focus
    // loss — the selector keeps the scenario alive.)
    if (otherArg.isEmpty()) {
        std::printf("vgsavecheck: note: no second voicegroup found, "
                    "mid-press structural rebuild skipped\n");
    } else {
        show();
        activateWindow();
        QCoreApplication::processEvents();
        const QString argBefore = tab->doc.cfg().voicegroupArg;
        QComboBox *vgCombo = m_vgBrowser->findChild<QComboBox *>(QStringLiteral("vgArgCombo"));
        int otherTrack = -1;
        const MidiTimeline *tl = tab->view->timeline();
        for (int t = 0; t < 16 && otherTrack < 0 && tl; t++) {
            if (t != tab->view->selectedTrack() && tl->tracks[t].used)
                otherTrack = t;
        }
        if (check(vgCombo && otherTrack >= 0,
                  "no vg selector or second track for the mid-press commit")) {
            QLineEdit *edit = vgCombo->lineEdit();
            edit->setFocus();
            QCoreApplication::processEvents();
            if (check(edit->hasFocus(), "vg selector did not take focus")) {
                edit->setText(otherArg);
                QPointer<QWidget> row = tab->view->findChild<QWidget *>(
                    QStringLiteral("trackHeaderRow%1").arg(otherTrack));
                if (check(row != nullptr, "no header row for the other track")) {
                    const QPoint pos(5, 5);
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                                      QPointF(row->mapToGlobal(pos)), Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(row, &press);
                    check(!row.isNull(), "header rebuild freed the row inside its own press");
                    check(tab->view->selectedTrack() == otherTrack,
                          "the header click did not select its track");
                    check(tab->doc.cfg().voicegroupArg == otherArg,
                          "the mid-press -G edit did not commit");
                    if (!row.isNull()) {
                        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                                            QPointF(row->mapToGlobal(pos)), Qt::LeftButton,
                                            Qt::NoButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(row, &release);
                    }
                }
                QCoreApplication::processEvents(); // deferred row deletion
                // The rebuilt panel is functional: its fresh rows select.
                QWidget *fresh =
                    tab->view->findChild<QWidget *>(QStringLiteral("trackHeaderRow%1").arg(track));
                if (check(fresh != nullptr, "no rebuilt header row") && track != otherTrack) {
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(QPoint(5, 5)),
                                      QPointF(fresh->mapToGlobal(QPoint(5, 5))), Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(fresh, &press);
                    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(QPoint(5, 5)),
                                        QPointF(fresh->mapToGlobal(QPoint(5, 5))), Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(fresh, &release);
                    QCoreApplication::processEvents();
                    check(tab->view->selectedTrack() == track,
                          "a rebuilt header row did not select its track");
                }
                tab->doc.undoStack()->undo(); // the mid-press -G switch
                check(tab->doc.cfg().voicegroupArg == argBefore && tab->vgSource &&
                          tab->vgSource->loadName() == vgLoadName,
                      "undo did not restore the mid-press -G switch");
            }
        }
    }

    // 10. The sample picker (the Sample field for DirectSound voices):
    // replaces the giant combo, filters by typed text, auditions the
    // highlighted sample, marks looped samples, commits an undoable symbol
    // change, and still accepts an unlisted typed symbol.
    {
        m_vgBrowser->revealSlot(dsSlot);
        QCoreApplication::processEvents();
        auto *picker =
            m_vgBrowser->findChild<SamplePickerButton *>(QStringLiteral("vgSamplePickerButton"));
        QComboBox *symbolCombo = nullptr;
        for (QComboBox *combo : m_vgBrowser->findChildren<QComboBox *>()) {
            if (combo->isEditable() && combo->objectName() != QLatin1String("vgArgCombo"))
                symbolCombo = combo;
        }
        if (check(picker && symbolCombo, "no sample picker in the editor")) {
            check(picker->isVisible() && !symbolCombo->isVisible(),
                  "sample voice does not show the picker instead of the combo");
            const VgVoice before = *tab->vgSource->voiceAt(dsSlot);
            check(picker->currentSymbol() == before.symbol,
                  "picker does not show the voice's symbol");

            QStringList auditioned;
            QList<VgAuditionKind> auditionKinds;
            int stops = 0;
            QMetaObject::Connection c1 =
                connect(m_vgBrowser, &VoicegroupBrowser::sampleAuditionRequested, this,
                        [&auditioned, &auditionKinds](const QString &s, VgAuditionKind kind,
                                                      const AuditionSlots::Adsr &) {
                            auditioned.append(s);
                            auditionKinds.append(kind);
                        });
            QMetaObject::Connection c2 =
                connect(m_vgBrowser, &VoicegroupBrowser::sampleAuditionStopRequested, this,
                        [&stops] { stops++; });

            picker->openPopup();
            auto *search = picker->findChild<QLineEdit *>(QStringLiteral("vgSamplePickerSearch"));
            auto *list = picker->findChild<QTreeWidget *>(QStringLiteral("vgSamplePickerList"));
            if (check(search && list && picker->popupVisible(), "picker popup did not open")) {
                // The popup floats over the browser, so it carries the menu
                // outline; its corner pixel is that border.
                QWidget *popupFrame =
                    picker->findChild<QWidget *>(QStringLiteral("vgSamplePickerPopup"));
                check(popupFrame && popupFrame->grab().toImage().pixelColor(0, 0) ==
                                        themes::color(themes::Role::menu_outline),
                      "the picker popup is missing the menu outline");
                // Every catalog sample is listed, and the committed sample
                // data drives at least one loop badge (vanilla projects have
                // plenty of looped instruments).
                int rows = 0, badges = 0;
                for (QTreeWidgetItemIterator it(list); *it; ++it) {
                    if ((*it)->data(0, Qt::UserRole).toString().isEmpty())
                        continue; // section label
                    rows++;
                    badges += (*it)->text(1).isEmpty() ? 0 : 1;
                }
                check(rows >= 2, "picker lists fewer than two symbols");
                check(badges > 0, "no loop badges on any sample row");

                // Keysplit rows audition too — the resolved sub-voice plays
                // (MainWindow::auditionKeysplit), so browsing them is
                // audible like everything else.
                QTreeWidgetItem *ksRow = nullptr;
                for (QTreeWidgetItemIterator it(list); *it && !ksRow; ++it) {
                    if ((*it)->data(0, Qt::UserRole + 1).toBool())
                        ksRow = *it;
                }
                if (ksRow) {
                    const int seen = auditioned.size();
                    list->setCurrentItem(ksRow);
                    check(auditioned.size() == seen + 1 &&
                              auditionKinds.last() == VgAuditionKind::Keysplit,
                          "keysplit row did not audition as a keysplit");
                } else {
                    std::printf("vgsavecheck: note: no keysplit instruments, "
                                "keysplit audition skipped\n");
                }

                if (!screenshotPath.isEmpty()) {
                    QWidget *popup =
                        picker->findChild<QWidget *>(QStringLiteral("vgSamplePickerPopup"));
                    check(popup && popup->grab().save(screenshotPath),
                          "could not save the picker screenshot");
                    // A -dock variant of the browser itself: the editor
                    // panel's Sample row (picker + glyph tool buttons).
                    const QFileInfo info(screenshotPath);
                    check(m_vgBrowser->grab().save(info.path() + QLatin1Char('/') +
                                                   info.completeBaseName() +
                                                   QStringLiteral("-dock.") + info.suffix()),
                          "could not save the dock screenshot");
                }

                // Pick a different plain sample by typing its full symbol:
                // the filter narrows onto it, highlighting auditions it, and
                // Return commits it as an undoable voice edit.
                QString target;
                for (QTreeWidgetItemIterator it(list); *it && target.isEmpty(); ++it) {
                    const QString s = (*it)->data(0, Qt::UserRole).toString();
                    if (!s.isEmpty() && s != before.symbol &&
                        !(*it)->data(0, Qt::UserRole + 1).toBool())
                        target = s;
                }
                if (check(!target.isEmpty(), "no alternate sample to pick")) {
                    search->setText(target);
                    check(auditioned.contains(target),
                          "filtering onto a sample did not audition it");
                    QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(search, &ret);
                    check(!picker->popupVisible(), "committing a pick did not close the popup");
                    check(stops > 0, "closing the popup did not stop audition");
                    check(tab->vgSource->voiceAt(dsSlot) &&
                              tab->vgSource->voiceAt(dsSlot)->symbol == target,
                          "the picked symbol did not commit");
                    check(tab->doc.isDirty(), "the picked symbol did not push an undo command");
                    tab->doc.undoStack()->undo();
                    check(tab->vgSource->voiceAt(dsSlot) &&
                              tab->vgSource->voiceAt(dsSlot)->symbol == before.symbol,
                          "undo did not restore the picked symbol");
                    check(picker->currentSymbol() == before.symbol,
                          "undo did not refresh the picker's label");
                }

                // An unlisted symbol still commits via the typed-text row
                // (the editable combo's old superpower).
                picker->openPopup();
                const QString unlisted = QStringLiteral("VgSaveCheckUnlisted");
                search->setText(unlisted);
                QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QCoreApplication::sendEvent(search, &ret);
                check(tab->vgSource->voiceAt(dsSlot) &&
                          tab->vgSource->voiceAt(dsSlot)->symbol == unlisted,
                      "an unlisted typed symbol did not commit");
                tab->doc.undoStack()->undo();
                check(tab->vgSource->voiceAt(dsSlot) &&
                          tab->vgSource->voiceAt(dsSlot)->symbol == before.symbol,
                      "undo did not restore the unlisted symbol");

                // Wave voices share the picker: switching the Type swaps the
                // list to the project's programmable waves, and highlighting
                // one auditions it as a CGB wave.
                if (vgCatalog(m_project.root()).progWave.isEmpty()) {
                    std::printf("vgsavecheck: note: no programmable waves, "
                                "wave picker section skipped\n");
                } else {
                    QComboBox *typeCombo = nullptr;
                    for (QComboBox *combo : m_vgBrowser->findChildren<QComboBox *>()) {
                        if (combo->findData(int(VgMacro::ProgWave)) >= 0)
                            typeCombo = combo;
                    }
                    int undos = 0;
                    if (check(typeCombo != nullptr, "no Type combo")) {
                        const int waveIndex = typeCombo->findData(int(VgMacro::ProgWave));
                        typeCombo->setCurrentIndex(waveIndex);
                        QMetaObject::invokeMethod(typeCombo, "activated", Qt::DirectConnection,
                                                  Q_ARG(int, waveIndex));
                        undos++;
                        const VgVoice *waveVoice = tab->vgSource->voiceAt(dsSlot);
                        check(waveVoice && waveVoice->macro == VgMacro::ProgWave,
                              "type switch to Prog Wave did not take");
                        check(picker->isVisible() && picker->currentSymbol() == waveVoice->symbol,
                              "wave voice does not show the picker");

                        picker->openPopup();
                        QString otherWave;
                        for (QTreeWidgetItemIterator it(list); *it && otherWave.isEmpty(); ++it) {
                            const QString s = (*it)->data(0, Qt::UserRole).toString();
                            if (!s.isEmpty() && s != waveVoice->symbol)
                                otherWave = s;
                        }
                        for (QTreeWidgetItemIterator it(list); *it; ++it) {
                            const QString s = (*it)->data(0, Qt::UserRole).toString();
                            check(s.isEmpty() || vgCatalog(m_project.root()).progWave.contains(s),
                                  "wave picker lists a non-wave symbol");
                            // Waves show the verbatim symbol (samples strip
                            // their shared prefix; waves don't).
                            check(s.isEmpty() || (*it)->text(0) == s,
                                  "wave row does not show its full symbol");
                        }
                        if (otherWave.isEmpty()) {
                            std::printf("vgsavecheck: note: single wave, "
                                        "wave audition/pick skipped\n");
                            QWidget *popup =
                                picker->findChild<QWidget *>(QStringLiteral("vgSamplePickerPopup"));
                            if (popup)
                                popup->hide();
                        } else {
                            const int seen = auditioned.size();
                            search->setText(otherWave);
                            check(auditioned.size() > seen && auditioned.last() == otherWave &&
                                      auditionKinds.last() == VgAuditionKind::Wave,
                                  "filtering onto a wave did not audition "
                                  "it as a wave");
                            QKeyEvent waveRet(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                            QCoreApplication::sendEvent(search, &waveRet);
                            undos++;
                            check(tab->vgSource->voiceAt(dsSlot) &&
                                      tab->vgSource->voiceAt(dsSlot)->symbol == otherWave,
                                  "the picked wave did not commit");
                        }
                        while (undos-- > 0)
                            tab->doc.undoStack()->undo();
                        const VgVoice *restored = tab->vgSource->voiceAt(dsSlot);
                        check(restored && restored->macro == before.macro &&
                                  restored->symbol == before.symbol,
                              "undo did not restore the sample voice after "
                              "the wave round trip");
                    }
                }
            }
            disconnect(c1);
            disconnect(c2);
        }
    }

    // 10b. The _alt PSG types are real choices: poryaaaa honors the FIX bit
    // on the square and wave channels, so each one is offered next to its
    // plain sibling, picking it writes the _alt macro word, and the shared
    // ADSR family carries the envelope digits across untouched. voice_noise_alt
    // stays out of the list — the engine ignores FIX on the noise channel.
    {
        m_vgBrowser->revealSlot(dsSlot);
        QCoreApplication::processEvents();
        QComboBox *typeCombo = nullptr;
        for (QComboBox *combo : m_vgBrowser->findChildren<QComboBox *>()) {
            if (combo->findData(int(VgMacro::Square1)) >= 0)
                typeCombo = combo;
        }
        if (check(typeCombo != nullptr, "no Type combo")) {
            const VgVoice before = *tab->vgSource->voiceAt(dsSlot);
            const VgMacro alts[] = {VgMacro::Square1Alt, VgMacro::Square2Alt, VgMacro::ProgWaveAlt};
            for (VgMacro alt : alts) {
                const int idx = typeCombo->findData(int(alt));
                if (check(idx >= 0, "the Type dropdown does not offer an _alt PSG type"))
                    check(typeCombo->itemText(idx) == vgMacroDisplayName(alt),
                          "an _alt Type entry is mislabeled");
            }
            check(typeCombo->findData(int(VgMacro::NoiseAlt)) < 0,
                  "the Type dropdown offers voice_noise_alt");

            // Sample -> Square 1 -> Square 1 (alt): the envelope the CGB
            // family adopts survives the second hop (same vgAdsrFamily), and
            // the rendered .inc carries the _alt macro word.
            int undos = 0;
            const auto pickType = [&](VgMacro macro) {
                const int idx = typeCombo->findData(int(macro));
                typeCombo->setCurrentIndex(idx);
                QMetaObject::invokeMethod(typeCombo, "activated", Qt::DirectConnection,
                                          Q_ARG(int, idx));
                undos++;
            };
            pickType(VgMacro::Square1);
            const VgVoice plain = *tab->vgSource->voiceAt(dsSlot);
            if (check(plain.macro == VgMacro::Square1, "type switch to Square 1 did not take")) {
                pickType(VgMacro::Square1Alt);
                const VgVoice *altVoice = tab->vgSource->voiceAt(dsSlot);
                if (check(altVoice && altVoice->macro == VgMacro::Square1Alt,
                          "type switch to Square 1 (alt) did not take")) {
                    check(altVoice->attack == plain.attack && altVoice->decay == plain.decay &&
                              altVoice->sustain == plain.sustain &&
                              altVoice->release == plain.release,
                          "the _alt hop did not keep the CGB envelope");
                    check(altVoice->symbol.isEmpty(), "a square voice kept an instrument symbol");
                    check(tab->vgSource->renderPreview().contains(
                              vgMacroName(VgMacro::Square1Alt).toUtf8()),
                          "the rendered .inc is missing the voice_square_1_alt macro");
                }
            }
            while (undos-- > 0)
                tab->doc.undoStack()->undo();
            const VgVoice *restored = tab->vgSource->voiceAt(dsSlot);
            check(restored && *restored == before,
                  "undo did not restore the sample voice after the _alt round trip");
        }
    }

    // 10c. The voice list's context menu and Copy / Paste shortcuts: one
    // voice through the system clipboard as its .inc macro line (plus a
    // private MIME that carries a synth's descriptor). Right-click selects
    // without auditioning; Paste validates against this project; Paste and
    // Clear are standalone undo entries that never merge with an editor
    // run; a view-only tab disables both.
    {
        QTreeWidget *tree = m_vgBrowser->findChild<QTreeWidget *>();
        QClipboard *clipboard = QGuiApplication::clipboard();
        QUndoStack *stack = tab->doc.undoStack();
        // The target: another editable voice that differs from the source.
        int pasteSlot = -1;
        for (int i = 0; i < VOICEGROUP_SIZE && pasteSlot < 0; i++) {
            const VgVoice *v = tab->vgSource->voiceAt(i);
            if (i != dsSlot && v && *v != *tab->vgSource->voiceAt(dsSlot) &&
                v->macro != VgMacro::Square2)
                pasteSlot = i;
        }
        if (check(tree && clipboard && pasteSlot >= 0, "copy/paste fixtures not found")) {
            show();
            const VgVoice source = *tab->vgSource->voiceAt(dsSlot);
            const VgVoice target = *tab->vgSource->voiceAt(pasteSlot);
            const int baseIndex = stack->index();
            const auto rowPos = [&](int slot) {
                m_vgBrowser->revealSlot(slot);
                QCoreApplication::processEvents();
                return tree->visualItemRect(tree->topLevelItem(slot)).center();
            };
            // Opens the row's context menu, records each item's enabled
            // (and, given `checked`, each checkable item's checked) state,
            // and picks `pick` (Escape when it is absent/disabled).
            const auto runMenu = [&](int slot, const QString &pick, QHash<QString, bool> *enabled,
                                     QHash<QString, bool> *checked = nullptr) {
                const QPoint pos = rowPos(slot);
                bool seen = false;
                QTimer::singleShot(0, this, [&] {
                    for (QWidget *w : QApplication::topLevelWidgets()) {
                        auto *menu = qobject_cast<QMenu *>(w);
                        if (!menu || !menu->isVisible())
                            continue;
                        seen = true;
                        QAction *chosen = nullptr;
                        for (QAction *a : menu->actions()) {
                            if (a->isSeparator())
                                continue;
                            enabled->insert(a->text(), a->isEnabled());
                            if (checked && a->isCheckable())
                                checked->insert(a->text(), a->isChecked());
                            if (a->text() == pick && a->isEnabled())
                                chosen = a;
                        }
                        if (chosen)
                            menu->setActiveAction(chosen);
                        QKeyEvent key(QEvent::KeyPress, chosen ? Qt::Key_Return : Qt::Key_Escape,
                                      Qt::NoModifier);
                        QCoreApplication::sendEvent(menu, &key);
                        return;
                    }
                });
                QContextMenuEvent event(QContextMenuEvent::Mouse, pos,
                                        tree->viewport()->mapToGlobal(pos));
                QCoreApplication::sendEvent(tree->viewport(), &event);
                QCoreApplication::processEvents(); // the timer, if no menu ran
                return seen;
            };
            const auto sendKeys = [&](int slot, const QKeySequence &seq) {
                m_vgBrowser->revealSlot(slot);
                const QKeyCombination combo = seq[0];
                QKeyEvent override(QEvent::ShortcutOverride, combo.key(),
                                   combo.keyboardModifiers());
                QCoreApplication::sendEvent(tree, &override);
                QKeyEvent press(QEvent::KeyPress, combo.key(), combo.keyboardModifiers());
                QCoreApplication::sendEvent(tree, &press);
                return override.isAccepted();
            };
            const QKeySequence copyKeys(QKeySequence::Copy), pasteKeys(QKeySequence::Paste);
            const auto voiceAt = [&](int slot) {
                const VgVoice *v = tab->vgSource->voiceAt(slot);
                return v ? *v : VgVoice{};
            };

            // Line rendering and parsing round-trip on their own.
            {
                VgVoice parsed;
                check(VoicegroupSource::parseVoiceLine(VoicegroupSource::renderVoiceLine(source),
                                                       &parsed) &&
                          parsed == source,
                      "renderVoiceLine/parseVoiceLine did not round-trip a voice");
                check(!VoicegroupSource::parseVoiceLine("voice_square_2 60, 0, 2, 0, 1, 9, 2\n"
                                                        "voice_square_2 60, 0, 2, 0, 1, 9, 3\n",
                                                        &parsed),
                      "parseVoiceLine accepted two voice lines");
                check(!VoicegroupSource::parseVoiceLine("cry Cry_Bulbasaur", &parsed),
                      "parseVoiceLine accepted a read-only cry line");
            }

            // Only the left button auditions; a right press still selects.
            int noteOns = 0;
            const QMetaObject::Connection auditions =
                connect(m_vgBrowser, &VoicegroupBrowser::auditionVoice, this,
                        [&noteOns](int, int, int velocity) { noteOns += velocity > 0 ? 1 : 0; });
            const auto click = [&](int slot, Qt::MouseButton button) {
                const QPoint pos = rowPos(slot);
                QMouseEvent press(QEvent::MouseButtonPress, pos, tree->viewport()->mapToGlobal(pos),
                                  button, button, Qt::NoModifier);
                QCoreApplication::sendEvent(tree->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, pos,
                                    tree->viewport()->mapToGlobal(pos), button, Qt::NoButton,
                                    Qt::NoModifier);
                QCoreApplication::sendEvent(tree->viewport(), &release);
            };
            click(dsSlot, Qt::LeftButton);
            check(noteOns == 1, "a left press on a voice row did not audition");
            click(pasteSlot, Qt::RightButton);
            check(noteOns == 1, "a right press on a voice row auditioned");
            check(m_vgBrowser->currentSlot() == pasteSlot, "a right press did not select the row");
            click(dsSlot, Qt::MiddleButton);
            check(noteOns == 1, "a middle press on a voice row auditioned");
            disconnect(auditions);

            // Space keeps toggling playback with the tree focused; the
            // copy/paste chords are claimed by the tree.
            {
                QKeyEvent space(QEvent::ShortcutOverride, Qt::Key_Space, Qt::NoModifier);
                QCoreApplication::sendEvent(tree, &space);
                check(!space.isAccepted(), "the voice tree claimed Space from the transport");
            }

            // Copy (menu) -> Paste (shortcut): the clipboard carries the
            // macro line and the private MIME; one undo entry, labelled.
            QHash<QString, bool> items;
            check(runMenu(dsSlot, tr("Copy"), &items), "the voice row's context menu did not open");
            check(items.value(tr("Copy")) && items.contains(tr("Paste")) &&
                      items.contains(tr("Clear")),
                  "the context menu lacks Copy/Paste/Clear");
            const QString sourceLine = QString::fromUtf8(VoicegroupSource::renderVoiceLine(source));
            check(clipboard->text() == sourceLine,
                  "Copy did not put the voice's macro line on the clipboard");
            check(clipboard->mimeData() && clipboard->mimeData()->hasFormat(
                                               QStringLiteral("application/x-porydaw-voice")),
                  "Copy did not write the private voice MIME type");
            check(sendKeys(pasteSlot, pasteKeys), "the voice tree did not claim the Paste chord");
            check(voiceAt(pasteSlot) == source, "Paste did not replace the target voice");
            check(stack->index() == baseIndex + 1 &&
                      stack->text(stack->index() - 1) == tr("paste voice %1").arg(pasteSlot),
                  "Paste was not one undo entry labelled \"paste voice N\"");
            stack->undo();
            check(voiceAt(pasteSlot) == target && stack->index() == baseIndex,
                  "one undo did not restore the pasted-over voice");

            // Copy by shortcut writes the same line.
            clipboard->clear();
            check(sendKeys(dsSlot, copyKeys) && clipboard->text() == sourceLine,
                  "the Copy shortcut did not copy the voice");

            // A hand-written .inc line (indent, comment, line ending) pastes
            // from the menu.
            clipboard->setText(QStringLiteral("\tvoice_square_2 60, 0, 1, 0, 1, 9, 2 @ by hand\n"));
            items.clear();
            runMenu(pasteSlot, tr("Paste"), &items);
            const VgVoice handWritten = voiceAt(pasteSlot);
            check(items.value(tr("Paste")) && handWritten.macro == VgMacro::Square2 &&
                      handWritten.duty == 1 && handWritten.attack == 0 && handWritten.decay == 1 &&
                      handWritten.sustain == 9 && handWritten.release == 2,
                  "a hand-written .inc line did not paste");
            if (stack->index() > baseIndex)
                stack->undo();

            // Refusals: an unknown sample symbol, an undefined macro, and
            // text that isn't a voice line push nothing and say why.
            clipboard->setText(QStringLiteral(
                "voice_directsound 60, 0, DirectSoundWaveData_NoSuchSample, 255, 0, 255, 0"));
            statusBar()->clearMessage();
            sendKeys(pasteSlot, pasteKeys);
            check(stack->index() == baseIndex && voiceAt(pasteSlot) == target,
                  "a voice with an unknown sample symbol pasted");
            check(statusBar()->currentMessage().contains(QStringLiteral("NoSuchSample")),
                  "the unknown-symbol refusal left no status message");
            // A project without the expansion's compressed macros (the
            // browser's macro list stands in for one).
            m_vgBrowser->setSampleVariants({QStringLiteral("voice_directsound")},
                                           vgCatalog(tab->root).cries);
            clipboard->setText(QStringLiteral(
                "voice_directsound_compressed 60, 0, Cry_Bulbasaur, 255, 0, 255, 0"));
            statusBar()->clearMessage();
            sendKeys(pasteSlot, pasteKeys);
            check(stack->index() == baseIndex &&
                      statusBar()->currentMessage().contains(
                          QStringLiteral("voice_directsound_compressed")),
                  "a macro this project doesn't define pasted");
            m_vgBrowser->setSampleVariants(vgCatalog(tab->root).voiceMacroWords,
                                           vgCatalog(tab->root).cries);

            // Pastes text over pasteSlot by shortcut: true when it pushed
            // one entry (left on the stack for the caller to inspect).
            const auto pasteText = [&](const QString &text) {
                clipboard->setText(text);
                statusBar()->clearMessage();
                sendKeys(pasteSlot, pasteKeys);
                return stack->index() == baseIndex + 1;
            };
            const auto unwind = [&] {
                while (stack->index() > baseIndex)
                    stack->undo();
            };
            const QString sample = vgCatalog(tab->root).directSound.value(0);
            const auto dsLine = [&](const QString &macro, const QString &args) {
                return QStringLiteral("%1 60, 0, %2, %3").arg(macro, sample, args);
            };

            // Keysplit and drumkit lines paste; a keysplit adopts the table
            // this project pairs with its sub-voicegroup.
            const auto &keysplits = vgCatalog(tab->root).keysplits;
            const QString drumkit = vgCatalog(tab->root).drumkits.value(0);
            const QString wave = vgCatalog(tab->root).progWave.value(0);
            if (check(!keysplits.isEmpty() && !drumkit.isEmpty() && !wave.isEmpty() &&
                          !sample.isEmpty(),
                      "no keysplit/drumkit/wave/sample symbols for the paste checks")) {
                const bool keysplitPasted =
                    pasteText(QStringLiteral("voice_keysplit %1, keysplit_vgsavecheck_wrong")
                                  .arg(keysplits.first().first));
                const VgVoice keysplit = voiceAt(pasteSlot);
                check(keysplitPasted && keysplit.macro == VgMacro::Keysplit &&
                          keysplit.symbol == keysplits.first().first &&
                          keysplit.keysplitTable == keysplits.first().second,
                      "a keysplit line did not paste with the project's table");
                unwind();
                const bool drumkitPasted =
                    pasteText(QStringLiteral("voice_keysplit_all %1").arg(drumkit));
                check(drumkitPasted && voiceAt(pasteSlot).macro == VgMacro::KeysplitAll &&
                          voiceAt(pasteSlot).symbol == drumkit,
                      "a drumkit line did not paste");
                unwind();
                // Unknown wave / drumkit / keysplit / cry symbols are refused.
                const QString unknownLines[] = {
                    QStringLiteral("voice_programmable_wave 60, 0, "
                                   "ProgrammableWaveData_VgSaveCheckNone, 0, 7, 15, 0"),
                    QStringLiteral("voice_keysplit_all voicegroup_vgsavecheck_none"),
                    QStringLiteral("voice_keysplit voicegroup_vgsavecheck_none, keysplit_piano"),
                    QStringLiteral(
                        "voice_directsound_compressed 60, 0, Cry_VgSaveCheckNone, 255, 0, 255, 0"),
                };
                for (const QString &line : unknownLines) {
                    const bool pasted = pasteText(line);
                    unwind();
                    // (Expansion projects lack the compressed macros: that
                    // refusal names the macro instead.)
                    const QString why = statusBar()->currentMessage();
                    check(!pasted && voiceAt(pasteSlot) == target &&
                              why.startsWith(QStringLiteral("Can't paste")) &&
                              (why.contains(QStringLiteral("vgsavecheck"), Qt::CaseInsensitive) ||
                               why.contains(QStringLiteral("doesn't define"))),
                          qPrintable(QStringLiteral("an unknown symbol pasted (or no status "
                                                    "message): %1")
                                         .arg(line)));
                }

                // A reverse twin keeps its spelling when the project defines
                // it and otherwise lands as the twin the project has; neither
                // is refused; an empty macro list (nothing scanned) trusts
                // the line.
                const QString plain = QStringLiteral("voice_directsound");
                const QString alt = QStringLiteral("voice_directsound_alt");
                const QString reverse = QStringLiteral("voice_directsound_reverse");
                const QString envelope = QStringLiteral("255, 0, 255, 0");
                const auto reversePastesAs = [&](const QStringList &words, const QString &macro) {
                    m_vgBrowser->setSampleVariants(words, vgCatalog(tab->root).cries);
                    const bool pasted = pasteText(dsLine(macro, envelope));
                    const VgMacro landed = voiceAt(pasteSlot).macro;
                    unwind();
                    return pasted ? int(landed) : -1;
                };
                check(reversePastesAs({plain, alt}, reverse) == int(VgMacro::DirectSoundAlt),
                      "a _reverse voice did not paste as _alt into an _alt-only project");
                check(reversePastesAs({plain, reverse}, alt) == int(VgMacro::DirectSoundReverse),
                      "an _alt voice did not paste as _reverse into a _reverse-only project");
                check(reversePastesAs({plain, alt, reverse}, reverse) ==
                          int(VgMacro::DirectSoundReverse),
                      "a _reverse voice did not keep its spelling when both are defined");
                check(reversePastesAs({plain, alt, reverse}, alt) == int(VgMacro::DirectSoundAlt),
                      "an _alt voice did not keep its spelling when both are defined");
                check(reversePastesAs({plain}, alt) == -1,
                      "a reversed voice pasted into a project with neither reverse macro");
                check(reversePastesAs({}, reverse) == int(VgMacro::DirectSoundReverse),
                      "an empty macro scan did not trust the pasted line");
                check(reversePastesAs({alt}, plain) == -1,
                      "voice_directsound pasted though the scan didn't find it");
                m_vgBrowser->setSampleVariants(vgCatalog(tab->root).voiceMacroWords,
                                               vgCatalog(tab->root).cries);

                // Out-of-range (or int-overflowing) numbers are refused, not
                // clamped; the boundaries paste.
                const QString outOfRange[] = {
                    QStringLiteral("voice_directsound 128, 0, %1, 255, 0, 255, 0").arg(sample),
                    QStringLiteral("voice_directsound 60, 128, %1, 255, 0, 255, 0").arg(sample),
                    QStringLiteral("voice_directsound 60, -1, %1, 255, 0, 255, 0").arg(sample),
                    QStringLiteral("voice_directsound 60, 0, %1, 256, 0, 255, 0").arg(sample),
                    QStringLiteral("voice_directsound 60, 0, %1, 255, 0, 255, 256").arg(sample),
                    QStringLiteral("voice_directsound 60, 0, %1, 255, 0, 255, 4294967296")
                        .arg(sample),
                    QStringLiteral("voice_square_1 60, 0, 128, 2, 0, 0, 15, 0"),
                    QStringLiteral("voice_square_1 60, 0, 0, 4, 0, 0, 15, 0"),
                    QStringLiteral("voice_square_2 60, 0, 2, 8, 0, 15, 0"),
                    QStringLiteral("voice_square_2 60, 0, 2, 0, 8, 15, 0"),
                    QStringLiteral("voice_square_2 60, 0, 2, 0, 0, 16, 0"),
                    QStringLiteral("voice_square_2 60, 0, 2, 0, 0, 15, 8"),
                    QStringLiteral("voice_noise 60, 0, 2, 0, 0, 15, 0"),
                    QStringLiteral("voice_programmable_wave 60, 0, %1, 0, 7, 16, 0").arg(wave),
                };
                for (const QString &line : outOfRange) {
                    const bool pasted = pasteText(line);
                    unwind();
                    check(
                        !pasted && voiceAt(pasteSlot) == target &&
                            statusBar()->currentMessage().contains(QStringLiteral("out of range")),
                        qPrintable(QStringLiteral("an out-of-range voice pasted (or no "
                                                  "status message): %1")
                                       .arg(line)));
                }
                const QString boundaries[] = {
                    QStringLiteral("voice_directsound 127, 127, %1, 255, 255, 255, 255")
                        .arg(sample),
                    QStringLiteral("voice_square_1 0, 0, 127, 3, 7, 7, 15, 7"),
                    QStringLiteral("voice_noise 60, 0, 1, 7, 7, 15, 7"),
                };
                for (const QString &line : boundaries) {
                    const bool pasted = pasteText(line);
                    unwind();
                    check(
                        pasted,
                        qPrintable(QStringLiteral("an in-range voice was refused: %1").arg(line)));
                }
            }

            // Every editable voice line in the project's sound data stays
            // pasteable: in range, no overflow, parsed the same.
            {
                int lines = 0;
                QStringList bad;
                QDirIterator it(tab->root + QStringLiteral("/sound"), {QStringLiteral("*.inc")},
                                QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString path = it.next();
                    for (const VgSourceLine &line :
                         VoicegroupSource::parseSource(readFileBytes(path)).lines) {
                        if (line.kind != VgLineKind::Editable)
                            continue;
                        lines++;
                        VgVoice parsed;
                        bool overflow = true;
                        VgFieldRange range;
                        if (!VoicegroupSource::parseVoiceLine(line.raw, &parsed, &overflow) ||
                            overflow || parsed != line.voice || vgVoiceOutOfRange(parsed, &range))
                            bad.append(QString::fromUtf8(line.raw.trimmed()));
                    }
                }
                std::printf("vgsavecheck: note: %d editable voice lines range-checked\n", lines);
                check(lines > 0 && bad.isEmpty(),
                      qPrintable(QStringLiteral("project voice lines would not paste: %1")
                                     .arg(bad.mid(0, 5).join(QStringLiteral(" | ")))));
            }

            // A row with no editable voice, or no row at all: Copy and Paste
            // say why instead of doing nothing.
            int emptySlot = -1;
            for (int i = VOICEGROUP_SIZE - 1; i >= 0 && emptySlot < 0; i--) {
                if (!tab->vgSource->voiceAt(i))
                    emptySlot = i;
            }
            if (check(emptySlot >= 0, "no non-editable voice row for the refusal checks")) {
                clipboard->setText(QStringLiteral("vgsavecheck sentinel"));
                statusBar()->clearMessage();
                sendKeys(emptySlot, copyKeys);
                check(clipboard->text() == QStringLiteral("vgsavecheck sentinel") &&
                          statusBar()->currentMessage().startsWith(QStringLiteral("Can't copy")),
                      "Copy on a non-editable row left no status message (or copied)");
                clipboard->setText(sourceLine);
                statusBar()->clearMessage();
                sendKeys(emptySlot, pasteKeys);
                check(stack->index() == baseIndex &&
                          statusBar()->currentMessage().startsWith(QStringLiteral("Can't paste")),
                      "Paste on a non-editable row left no status message (or pasted)");
            }
            // voicegroup.copy rebound: the platform Copy chord no longer
            // copies, and the view doesn't copy the row's text either.
            {
                auto &keys = keymap::Registry::instance();
                keys.setBinding(QStringLiteral("voicegroup.copy"),
                                QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F8")));
                clipboard->setText(QStringLiteral("vgsavecheck sentinel"));
                sendKeys(dsSlot, copyKeys);
                check(clipboard->text() == QStringLiteral("vgsavecheck sentinel"),
                      "the Copy chord copied something with voicegroup.copy rebound");
                keys.resetBinding(QStringLiteral("voicegroup.copy"));
            }

            clipboard->setText(QStringLiteral("hello"));
            items.clear();
            runMenu(pasteSlot, QString(), &items);
            check(items.contains(tr("Paste")) && !items.value(tr("Paste")),
                  "Paste is enabled with no voice on the clipboard");

            // Clear writes the placeholder, as one labelled undo entry, and
            // is disabled on a voice that already is the placeholder.
            const VgVoice placeholder{
                VgMacro::Square1, 60, 0, QString(), QString(), 0, 2, 0, 0, 0, 15, 0};
            items.clear();
            runMenu(dsSlot, tr("Clear"), &items);
            check(voiceAt(dsSlot) == placeholder && tab->vgSource->renderPreview().contains(
                                                        "voice_square_1 60, 0, 0, 2, 0, 0, 15, 0"),
                  "Clear did not write the placeholder voice");
            check(stack->index() == baseIndex + 1 &&
                      stack->text(stack->index() - 1) == tr("clear voice %1").arg(dsSlot),
                  "Clear was not one undo entry labelled \"clear voice N\"");
            items.clear();
            runMenu(dsSlot, QString(), &items);
            check(items.contains(tr("Clear")) && !items.value(tr("Clear")),
                  "Clear is enabled on a placeholder voice");
            stack->undo();
            check(voiceAt(dsSlot) == source && stack->index() == baseIndex,
                  "one undo did not restore the cleared voice");

            // Equality is the rendered line: a Square 1 that writes the
            // placeholder line but carries a field the macro doesn't use
            // (a noise period) is the placeholder for Clear, and pasting
            // its own line over it is a no-op.
            {
                VgVoice hidden = placeholder;
                hidden.period = 1;
                emit m_vgBrowser->voiceEditRequested(dsSlot, hidden, true,
                                                     QStringLiteral("vgsavecheck hidden field"));
                const int hiddenIndex = stack->index();
                if (check(hiddenIndex == baseIndex + 1 && voiceAt(dsSlot) == hidden,
                          "the hidden-field voice fixture did not land")) {
                    items.clear();
                    runMenu(dsSlot, QString(), &items);
                    check(items.contains(tr("Clear")) && !items.value(tr("Clear")),
                          "Clear is enabled on a voice that renders as the placeholder");
                    clipboard->setText(
                        QString::fromUtf8(VoicegroupSource::renderVoiceLine(hidden)));
                    sendKeys(dsSlot, pasteKeys);
                    check(stack->index() == hiddenIndex && voiceAt(dsSlot) == hidden,
                          "pasting a voice's own rendered line over it pushed an undo entry");
                }
                while (stack->index() > baseIndex)
                    stack->undo();
                check(voiceAt(dsSlot) == source, "undo did not restore the hidden-field fixture");
            }

            // A scalar-only paste after a spin edit of the same field stays
            // its own entry (and a spin edit after it doesn't fold in).
            QSpinBox *releaseSpin = nullptr;
            for (QSpinBox *spin : m_vgBrowser->findChildren<QSpinBox *>()) {
                if (spin->toolTip() == tr("Release"))
                    releaseSpin = spin;
            }
            if (check(releaseSpin != nullptr, "no Release spin box for the merge check")) {
                m_vgBrowser->selectSlot(dsSlot);
                const int spun = source.release == 20 ? 21 : 20;
                releaseSpin->setValue(spun);
                VgVoice scalar = source;
                scalar.release = source.release == 30 ? 31 : 30;
                clipboard->setText(QString::fromUtf8(VoicegroupSource::renderVoiceLine(scalar)));
                sendKeys(dsSlot, pasteKeys);
                check(stack->index() == baseIndex + 2 && voiceAt(dsSlot) == scalar,
                      "a scalar-only paste merged into the preceding spin edit");
                m_vgBrowser->selectSlot(dsSlot);
                releaseSpin->setValue(spun);
                check(stack->index() == baseIndex + 3,
                      "a spin edit merged into the preceding paste");
                stack->undo();
                stack->undo();
                check(voiceAt(dsSlot).release == spun,
                      "undoing the paste did not return to the spin-edited voice");
                stack->undo();
                check(voiceAt(dsSlot) == source && stack->index() == baseIndex,
                      "undoing the spin edit did not restore the voice");
            }

            // A synth only the private MIME's descriptor knows (another
            // project's pending definition) resolves through the mint path;
            // the same line as plain text is refused.
            // (Section 7 gave the scratch project synth support.)
            if (check(vgCatalog(tab->root).synths.creatable(),
                      "the scratch project has no synth support for the paste checks")) {
                const VgSynthDesc foreign{0, 0x33, 0x01, 0x22, 0x00};
                const QString line = QStringLiteral("voice_directsound 60, 0, "
                                                    "DirectSoundSynth_Elsewhere, 255, 0, 255, 165");
                clipboard->setText(line);
                sendKeys(pasteSlot, pasteKeys);
                check(stack->index() == baseIndex,
                      "an unknown synth symbol pasted without its descriptor");
                auto *mime = new QMimeData;
                mime->setText(line);
                mime->setData(QStringLiteral("application/x-porydaw-voice"),
                              QStringLiteral("{\"line\":\"%1\",\"synth\":{\"waveform\":0,"
                                             "\"baseDuty\":51,\"dutyStep\":1,\"modDepth\":34,"
                                             "\"phase\":0}}")
                                  .arg(line)
                                  .toUtf8());
                clipboard->setMimeData(mime);
                sendKeys(pasteSlot, pasteKeys);
                const VgVoice pasted = voiceAt(pasteSlot);
                check(stack->index() == baseIndex + 1 &&
                          pasted.symbol == vgSynthSymbolName(foreign) &&
                          m_pendingSynths.value(pasted.symbol) == foreign,
                      "a synth descriptor on the clipboard did not mint its symbol");
                // Copying the pasted synth carries its descriptor along.
                sendKeys(pasteSlot, copyKeys);
                check(clipboard->mimeData() &&
                          clipboard->mimeData()
                              ->data(QStringLiteral("application/x-porydaw-voice"))
                              .contains("\"baseDuty\":51"),
                      "copying a synth voice did not carry its descriptor");
                while (stack->index() > baseIndex)
                    stack->undo();

                // A local synth of the same name but another descriptor is a
                // different instrument: the descriptor resolves, not the name.
                // The same descriptor keeps the name.
                const auto synthMime = [&](const QString &symbol, const VgSynthDesc &desc) {
                    const QString synthLine =
                        QStringLiteral("voice_directsound 60, 0, %1, 255, 0, 255, 165").arg(symbol);
                    auto *m = new QMimeData;
                    m->setText(synthLine);
                    m->setData(QStringLiteral("application/x-porydaw-voice"),
                               QStringLiteral("{\"line\":\"%1\",\"synth\":{\"waveform\":%2,"
                                              "\"baseDuty\":%3,\"dutyStep\":%4,"
                                              "\"modDepth\":%5,\"phase\":%6}}")
                                   .arg(synthLine)
                                   .arg(desc.waveform)
                                   .arg(desc.baseDuty)
                                   .arg(desc.dutyStep)
                                   .arg(desc.modDepth)
                                   .arg(desc.phase)
                                   .toUtf8());
                    clipboard->setMimeData(m);
                    sendKeys(pasteSlot, pasteKeys);
                    const QString symbolLanded =
                        stack->index() == baseIndex + 1 ? voiceAt(pasteSlot).symbol : QString();
                    while (stack->index() > baseIndex)
                        stack->undo();
                    return symbolLanded;
                };
                const VgSynthDesc saw{1, 0, 0, 0, 0};
                check(synthMime(QStringLiteral("VgSaveCheckSaw"), foreign) ==
                          vgSynthSymbolName(foreign),
                      "a synth pasted onto a same-named local synth with another descriptor "
                      "kept the local name");
                check(synthMime(QStringLiteral("VgSaveCheckSaw"), saw) ==
                          QStringLiteral("VgSaveCheckSaw"),
                      "a synth with the local definition's descriptor did not keep its name");
            }

            // 10d. The variant radio group: siblings that share an envelope
            // family (Normal / No Resample / Reversed, Normal / Reversed for
            // compressed samples, Normal / Detuned for squares and waves),
            // only those the project defines; the current macro is checked;
            // each pick is one labelled undo entry that changes nothing but
            // the macro word. Reversed writes the project's own spelling, in
            // the menu and in the Type dropdown alike.
            {
                const QString plainW = QStringLiteral("voice_directsound");
                const QString noResW = QStringLiteral("voice_directsound_no_resample");
                const QString altW = QStringLiteral("voice_directsound_alt");
                const QString revW = QStringLiteral("voice_directsound_reverse");
                const QString compW = QStringLiteral("voice_directsound_compressed");
                const QString compRevW = QStringLiteral("voice_directsound_compressed_reverse");
                const QStringList allWords{plainW, noResW, altW, revW, compW, compRevW};
                const QStringList &cries = vgCatalog(tab->root).cries;
                const QString normal = tr("Normal"), noRes = tr("No Resample"),
                              reversed = tr("Reversed"), detuned = tr("Detuned");
                const QString variantText = tr("set voice %1 variant").arg(pasteSlot);
                // A fixture voice over pasteSlot, as the one entry above
                // baseIndex, in a project defining `words` (set after the
                // structural reload, which hands the browser the real scan).
                const auto setFixture = [&](const QStringList &words, const QString &line) {
                    unwind();
                    VgVoice v;
                    if (!VoicegroupSource::parseVoiceLine(line.toUtf8(), &v))
                        return false;
                    emit m_vgBrowser->voiceEditRequested(pasteSlot, v, true,
                                                         QStringLiteral("vgsavecheck fixture"));
                    m_vgBrowser->setSampleVariants(words, cries);
                    return stack->index() == baseIndex + 1 && voiceAt(pasteSlot) == v;
                };
                struct MenuState {
                    QStringList group; // the checkable items, sorted
                    QString checked;
                    bool anyEnabled = false;
                    bool allEnabled = true;
                };
                const auto menuState = [&](int slot) {
                    QHash<QString, bool> enabled, checked;
                    runMenu(slot, QString(), &enabled, &checked);
                    MenuState st;
                    for (auto it = checked.cbegin(); it != checked.cend(); ++it) {
                        st.group.append(it.key());
                        if (it.value())
                            st.checked = it.key();
                        st.anyEnabled |= enabled.value(it.key());
                        st.allEnabled &= enabled.value(it.key());
                    }
                    st.group.sort();
                    return st;
                };
                const auto sorted = [](QStringList list) {
                    list.sort();
                    return list;
                };
                const auto pick = [&](const QString &label) {
                    QHash<QString, bool> enabled;
                    runMenu(pasteSlot, label, &enabled);
                };
                // Only the macro moved.
                const auto sameButMacro = [](const VgVoice &a, const VgVoice &b) {
                    VgVoice c = a;
                    c.macro = b.macro;
                    return c == b;
                };
                const QString dsFixture =
                    QStringLiteral("voice_directsound 50, 70, %1, 250, 10, 200, 30").arg(sample);

                // Plain samples, per what the project defines.
                struct Gate {
                    QStringList words;
                    QStringList group;
                    int reversedMacro; // what Reversed writes; -1 when absent
                };
                const Gate gates[] = {
                    {{plainW, noResW, altW},
                     {normal, noRes, reversed},
                     int(VgMacro::DirectSoundAlt)},
                    {{plainW, noResW, revW},
                     {normal, noRes, reversed},
                     int(VgMacro::DirectSoundReverse)},
                    {{plainW, noResW, altW, revW},
                     {normal, noRes, reversed},
                     int(VgMacro::DirectSoundAlt)},
                    {{}, {normal, noRes, reversed}, int(VgMacro::DirectSoundAlt)},
                    {{plainW, noResW}, {normal, noRes}, -1},
                    {{plainW}, {}, -1},
                };
                for (const Gate &gate : gates) {
                    const QString words =
                        QStringLiteral("[%1]").arg(gate.words.join(QStringLiteral(", ")));
                    if (!check(setFixture(gate.words, dsFixture),
                               "the plain-sample variant fixture did not land"))
                        continue;
                    const MenuState st = menuState(pasteSlot);
                    check(st.group == sorted(gate.group) &&
                              (gate.group.isEmpty() || (st.checked == normal && st.allEnabled)),
                          qPrintable(
                              QStringLiteral("a plain sample's variant group is wrong for "
                                             "macros %1: [%2], checked '%3'")
                                  .arg(words, st.group.join(QStringLiteral(", ")), st.checked)));
                    if (gate.reversedMacro < 0)
                        continue;
                    const VgVoice before = voiceAt(pasteSlot);
                    pick(reversed);
                    const VgVoice after = voiceAt(pasteSlot);
                    check(after.macro == VgMacro(gate.reversedMacro) &&
                              sameButMacro(before, after) && stack->index() == baseIndex + 2 &&
                              stack->text(baseIndex + 1) == variantText,
                          qPrintable(QStringLiteral("Reversed did not write %1 as one \"set "
                                                    "voice N variant\" entry for macros %2")
                                         .arg(vgMacroName(VgMacro(gate.reversedMacro)), words)));
                    check(menuState(pasteSlot).checked == reversed,
                          "the reversed voice does not show Reversed checked");
                    pick(noRes);
                    check(voiceAt(pasteSlot).macro == VgMacro::DirectSoundNoResample &&
                              sameButMacro(before, voiceAt(pasteSlot)) &&
                              stack->index() == baseIndex + 3,
                          "No Resample did not land as its own entry");
                    stack->undo();
                    check(voiceAt(pasteSlot) == after,
                          "one undo did not return to the reversed variant");
                }

                // A reversed voice shows as Reversed in either spelling. One
                // the project defines (or that no scan can judge, or that has
                // no defined twin) keeps it: re-picking is a no-op. One the
                // project lacks while it defines the twin is one click from
                // repaired: re-picking Reversed writes the twin as one
                // labelled entry.
                const struct {
                    QStringList words;
                    VgMacro macro;
                    VgMacro repaired; // what re-picking Reversed leaves
                } reversedCases[] = {
                    {{plainW, noResW, altW, revW},
                     VgMacro::DirectSoundReverse,
                     VgMacro::DirectSoundReverse},
                    {{plainW, noResW, altW, revW},
                     VgMacro::DirectSoundAlt,
                     VgMacro::DirectSoundAlt},
                    {{}, VgMacro::DirectSoundReverse, VgMacro::DirectSoundReverse},
                    {{plainW, noResW}, VgMacro::DirectSoundAlt, VgMacro::DirectSoundAlt},
                    {{plainW, noResW, altW}, VgMacro::DirectSoundReverse, VgMacro::DirectSoundAlt},
                    {{plainW, noResW, revW}, VgMacro::DirectSoundAlt, VgMacro::DirectSoundReverse},
                };
                for (const auto &rc : reversedCases) {
                    const QString words =
                        QStringLiteral("[%1]").arg(rc.words.join(QStringLiteral(", ")));
                    if (!check(
                            setFixture(rc.words, QStringLiteral("%1 50, 70, %2, 250, 10, 200, 30")
                                                     .arg(vgMacroName(rc.macro), sample)),
                            "the reversed-sample fixture did not land"))
                        continue;
                    const MenuState st = menuState(pasteSlot);
                    check(st.group == sorted({normal, noRes, reversed}) && st.checked == reversed,
                          qPrintable(QStringLiteral("a %1 voice does not show Reversed checked "
                                                    "for macros %2")
                                         .arg(vgMacroName(rc.macro), words)));
                    const VgVoice before = voiceAt(pasteSlot);
                    pick(reversed);
                    const bool repairs = rc.repaired != rc.macro;
                    check(voiceAt(pasteSlot).macro == rc.repaired &&
                              sameButMacro(before, voiceAt(pasteSlot)) &&
                              stack->index() == baseIndex + (repairs ? 2 : 1) &&
                              (!repairs || stack->text(baseIndex + 1) == variantText),
                          qPrintable(
                              repairs
                                  ? QStringLiteral("re-picking Reversed did not repair %1 to "
                                                   "%2 as one \"set voice N variant\" entry "
                                                   "for macros %3")
                                        .arg(vgMacroName(rc.macro), vgMacroName(rc.repaired), words)
                                  : QStringLiteral("re-picking the checked Reversed changed "
                                                   "a %1 voice for macros %2")
                                        .arg(vgMacroName(rc.macro), words)));
                    if (repairs) {
                        // (The macro change reloaded the real scan.)
                        m_vgBrowser->setSampleVariants(rc.words, cries);
                        check(menuState(pasteSlot).checked == reversed,
                              "the repaired voice does not show Reversed checked");
                        pick(reversed);
                        check(stack->index() == baseIndex + 2 &&
                                  voiceAt(pasteSlot).macro == rc.repaired,
                              "re-picking Reversed on the repaired voice changed it");
                    }
                    const int at = stack->index();
                    pick(normal);
                    check(voiceAt(pasteSlot).macro == VgMacro::DirectSound &&
                              stack->index() == at + 1,
                          "Normal did not switch a reversed voice back to voice_directsound");
                }

                // Compressed samples: Normal / Reversed, both defined or no
                // group at all.
                const QString cry = cries.value(0);
                if (check(!cry.isEmpty(), "no cry symbol for the compressed variant checks")) {
                    const QString compFixture =
                        QStringLiteral("voice_directsound_compressed 60, 0, %1, 255, 0, 255, 0")
                            .arg(cry);
                    if (check(setFixture(allWords, compFixture),
                              "the compressed variant fixture did not land")) {
                        const MenuState st = menuState(pasteSlot);
                        check(st.group == sorted({normal, reversed}) && st.checked == normal,
                              "a compressed voice's variant group is not Normal / Reversed");
                        const VgVoice before = voiceAt(pasteSlot);
                        pick(reversed);
                        check(voiceAt(pasteSlot).macro == VgMacro::DirectSoundCompressedReverse &&
                                  sameButMacro(before, voiceAt(pasteSlot)) &&
                                  stack->index() == baseIndex + 2,
                              "Reversed did not write voice_directsound_compressed_reverse");
                    }
                    if (check(setFixture({plainW, compW}, compFixture),
                              "the compressed variant fixture did not land"))
                        check(menuState(pasteSlot).group.isEmpty(),
                              "a compressed voice offers Reversed without its macro");
                    if (check(setFixture({}, compFixture),
                              "the compressed variant fixture did not land"))
                        check(menuState(pasteSlot).group == sorted({normal, reversed}),
                              "an empty macro scan gated the compressed variants");
                }

                // Squares and waves: Normal / Detuned, never gated by the
                // (sample-only) macro scan.
                const struct {
                    QString line;
                    VgMacro plain;
                    VgMacro alt;
                } psgCases[] = {
                    {QStringLiteral("voice_square_1 60, 0, 21, 1, 1, 2, 9, 3"), VgMacro::Square1,
                     VgMacro::Square1Alt},
                    {QStringLiteral("voice_square_2_alt 60, 0, 1, 1, 2, 9, 3"), VgMacro::Square2,
                     VgMacro::Square2Alt},
                    {QStringLiteral("voice_programmable_wave 60, 0, %1, 1, 2, 9, 3").arg(wave),
                     VgMacro::ProgWave, VgMacro::ProgWaveAlt},
                };
                for (const auto &pc : psgCases) {
                    if (!check(setFixture({plainW}, pc.line),
                               "the PSG variant fixture did not land"))
                        continue;
                    const VgVoice before = voiceAt(pasteSlot);
                    const bool startsAlt = before.macro == pc.alt;
                    const MenuState st = menuState(pasteSlot);
                    check(st.group == sorted({normal, detuned}) &&
                              st.checked == (startsAlt ? detuned : normal),
                          qPrintable(QStringLiteral("%1's variant group is not Normal / Detuned "
                                                    "with the current one checked")
                                         .arg(vgMacroName(before.macro))));
                    pick(startsAlt ? normal : detuned);
                    const VgVoice after = voiceAt(pasteSlot);
                    check(after.macro == (startsAlt ? pc.plain : pc.alt) &&
                              sameButMacro(before, after) && stack->index() == baseIndex + 2 &&
                              stack->text(baseIndex + 1) == variantText,
                          qPrintable(QStringLiteral("the %1 variant pick did not switch only the "
                                                    "macro as one entry")
                                         .arg(vgMacroName(before.macro))));
                }

                // No group: noise (its _alt is inert), keysplit, drumkit,
                // synth, and rows without an editable voice.
                QStringList noGroup{QStringLiteral("voice_noise 60, 0, 0, 0, 2, 9, 3"),
                                    QStringLiteral("voice_noise_alt 60, 0, 1, 0, 2, 9, 3"),
                                    QStringLiteral("voice_keysplit_all %1").arg(drumkit)};
                if (!keysplits.isEmpty())
                    noGroup.append(QStringLiteral("voice_keysplit %1, %2")
                                       .arg(keysplits.first().first, keysplits.first().second));
                if (vgCatalog(tab->root).synths.creatable())
                    noGroup.append(QStringLiteral(
                        "voice_directsound 60, 0, VgSaveCheckSaw, 255, 0, 255, 165"));
                for (const QString &line : noGroup) {
                    if (check(setFixture(allWords, line), "a no-variant fixture did not land"))
                        check(
                            menuState(pasteSlot).group.isEmpty(),
                            qPrintable(QStringLiteral("a variant group showed for %1").arg(line)));
                }
                unwind();
                if (emptySlot >= 0)
                    check(menuState(emptySlot).group.isEmpty(),
                          "a row without an editable voice shows a variant group");

                // View-only: the group shows, every item disabled.
                if (check(setFixture(allWords, dsFixture), "the view-only fixture did not land")) {
                    m_vgBrowser->setViewOnly(true);
                    const MenuState st = menuState(pasteSlot);
                    check(!st.group.isEmpty() && !st.anyEnabled,
                          "view-only left the variant items enabled");
                    pick(reversed);
                    check(stack->index() == baseIndex + 1,
                          "a variant pick edited a view-only voicegroup");
                    m_vgBrowser->setViewOnly(false);
                }

                // The Type dropdown lists "Sample (reversed)" once, as the
                // voice's own spelling or else the project's, and writes it.
                QComboBox *typeCombo = nullptr;
                for (QComboBox *combo : m_vgBrowser->findChildren<QComboBox *>()) {
                    if (combo->findData(int(VgMacro::Square1)) >= 0)
                        typeCombo = combo;
                }
                const QString reversedName = vgMacroDisplayName(VgMacro::DirectSoundAlt);
                const struct {
                    QStringList words;
                    QString line;
                    int listed; // the reversed entry's macro; -1 when absent
                } typeCases[] = {
                    {{plainW, noResW, revW}, dsFixture, int(VgMacro::DirectSoundReverse)},
                    {{plainW, noResW, altW, revW}, dsFixture, int(VgMacro::DirectSoundAlt)},
                    {{plainW, noResW, altW}, dsFixture, int(VgMacro::DirectSoundAlt)},
                    {{}, dsFixture, int(VgMacro::DirectSoundAlt)},
                    {{plainW, noResW}, dsFixture, -1},
                    {{plainW, noResW, altW, revW},
                     QStringLiteral("voice_directsound_reverse 50, 70, %1, 250, 10, 200, 30")
                         .arg(sample),
                     int(VgMacro::DirectSoundReverse)},
                };
                if (check(typeCombo != nullptr, "no Type combo for the reverse-spelling checks")) {
                    for (const auto &tc : typeCases) {
                        if (!check(setFixture(tc.words, tc.line),
                                   "the Type dropdown fixture did not land"))
                            continue;
                        m_vgBrowser->selectSlot(dsSlot);
                        m_vgBrowser->selectSlot(pasteSlot);
                        int named = 0;
                        for (int i = 0; i < typeCombo->count(); i++)
                            named += typeCombo->itemText(i) == reversedName ? 1 : 0;
                        const int altIdx = typeCombo->findData(int(VgMacro::DirectSoundAlt));
                        const int revIdx = typeCombo->findData(int(VgMacro::DirectSoundReverse));
                        const int idx = tc.listed == int(VgMacro::DirectSoundAlt) ? altIdx : revIdx;
                        const bool listedOk =
                            tc.listed < 0 ? named == 0 && altIdx < 0 && revIdx < 0
                                          : named == 1 && idx >= 0 && (altIdx < 0 || revIdx < 0);
                        const QString words =
                            QStringLiteral("[%1]").arg(tc.words.join(QStringLiteral(", ")));
                        if (!check(listedOk,
                                   qPrintable(QStringLiteral("the Type dropdown's reversed entry "
                                                             "is wrong for macros %1 (%2 entries)")
                                                  .arg(words)
                                                  .arg(named))) ||
                            tc.listed < 0)
                            continue;
                        const VgVoice before = voiceAt(pasteSlot);
                        if (before.macro == VgMacro(tc.listed)) {
                            check(typeCombo->currentIndex() == idx,
                                  "the Type dropdown does not show a _reverse voice's entry");
                            continue;
                        }
                        typeCombo->setCurrentIndex(idx);
                        QMetaObject::invokeMethod(typeCombo, "activated", Qt::DirectConnection,
                                                  Q_ARG(int, idx));
                        check(voiceAt(pasteSlot).macro == VgMacro(tc.listed) &&
                                  stack->index() == baseIndex + 2,
                              qPrintable(QStringLiteral("the Type dropdown did not write %1 for "
                                                        "macros %2")
                                             .arg(vgMacroName(VgMacro(tc.listed)), words)));
                    }
                }
                unwind();
                m_vgBrowser->setSampleVariants(vgCatalog(tab->root).voiceMacroWords, cries);
                check(voiceAt(pasteSlot) == target, "the variant checks left the voice changed");
            }

            // View-only: Copy stays, Paste and Clear are disabled, and the
            // shortcut pushes nothing.
            m_vgBrowser->setViewOnly(true);
            clipboard->setText(sourceLine);
            items.clear();
            runMenu(pasteSlot, QString(), &items);
            check(items.value(tr("Copy")) && items.contains(tr("Paste")) &&
                      !items.value(tr("Paste")) && items.contains(tr("Clear")) &&
                      !items.value(tr("Clear")),
                  "view-only did not disable Paste/Clear (or disabled Copy)");
            sendKeys(pasteSlot, pasteKeys);
            check(stack->index() == baseIndex && voiceAt(pasteSlot) == target,
                  "the Paste shortcut edited a view-only voicegroup");
            m_vgBrowser->setViewOnly(false);
            check(voiceAt(dsSlot) == source && voiceAt(pasteSlot) == target,
                  "copy/paste checks left the voices changed");
        }
    }

    // 11. New… creates the voicegroup file AND assigns it to the current
    // song — the same undoable cfg edit the selector makes, so undo returns
    // to the previous voicegroup. The modal dialog is filled and accepted
    // from a timer inside its own exec() loop.
    {
        const QString argBefore = tab->doc.cfg().voicegroupArg;
        const QString newName = QStringLiteral("vgsavecheck_created");
        QTimer::singleShot(0, this, [this, newName] {
            for (QDialog *d : findChildren<QDialog *>()) {
                if (!d->isVisible() || d->windowTitle() != tr("New Voicegroup"))
                    continue;
                if (QLineEdit *edit = d->findChild<QLineEdit *>()) {
                    edit->setText(newName);
                    d->accept();
                } else {
                    d->reject(); // never hang the harness in exec()
                }
                return;
            }
        });
        newVoicegroup();
        check(QFile::exists(projectRoot + QStringLiteral("/sound/voicegroups/") + newName +
                            QStringLiteral(".inc")),
              "New… did not create the voicegroup file");
        check(tab->doc.cfg().voicegroupArg == QStringLiteral("_") + newName && tab->doc.isDirty(),
              "New… did not auto-assign the voicegroup as an undoable edit");
        check(tab->vgSource && tab->vgSource->filePath().endsWith(newName + QStringLiteral(".inc")),
              "New… auto-assign did not swap the voicegroup source");
        tab->doc.undoStack()->undo();
        check(tab->doc.cfg().voicegroupArg == argBefore && tab->vgSource &&
                  tab->vgSource->loadName() == vgLoadName,
              "undoing the New… auto-assign did not restore the voicegroup");
    }

    std::printf("vgsavecheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures == 0;
}

int runVgSaveCheck(const QString &projectRoot, const QString &songLabel,
                   const QString &screenshotPath)
{
    // Redirected settings: the user's real session is never touched.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "vgsavecheck: no temp dir for settings\n");
        return 1;
    }
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

    MainWindow window;
    return window.runVgSaveCheck(projectRoot, songLabel, screenshotPath) ? 0 : 1;
}
