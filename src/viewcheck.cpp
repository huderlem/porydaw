#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cstdio>

#include "core/miditimeline.h"
#include "project/decompproject.h"
#include "ui/songview.h"
#include "ui/songviewmodel.h"

// --viewcheck <projectRoot>: M1 acceptance check. For every song with a MIDI
// source, build the view model and verify that every timeline event lands in
// exactly one presentation bucket (piano roll, automation lane, voice marker,
// tempo lane, or the other-events strip) — i.e. nothing is silently
// invisible — then render the SongView offscreen as a paint smoke test.
int runViewCheck(const QString &projectRoot, const QString &screenshotSong,
                 const QString &screenshotPath)
{
    DecompProject project;
    QString error;
    if (!project.open(projectRoot, &error)) {
        std::fprintf(stderr, "viewcheck: %s\n", qUtf8Printable(error));
        return 1;
    }

    QElapsedTimer timer;
    timer.start();

    SongView view;
    view.resize(1280, 800);

    int checked = 0;
    int failures = 0;
    bool gridChecked = false;
    size_t totalEvents = 0, totalNotes = 0, totalLanePoints = 0, totalStrip = 0;
    int songsWithUnpaired = 0, songsWithOrphans = 0, songsWithDropped = 0;

    for (const SongInfo &song : project.songs()) {
        if (!song.isPlayable())
            continue;
        auto tl = MidiTimeline::load(song.midPath, 48000.0, &error);
        if (!tl) {
            std::fprintf(stderr, "viewcheck: FAIL %s: %s\n", qUtf8Printable(song.label),
                         qUtf8Printable(error));
            failures++;
            continue;
        }

        const SongViewModel model = buildSongViewModel(*tl);

        // Bucket-sum accounting: every timeline event must be presented
        // exactly once. Orphan note-offs land in the strip, so they are
        // covered by stripFromEvents rather than counted separately.
        const size_t tempoEvents =
            size_t(std::count_if(tl->events.begin(), tl->events.end(),
                                 [](const TimelineEvent &ev) {
                                     return ev.type == TIMELINE_EVT_TEMPO;
                                 }));
        size_t lanePoints = 0;
        for (const AutoLane &lane : model.lanes)
            lanePoints += lane.points.size();
        const size_t stripFromEvents = model.strip.size() - tl->otherEvents.size();
        const size_t pairedOffs = model.notes.size() - model.unpairedNoteOns;
        const size_t bucketSum = model.notes.size() + pairedOffs + lanePoints
            + model.voices.size() + tempoEvents + stripFromEvents;
        if (bucketSum != tl->events.size()) {
            std::fprintf(stderr,
                         "viewcheck: FAIL %s: %zu events but %zu presented "
                         "(some events would be invisible)\n",
                         qUtf8Printable(song.label), tl->events.size(), bucketSum);
            failures++;
        }

        // Paint smoke test: exercise every used track's lane set, then grab.
        view.setSong(tl.get(), nullptr);
        for (int t = 0; t < 16; t++)
            if (tl->tracks[t].used)
                view.selectTrack(t);
        view.setPlayheadSample(tl->lengthSamples / 2, true);
        const QImage image = view.grab().toImage();
        if (image.isNull()) {
            std::fprintf(stderr, "viewcheck: FAIL %s: offscreen render produced no image\n",
                         qUtf8Printable(song.label));
            failures++;
        }
        if (song.label == screenshotSong && !screenshotPath.isEmpty()) {
            image.save(screenshotPath);
            std::printf("viewcheck: wrote %s\n", qUtf8Printable(screenshotPath));
        }

        const uint64_t tpb = std::max<uint32_t>(1, tl->ticksPerBeat);
        if (!gridChecked && view.gridSegAt(0).beatTicks == tpb) {
            // Snap-grid semantics on the first song governed by a /4
            // signature at tick 0: at a fixed 64 px/beat zoom the grid must
            // follow the feel's subdivision ladder and the minimum-
            // subdivision floor. No document is attached, so the clock
            // floor is 1 tick.
            gridChecked = true;
            SongView::ViewState zoom;
            zoom.valid = true;
            zoom.pxPerBeat = 64.0;
            view.applyViewState(zoom);
            const auto expectGrid = [&](const char *what, uint64_t expected) {
                if (view.gridTicksAt(0) != expected) {
                    std::fprintf(stderr,
                                 "viewcheck: FAIL %s: %s grid = %llu ticks, "
                                 "expected %llu\n",
                                 qUtf8Printable(song.label), what,
                                 (unsigned long long)view.gridTicksAt(0),
                                 (unsigned long long)expected);
                    failures++;
                }
            };
            expectGrid("straight auto", std::max<uint64_t>(1, tpb / 8));
            view.setGridFeel(SongView::GridFeel::Triplet);
            expectGrid("triplet auto", std::max<uint64_t>(1, tpb / 6));
            view.setGridMinDenom(8);
            expectGrid("triplet 1/8", std::max<uint64_t>(1, tpb / 3));
            view.setGridFeel(SongView::GridFeel::Straight);
            view.setGridMinDenom(16);
            expectGrid("straight 1/16", std::max<uint64_t>(1, tpb / 4));
            view.setGridMinDenom(4);
            expectGrid("straight 1/4", tpb);
            view.setGridMinDenom(0);
            std::printf("viewcheck: snap-grid semantics checked on %s\n",
                        qUtf8Printable(song.label));
        }

        // Every drawn beat/bar line must be reachable by snapping, whatever
        // the song's time-signature changes do (mid-measure signatures
        // restart the grid; a signature's denominator rescales the beat).
        // With the 1/4 floor the snap grid IS the beat grid, so each line
        // must be a snap fixed point.
        view.setGridMinDenom(4);
        int badSnaps = 0;
        uint64_t badTick = 0;
        view.forEachGridLine(0, tl->lengthTicks, [&](uint64_t t, bool, int) {
            if (view.snapTick(double(t)) != t && badSnaps++ == 0)
                badTick = t;
        });
        view.setGridMinDenom(0);
        if (badSnaps) {
            std::fprintf(stderr,
                         "viewcheck: FAIL %s: %d grid line(s) not snappable "
                         "(first at tick %llu)\n",
                         qUtf8Printable(song.label), badSnaps,
                         (unsigned long long)badTick);
            failures++;
        }
        view.setSong(nullptr, nullptr);

        checked++;
        totalEvents += tl->events.size();
        totalNotes += model.notes.size();
        totalLanePoints += lanePoints;
        totalStrip += model.strip.size();
        if (model.unpairedNoteOns) {
            songsWithUnpaired++;
            std::fprintf(stderr, "viewcheck: note %s: %zu unpaired note-on(s)\n",
                         qUtf8Printable(song.label), model.unpairedNoteOns);
        }
        if (model.orphanNoteOffs) {
            songsWithOrphans++;
            std::fprintf(stderr, "viewcheck: note %s: %zu orphan note-off(s)\n",
                         qUtf8Printable(song.label), model.orphanNoteOffs);
        }
        if (tl->droppedTracks)
            songsWithDropped++;
    }

    std::printf("viewcheck: %d songs in %lld ms — %zu events -> %zu notes, %zu lane "
                "points, %zu strip items\n",
                checked, (long long)timer.elapsed(), totalEvents, totalNotes,
                totalLanePoints, totalStrip);
    if (songsWithUnpaired || songsWithOrphans || songsWithDropped)
        std::printf("viewcheck: quirks — %d songs with unpaired note-ons, %d with orphan "
                    "note-offs, %d with dropped tracks (all still displayed)\n",
                    songsWithUnpaired, songsWithOrphans, songsWithDropped);
    std::printf("viewcheck: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
