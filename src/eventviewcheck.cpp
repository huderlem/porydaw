#include <QAbstractItemModel>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QTableView>
#include <algorithm>
#include <cstdio>

#include "core/songdocument.h"
#include "project/decompproject.h"
#include "ui/songview.h"

// --eventviewcheck <projectRoot>: MIDI event list check. Model pass over
// every song with a MIDI source: the raw-event API (insert, same-tick
// modify, tick-moving modify, delete, end-of-track move with its clamp)
// keeps event ticks sorted and undoes back to the original bytes. UI pass
// on the first song: the event list swaps in for the roll, mirrors the
// chunk's events plus an end-of-track row, routes cell edits into the
// document (queued), filters, and round-trips through view state.

namespace {

bool trackSorted(const SmfTrack &track)
{
    for (size_t i = 1; i < track.events.size(); i++) {
        if (track.events[i].tick < track.events[i - 1].tick)
            return false;
    }
    return true;
}

// The chunk the check edits: the first engine track's, else the first chunk
// holding any event. -1 when the file is empty.
int pickChunk(const SongDocument &doc)
{
    const int mapped = doc.smfTrackFor(0);
    if (mapped >= 0)
        return mapped;
    for (size_t t = 0; t < doc.smf().tracks.size(); t++) {
        if (!doc.smf().tracks[t].events.empty())
            return int(t);
    }
    return doc.smf().tracks.empty() ? -1 : 0;
}

size_t countMatching(const SmfTrack &track, const SmfEvent &target)
{
    return size_t(std::count(track.events.begin(), track.events.end(), target));
}

long long indexOf(const SmfTrack &track, const SmfEvent &target)
{
    const auto it = std::find(track.events.begin(), track.events.end(), target);
    return it == track.events.end() ? -1 : (long long)(it - track.events.begin());
}

int runUiPass(const SongInfo &song, const QString &screenshotPath)
{
    QString error;
    SongDocument doc;
    if (!doc.load(song, &error)) {
        std::fprintf(stderr, "eventviewcheck: FAIL ui %s: %s\n",
                     qUtf8Printable(song.label), qUtf8Printable(error));
        return 1;
    }
    auto tl = doc.buildTimeline(48000.0);

    SongView view;
    view.resize(1280, 800);
    view.setSong(tl.get(), nullptr);
    view.setDocument(&doc);

    int failures = 0;
    auto fail = [&](const char *what) {
        std::fprintf(stderr, "eventviewcheck: FAIL ui %s: %s\n",
                     qUtf8Printable(song.label), what);
        failures++;
    };

    if (view.eventListVisible())
        fail("event list visible before the toggle");
    view.setEventListVisible(true);
    if (!view.eventListVisible())
        fail("event list not visible after the toggle");

    auto *table = view.findChild<QTableView *>(QStringLiteral("eventListTable"));
    auto *chunkCombo = view.findChild<QComboBox *>(QStringLiteral("eventListChunk"));
    auto *filterCombo = view.findChild<QComboBox *>(QStringLiteral("eventListFilter"));
    if (!table || !chunkCombo || !filterCombo) {
        fail("event list widgets not found");
        return failures;
    }
    QAbstractItemModel *model = table->model();
    const int chunk = chunkCombo->currentData().toInt();
    if (chunk < 0 || chunk >= int(doc.smf().tracks.size())) {
        fail("no chunk selected");
        return failures;
    }
    const int selectedChunk = doc.smfTrackFor(view.selectedTrack());
    if (selectedChunk >= 0 && selectedChunk != chunk)
        fail("chunk combo does not follow the selected track");

    const SmfTrack &track = doc.smf().tracks[chunk];
    if (model->rowCount() != int(track.events.size()) + 1)
        fail("row count != chunk events + end-of-track row");
    const QModelIndex eotIdx = model->index(model->rowCount() - 1, 0);
    if (model->data(eotIdx, Qt::EditRole).toULongLong() != track.endTick)
        fail("end-of-track row shows the wrong tick");

    // Cell edit: bump the first event's tick. The mutation is queued (must
    // not reset the model inside the delegate's commit), so pump the loop.
    if (!track.events.empty()) {
        const qulonglong newTick = qulonglong(track.events[0].tick + 3);
        if (!model->setData(model->index(0, 0), newTick, Qt::EditRole))
            fail("tick cell rejected the edit");
        QCoreApplication::processEvents();
        if (!doc.undoStack()->canUndo())
            fail("tick edit never reached the document");
        if (!trackSorted(doc.smf().tracks[chunk]))
            fail("events unsorted after the tick edit");
        if (model->rowCount() != int(doc.smf().tracks[chunk].events.size()) + 1)
            fail("table did not refresh after the tick edit");
        doc.undoStack()->undo();
        if (model->rowCount() != int(doc.smf().tracks[chunk].events.size()) + 1)
            fail("table did not refresh after undo");

        // 64-bit ticks must not squeeze through an int anywhere in the edit
        // path (the event moves past everything, so it lands last).
        const qulonglong bigTick = 3000000000ULL; // > INT_MAX
        model->setData(model->index(0, 0), bigTick, Qt::EditRole);
        QCoreApplication::processEvents();
        const auto &evs = doc.smf().tracks[chunk].events;
        if (evs.empty() || evs.back().tick != bigTick)
            fail("a >INT_MAX tick edit did not land exactly");
        doc.undoStack()->undo();
    }

    // Filter: the Meta entry must show exactly the chunk's meta events.
    const size_t metas = size_t(std::count_if(
        doc.smf().tracks[chunk].events.begin(), doc.smf().tracks[chunk].events.end(),
        [](const SmfEvent &ev) { return ev.isMeta(); }));
    filterCombo->setCurrentIndex(7); // FilterMeta
    if (model->rowCount() != int(metas) + 1)
        fail("meta filter shows the wrong row count");
    filterCombo->setCurrentIndex(0); // FilterAll

    const QImage image = view.grab().toImage();
    if (image.isNull())
        fail("offscreen render produced no image");
    if (!screenshotPath.isEmpty()) {
        image.save(screenshotPath);
        std::printf("eventviewcheck: wrote %s\n", qUtf8Printable(screenshotPath));
    }

    // View-state round trip carries the mode both ways.
    SongView::ViewState state = view.viewState();
    if (!state.eventList)
        fail("viewState does not record the event list mode");
    state.eventList = false;
    view.applyViewState(state);
    if (view.eventListVisible())
        fail("applyViewState did not restore the roll");

    view.setDocument(nullptr);
    view.setSong(nullptr, nullptr);
    return failures;
}

} // namespace

int runEventViewCheck(const QString &projectRoot, const QString &screenshotSong,
                      const QString &screenshotPath)
{
    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "eventviewcheck: %s\n", qUtf8Printable(error));
        return 1;
    }

    QElapsedTimer timer;
    timer.start();

    int checked = 0, failures = 0;
    bool uiChecked = false;
    for (const SongInfo &song : project.songs()) {
        if (!song.isPlayable())
            continue;

        SongDocument doc;
        if (!doc.load(song, &error)) {
            std::fprintf(stderr, "eventviewcheck: FAIL %s: %s\n",
                         qUtf8Printable(song.label), qUtf8Printable(error));
            failures++;
            continue;
        }
        const QByteArray baseline = doc.smf().write();
        const int chunk = pickChunk(doc);
        if (chunk < 0)
            continue;

        auto fail = [&](const char *what) {
            std::fprintf(stderr, "eventviewcheck: FAIL %s: %s\n",
                         qUtf8Printable(song.label), what);
            failures++;
        };
        const auto &track = doc.smf().tracks[chunk];

        uint8_t channel = 0;
        for (const SmfEvent &ev : track.events) {
            if (ev.isChannel()) {
                channel = ev.channel();
                break;
            }
        }
        uint64_t base = 0;
        for (const SmfTrack &t : doc.smf().tracks)
            base = std::max(base, t.endTick);
        base += 100;

        const size_t before = track.events.size();
        bool ok = true;

        // Insert: appended past everything, and the EOT follows it out.
        SmfEvent ev;
        ev.tick = base;
        ev.status = uint8_t(0xB0 | channel);
        ev.data0 = 7;
        ev.data1 = 64;
        doc.insertRawEvent(chunk, ev);
        if (track.events.size() != before + 1 || !trackSorted(track)
            || countMatching(track, ev) != 1 || track.endTick != base) {
            fail("insertRawEvent produced wrong content");
            ok = false;
        }

        // Same-tick modify: in place, no reorder.
        if (ok) {
            const long long idx = indexOf(track, ev);
            SmfEvent edited = ev;
            edited.data1 = 99;
            doc.modifyRawEvent(chunk, size_t(idx), edited);
            if (indexOf(track, edited) != idx || !trackSorted(track)) {
                fail("same-tick modifyRawEvent produced wrong content");
                ok = false;
            }
            ev = edited;
        }

        // Tick-moving modify: re-inserted at the new position, still sorted.
        if (ok) {
            const long long idx = indexOf(track, ev);
            SmfEvent moved = ev;
            moved.tick = 0;
            const size_t movedBefore = countMatching(track, moved);
            doc.modifyRawEvent(chunk, size_t(idx), moved);
            if (countMatching(track, moved) != movedBefore + 1
                || !trackSorted(track) || track.events.size() != before + 1) {
                fail("tick-moving modifyRawEvent produced wrong content");
                ok = false;
            }
            ev = moved;
        }

        // Delete brings the chunk back to its original event count.
        if (ok) {
            const size_t had = countMatching(track, ev);
            doc.deleteRawEvents(chunk, {size_t(indexOf(track, ev))});
            if (track.events.size() != before || countMatching(track, ev) != had - 1) {
                fail("deleteRawEvents produced wrong content");
                ok = false;
            }
        }

        // End-of-track moves freely forward but clamps at the last event.
        if (ok) {
            doc.setTrackEndTick(chunk, base + 500);
            if (track.endTick != base + 500) {
                fail("setTrackEndTick did not move the end");
                ok = false;
            }
            const uint64_t lastTick =
                track.events.empty() ? 0 : track.events.back().tick;
            doc.setTrackEndTick(chunk, 0);
            if (track.endTick != lastTick) {
                fail("setTrackEndTick not clamped at the last event");
                ok = false;
            }
        }

        // Undo everything: byte-identical; redo deterministic. (The script
        // is net-zero — insert, move, delete, EOT clamped back — so the
        // redone bytes are compared against the captured edited state, not
        // against "anything but the baseline".)
        const QByteArray edited = doc.smf().write();
        while (doc.undoStack()->canUndo())
            doc.undoStack()->undo();
        if (doc.smf().write() != baseline) {
            fail("undo-all did not restore the original bytes");
        } else {
            while (doc.undoStack()->canRedo())
                doc.undoStack()->redo();
            const QByteArray redone = doc.smf().write();
            while (doc.undoStack()->canUndo())
                doc.undoStack()->undo();
            if (doc.smf().write() != baseline)
                fail("undo after redo did not restore the original bytes");
            else if (redone != edited)
                fail("redo-all did not reproduce the edited state");
        }

        // The UI pass runs once — on the named screenshot song, else the
        // first playable one.
        if (song.label == screenshotSong) {
            uiChecked = true;
            failures += runUiPass(song, screenshotPath);
        } else if (!uiChecked && screenshotSong.isEmpty()) {
            uiChecked = true;
            failures += runUiPass(song, QString());
        }
        checked++;
    }

    std::printf("eventviewcheck: %d songs in %lld ms\n", checked,
                (long long)timer.elapsed());
    std::printf("eventviewcheck: %s (%d failures)\n", failures ? "FAIL" : "PASS",
                failures);
    return failures ? 1 : 0;
}
