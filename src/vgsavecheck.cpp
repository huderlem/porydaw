#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QTreeWidgetItemIterator>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

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

    // A DirectSound-family voice to edit (scalar fields + a sample symbol).
    int dsSlot = -1;
    for (int i = 0; i < VOICEGROUP_SIZE && dsSlot < 0; i++) {
        const VgVoice *v = tab->vgSource->voiceAt(i);
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
    check(readFileBytes(vgPath) == vgBytesOriginal,
          "voicegroup file changed without a save");

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
    check(readFileBytes(vgPath) != vgBytesOriginal,
          "save did not write the voicegroup file");
    check(readFileBytes(tab->doc.midPath()) != midBytesOriginal,
          "save did not write the .mid");
    {
        VoicegroupSource fresh;
        check(fresh.open(projectRoot, tab->doc.cfg().voicegroupArg, &error)
                  && fresh.voiceAt(dsSlot)
                  && fresh.voiceAt(dsSlot)->release == edited.release,
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
        SongCfg cfg = tab->doc.cfg();
        cfg.voicegroupArg = otherArg;
        tab->doc.setCfg(cfg);
        check(tab->vgSource && tab->vgSource->loadName() != vgLoadName,
              "-G switch did not swap the voicegroup source");
        tab->doc.undoStack()->undo(); // the -G switch
        check(tab->vgSource && tab->vgSource->loadName() == vgLoadName,
              "undoing the -G switch did not reopen the old voicegroup");
        check(tab->vgSource && tab->vgSource->voiceAt(dsSlot)
                  && tab->vgSource->voiceAt(dsSlot)->release == edited2.release
                  && tab->vgSource->dirty(),
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
        QComboBox *vgCombo =
            m_vgBrowser->findChild<QComboBox *>(QStringLiteral("vgArgCombo"));
        if (check(vgCombo != nullptr, "no voicegroup selector in the dock")) {
            const QString originalArg = tab->doc.cfg().voicegroupArg;
            const QString shown = SongRegistry::voicegroupDisplayName(
                originalArg.isEmpty() ? QStringLiteral("_dummy") : originalArg);
            check(vgCombo->isEnabled() && vgCombo->currentText() == shown,
                  "dock selector does not show the song's voicegroup");
            check(vgCombo->findText(SongRegistry::voicegroupDisplayName(otherArg))
                      >= 0,
                  "dock selector is missing a known voicegroup arg");
            vgCombo->setCurrentText(SongRegistry::voicegroupDisplayName(otherArg));
            QMetaObject::invokeMethod(vgCombo, "activated", Qt::DirectConnection,
                                      Q_ARG(int, 0));
            check(tab->doc.cfg().voicegroupArg == otherArg && tab->doc.isDirty(),
                  "dock selector did not commit an undoable -G switch");
            check(tab->vgSource && tab->vgSource->loadName() != vgLoadName,
                  "dock selector switch did not swap the voicegroup source");
            tab->doc.undoStack()->undo(); // the selector's -G switch
            check(tab->doc.cfg().voicegroupArg == originalArg
                      && !tab->doc.isDirty(),
                  "undoing the dock selector switch did not restore the cfg");
            check(vgCombo->currentText() == shown,
                  "undo did not refresh the dock selector's text");
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
            check(tab->doc.isDirty() && tab->vgSource->dirty()
                      && tab->vgSource->voiceAt(dsSlot)->release == uiValue,
                  "editing the Release spin box did not push an undo command");
            tab->doc.undoStack()->undo();
            check(!tab->doc.isDirty() && !tab->vgSource->dirty()
                      && releaseSpin->value() == original.release,
                  "undo did not refresh the Release spin box");
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
            QFile macros(projectRoot
                         + QStringLiteral("/asm/macros/vgsavecheck_synth.inc"));
            wrote = macros.open(QIODevice::WriteOnly)
                && macros.write("\t.macro set_synth_pulse base_duty=0x80, "
                                "duty_step=0x00, mod_depth=0x00, duty_phase=0x00\n"
                                "\t.endm\n"
                                "\t.macro set_synth_saw\n\t.endm\n"
                                "\t.macro set_synth_triangle\n\t.endm\n")
                    > 0;
        }
        {
            QFile data(synthPath);
            wrote = wrote && data.open(QIODevice::WriteOnly | QIODevice::Append)
                && data.write("\n\t.align 2\nVgSaveCheckSaw::\n\tset_synth_saw\n")
                    > 0;
        }
        if (!check(wrote, "cannot write synth support files"))
            return false;
        invalidateVgCatalog();
        updateVoicegroupBrowser(); // hand the browser the new catalog
        const VgSynthCatalog setupCatalog =
            VoicegroupSource::synthInstruments(projectRoot);
        const int defsAfterSetup = setupCatalog.defs.size();
        const QByteArray synthBytesSetup = readFileBytes(synthPath);
        const int indexBeforeSynth = tab->doc.undoStack()->index();

        // A sample voice that is NOT already a synth (a synth-heavy
        // voicegroup's first DirectSound voice may be one, and switching it
        // to Synth would rightly be a no-op).
        int synthSlot = -1;
        for (int i = 0; i < VOICEGROUP_SIZE && synthSlot < 0; i++) {
            const VgVoice *v = tab->vgSource->voiceAt(i);
            if (v
                && (v->macro == VgMacro::DirectSound
                    || v->macro == VgMacro::DirectSoundNoResample
                    || v->macro == VgMacro::DirectSoundAlt)
                && !setupCatalog.find(v->symbol))
                synthSlot = i;
        }
        if (synthSlot < 0) {
            std::printf("vgsavecheck: note: every sample voice is already a "
                        "synth, synth section skipped\n");
            std::printf("vgsavecheck: %s (%d failures)\n",
                        failures ? "FAIL" : "PASS", failures);
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
            if (combo->isEditable()
                && combo->objectName() != QLatin1String("vgArgCombo"))
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
        if (check(typeCombo && symbolCombo && waveCombo && dutySpin && stepSpin
                      && depthSpin && phaseSpin,
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
            check(tab->vgSource->voiceAt(synthSlot)->symbol
                      != synthOriginal.symbol,
                  "switching the voice to Synth did not take");
            // Dial a known pulse through several commits (each one mints).
            activate(waveCombo, 0);
            dutySpin->setValue(0x21);
            stepSpin->setValue(0x43);
            depthSpin->setValue(0x65);
            phaseSpin->setValue(0x87);
            const QString wantSymbol =
                vgSynthSymbolName(VgSynthDesc{0, 0x21, 0x43, 0x65, 0x87});
            check(tab->vgSource->voiceAt(synthSlot)->symbol == wantSymbol,
                  "param edits did not land on the param-named symbol");
            check(readFileBytes(synthPath) == synthBytesSetup,
                  "a param edit wrote to the synth data file before save");
            bool listed = false;
            for (int i = 0; i < symbolCombo->count(); i++)
                listed = listed || symbolCombo->itemText(i) == wantSymbol;
            check(!listed, "an unsaved definition appeared in the dropdown");
            const ToneData &td = m_audio.voicegroup()->voices[synthSlot];
            check(td.wav && td.wav->size == 0
                      && uint8_t(td.wav->data[1]) == 0
                      && uint8_t(td.wav->data[2]) == 0x21
                      && uint8_t(td.wav->data[5]) == 0x87,
                  "param edits were not patched into the loaded tone");
            // The scalar path renames the voice too (param-named symbols):
            // voiceNames feeds track labels and the browser tree.
            check(QString::fromUtf8(
                      m_audio.voicegroup()->voiceNames[synthSlot])
                      == wantSymbol,
                  "param edits did not sync the loaded voice name");

            // Save: exactly one definition (the referenced one) is written,
            // it now shows up in the dropdown, and the synth data file is
            // wired into the build (sound_data.s .include).
            check(saveSession(*tab), "synth save failed");
            check(readFileBytes(synthPath).contains(wantSymbol.toUtf8() + "::"),
                  "save did not write the referenced synth definition");
            bool wired = false;
            for (const QString &dir :
                 {projectRoot + QStringLiteral("/data"), projectRoot}) {
                QDirIterator wiredIt(dir, {QStringLiteral("*.s")}, QDir::Files);
                while (wiredIt.hasNext() && !wired)
                    wired = readFileBytes(wiredIt.next())
                                .contains("direct_sound_synth_data.inc");
            }
            check(wired, "save did not wire the synth data file into the build");
            check(VoicegroupSource::synthInstruments(projectRoot).defs.size()
                      == defsAfterSetup + 1,
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
            const VgSynthCatalog sawCatalog =
                VoicegroupSource::synthInstruments(projectRoot);
            const VgSynthDesc *sawDef = sawCatalog.find(sawSymbol);
            check(sawDef && sawDef->waveform == 1,
                  "waveform flip to saw did not dedupe onto an on-disk saw");
            activate(waveCombo, 0);
            // Deduped onto an on-disk 50% pulse when the project has one,
            // minted under the param name otherwise — never the duty-0 name.
            const QString pulseSymbol = tab->vgSource->voiceAt(synthSlot)->symbol;
            const VgSynthCatalog pulseCatalog =
                VoicegroupSource::synthInstruments(projectRoot);
            const VgSynthDesc *pulseDef = pulseCatalog.find(pulseSymbol);
            check((pulseDef && *pulseDef == VgSynthDesc{})
                      || pulseSymbol == vgSynthSymbolName(VgSynthDesc{}),
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
    // song references render bold, revealTrackVoice raises the dock and
    // selects the track's current program, and document edits that add or
    // remove voice changes keep the marks in sync.
    {
        QTreeWidget *tree = m_vgBrowser->findChild<QTreeWidget *>();
        if (check(tree != nullptr, "voicegroup browser has no tree")) {
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
                marksOk = marksOk && item && item->font(0).bold();
            }
            check(marksOk, "used voices are not marked bold in the dock");
            check(unused < 0 || !tree->topLevelItem(unused)->font(0).bold(),
                  "an unused voice is marked bold");

            // Track-header path: the dock reappears and the track's current
            // program becomes the selected slot.
            m_vgDock->hide();
            const int prog = tab->view->currentProgram(track);
            check(prog >= 0, "track under test has no program");
            tab->view->revealTrackVoice(track);
            check(!m_vgDock->isHidden(), "reveal did not show the voicegroup dock");
            check(m_vgBrowser->currentSlot() == prog,
                  "reveal did not select the track's program");

            if (unused >= 0) {
                // Explicit-program path (the event list's context menu).
                tab->view->revealVoice(unused);
                check(m_vgBrowser->currentSlot() == unused,
                      "revealVoice did not select the requested slot");

                // A new voice change gains the mark; undoing it clears it.
                tab->doc.addLanePoint(track, DOC_CC_VOICE, 480, unused);
                check(tree->topLevelItem(unused)->font(0).bold(),
                      "a new voice change did not gain the used mark");
                tab->doc.undoStack()->undo();
                check(!tree->topLevelItem(unused)->font(0).bold(),
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
        QComboBox *vgCombo =
            m_vgBrowser->findChild<QComboBox *>(QStringLiteral("vgArgCombo"));
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
                                      QPointF(row->mapToGlobal(pos)),
                                      Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
                    QCoreApplication::sendEvent(row, &press);
                    check(!row.isNull(),
                          "header rebuild freed the row inside its own press");
                    check(tab->view->selectedTrack() == otherTrack,
                          "the header click did not select its track");
                    check(tab->doc.cfg().voicegroupArg == otherArg,
                          "the mid-press -G edit did not commit");
                    if (!row.isNull()) {
                        QMouseEvent release(QEvent::MouseButtonRelease,
                                            QPointF(pos),
                                            QPointF(row->mapToGlobal(pos)),
                                            Qt::LeftButton, Qt::NoButton,
                                            Qt::NoModifier);
                        QCoreApplication::sendEvent(row, &release);
                    }
                }
                QCoreApplication::processEvents(); // deferred row deletion
                // The rebuilt panel is functional: its fresh rows select.
                QWidget *fresh = tab->view->findChild<QWidget *>(
                    QStringLiteral("trackHeaderRow%1").arg(track));
                if (check(fresh != nullptr, "no rebuilt header row")
                    && track != otherTrack) {
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(QPoint(5, 5)),
                                      QPointF(fresh->mapToGlobal(QPoint(5, 5))),
                                      Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
                    QCoreApplication::sendEvent(fresh, &press);
                    QMouseEvent release(QEvent::MouseButtonRelease,
                                        QPointF(QPoint(5, 5)),
                                        QPointF(fresh->mapToGlobal(QPoint(5, 5))),
                                        Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                    QCoreApplication::sendEvent(fresh, &release);
                    QCoreApplication::processEvents();
                    check(tab->view->selectedTrack() == track,
                          "a rebuilt header row did not select its track");
                }
                tab->doc.undoStack()->undo(); // the mid-press -G switch
                check(tab->doc.cfg().voicegroupArg == argBefore
                          && tab->vgSource
                          && tab->vgSource->loadName() == vgLoadName,
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
        auto *picker = m_vgBrowser->findChild<SamplePickerButton *>(
            QStringLiteral("vgSamplePickerButton"));
        QComboBox *symbolCombo = nullptr;
        for (QComboBox *combo : m_vgBrowser->findChildren<QComboBox *>()) {
            if (combo->isEditable()
                && combo->objectName() != QLatin1String("vgArgCombo"))
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
            QMetaObject::Connection c1 = connect(
                m_vgBrowser, &VoicegroupBrowser::sampleAuditionRequested, this,
                [&auditioned, &auditionKinds](const QString &s,
                                              VgAuditionKind kind,
                                              const AuditionSlots::Adsr &) {
                    auditioned.append(s);
                    auditionKinds.append(kind);
                });
            QMetaObject::Connection c2 = connect(
                m_vgBrowser, &VoicegroupBrowser::sampleAuditionStopRequested,
                this, [&stops] { stops++; });

            picker->openPopup();
            auto *search = picker->findChild<QLineEdit *>(
                QStringLiteral("vgSamplePickerSearch"));
            auto *list = picker->findChild<QTreeWidget *>(
                QStringLiteral("vgSamplePickerList"));
            if (check(search && list && picker->popupVisible(),
                      "picker popup did not open")) {
                // The popup floats over the browser, so it carries the menu
                // outline; its corner pixel is that border.
                QWidget *popupFrame = picker->findChild<QWidget *>(
                    QStringLiteral("vgSamplePickerPopup"));
                check(popupFrame
                          && popupFrame->grab().toImage().pixelColor(0, 0)
                              == themes::color(themes::Role::menu_outline),
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
                    check(auditioned.size() == seen + 1
                              && auditionKinds.last()
                                  == VgAuditionKind::Keysplit,
                          "keysplit row did not audition as a keysplit");
                } else {
                    std::printf("vgsavecheck: note: no keysplit instruments, "
                                "keysplit audition skipped\n");
                }

                if (!screenshotPath.isEmpty()) {
                    QWidget *popup = picker->findChild<QWidget *>(
                        QStringLiteral("vgSamplePickerPopup"));
                    check(popup && popup->grab().save(screenshotPath),
                          "could not save the picker screenshot");
                    // A -dock variant of the browser itself: the editor
                    // panel's Sample row (picker + glyph tool buttons).
                    const QFileInfo info(screenshotPath);
                    check(m_vgBrowser->grab().save(
                              info.path() + QLatin1Char('/')
                              + info.completeBaseName()
                              + QStringLiteral("-dock.") + info.suffix()),
                          "could not save the dock screenshot");
                }

                // Pick a different plain sample by typing its full symbol:
                // the filter narrows onto it, highlighting auditions it, and
                // Return commits it as an undoable voice edit.
                QString target;
                for (QTreeWidgetItemIterator it(list); *it && target.isEmpty();
                     ++it) {
                    const QString s = (*it)->data(0, Qt::UserRole).toString();
                    if (!s.isEmpty() && s != before.symbol
                        && !(*it)->data(0, Qt::UserRole + 1).toBool())
                        target = s;
                }
                if (check(!target.isEmpty(), "no alternate sample to pick")) {
                    search->setText(target);
                    check(auditioned.contains(target),
                          "filtering onto a sample did not audition it");
                    QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return,
                                  Qt::NoModifier);
                    QCoreApplication::sendEvent(search, &ret);
                    check(!picker->popupVisible(),
                          "committing a pick did not close the popup");
                    check(stops > 0, "closing the popup did not stop audition");
                    check(tab->vgSource->voiceAt(dsSlot)
                              && tab->vgSource->voiceAt(dsSlot)->symbol == target,
                          "the picked symbol did not commit");
                    check(tab->doc.isDirty(),
                          "the picked symbol did not push an undo command");
                    tab->doc.undoStack()->undo();
                    check(tab->vgSource->voiceAt(dsSlot)
                              && tab->vgSource->voiceAt(dsSlot)->symbol
                                  == before.symbol,
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
                check(tab->vgSource->voiceAt(dsSlot)
                          && tab->vgSource->voiceAt(dsSlot)->symbol == unlisted,
                      "an unlisted typed symbol did not commit");
                tab->doc.undoStack()->undo();
                check(tab->vgSource->voiceAt(dsSlot)
                          && tab->vgSource->voiceAt(dsSlot)->symbol
                              == before.symbol,
                      "undo did not restore the unlisted symbol");

                // Wave voices share the picker: switching the Type swaps the
                // list to the project's programmable waves, and highlighting
                // one auditions it as a CGB wave.
                if (vgCatalog().progWave.isEmpty()) {
                    std::printf("vgsavecheck: note: no programmable waves, "
                                "wave picker section skipped\n");
                } else {
                    QComboBox *typeCombo = nullptr;
                    for (QComboBox *combo :
                         m_vgBrowser->findChildren<QComboBox *>()) {
                        if (combo->findData(int(VgMacro::ProgWave)) >= 0)
                            typeCombo = combo;
                    }
                    int undos = 0;
                    if (check(typeCombo != nullptr, "no Type combo")) {
                        const int waveIndex =
                            typeCombo->findData(int(VgMacro::ProgWave));
                        typeCombo->setCurrentIndex(waveIndex);
                        QMetaObject::invokeMethod(typeCombo, "activated",
                                                  Qt::DirectConnection,
                                                  Q_ARG(int, waveIndex));
                        undos++;
                        const VgVoice *waveVoice =
                            tab->vgSource->voiceAt(dsSlot);
                        check(waveVoice
                                  && waveVoice->macro == VgMacro::ProgWave,
                              "type switch to Prog Wave did not take");
                        check(picker->isVisible()
                                  && picker->currentSymbol()
                                      == waveVoice->symbol,
                              "wave voice does not show the picker");

                        picker->openPopup();
                        QString otherWave;
                        for (QTreeWidgetItemIterator it(list);
                             *it && otherWave.isEmpty(); ++it) {
                            const QString s =
                                (*it)->data(0, Qt::UserRole).toString();
                            if (!s.isEmpty() && s != waveVoice->symbol)
                                otherWave = s;
                        }
                        for (QTreeWidgetItemIterator it(list); *it; ++it) {
                            const QString s =
                                (*it)->data(0, Qt::UserRole).toString();
                            check(s.isEmpty()
                                      || vgCatalog().progWave.contains(s),
                                  "wave picker lists a non-wave symbol");
                            // Waves show the verbatim symbol (samples strip
                            // their shared prefix; waves don't).
                            check(s.isEmpty() || (*it)->text(0) == s,
                                  "wave row does not show its full symbol");
                        }
                        if (otherWave.isEmpty()) {
                            std::printf("vgsavecheck: note: single wave, "
                                        "wave audition/pick skipped\n");
                            QWidget *popup = picker->findChild<QWidget *>(
                                QStringLiteral("vgSamplePickerPopup"));
                            if (popup)
                                popup->hide();
                        } else {
                            const int seen = auditioned.size();
                            search->setText(otherWave);
                            check(auditioned.size() > seen
                                      && auditioned.last() == otherWave
                                      && auditionKinds.last()
                                          == VgAuditionKind::Wave,
                                  "filtering onto a wave did not audition "
                                  "it as a wave");
                            QKeyEvent waveRet(QEvent::KeyPress, Qt::Key_Return,
                                              Qt::NoModifier);
                            QCoreApplication::sendEvent(search, &waveRet);
                            undos++;
                            check(tab->vgSource->voiceAt(dsSlot)
                                      && tab->vgSource->voiceAt(dsSlot)->symbol
                                          == otherWave,
                              "the picked wave did not commit");
                        }
                        while (undos-- > 0)
                            tab->doc.undoStack()->undo();
                        const VgVoice *restored =
                            tab->vgSource->voiceAt(dsSlot);
                        check(restored && restored->macro == before.macro
                                  && restored->symbol == before.symbol,
                              "undo did not restore the sample voice after "
                              "the wave round trip");
                    }
                }
            }
            disconnect(c1);
            disconnect(c2);
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
        check(QFile::exists(projectRoot + QStringLiteral("/sound/voicegroups/")
                            + newName + QStringLiteral(".inc")),
              "New… did not create the voicegroup file");
        check(tab->doc.cfg().voicegroupArg == QStringLiteral("_") + newName
                  && tab->doc.isDirty(),
              "New… did not auto-assign the voicegroup as an undoable edit");
        check(tab->vgSource
                  && tab->vgSource->filePath().endsWith(
                      newName + QStringLiteral(".inc")),
              "New… auto-assign did not swap the voicegroup source");
        tab->doc.undoStack()->undo();
        check(tab->doc.cfg().voicegroupArg == argBefore && tab->vgSource
                  && tab->vgSource->loadName() == vgLoadName,
              "undoing the New… auto-assign did not restore the voicegroup");
    }

    std::printf("vgsavecheck: %s (%d failures)\n", failures ? "FAIL" : "PASS",
                failures);
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
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                       settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());

    MainWindow window;
    return window.runVgSaveCheck(projectRoot, songLabel, screenshotPath) ? 0 : 1;
}
