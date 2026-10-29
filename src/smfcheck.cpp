#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>
#include <cstdint>
#include <cstdio>

#include "core/smf.h"
#include "core/songdocument.h"
#include "project/decompproject.h"

extern "C" {
#include "m4a_engine.h"
}

// --smfcheck: SMF parse-validation + note-pairing check, fully self-contained
// (no project needed). Covers the hostile-MIDI hardening properties:
//
//  1. SmfFile::read consumes channel-event data bytes exactly as mid2agb
//     does — bit 7 preserved, per-event sizes kept, so the stream never
//     desyncs and a load -> save round-trip stays byte-faithful. Real
//     exporters emit values like CC volume 0x80 in files the decomp builds,
//     so out-of-range data must survive parsing; it is neutralized at the
//     engine boundary instead (property 3).
//  2. SongDocument::notesForTrack pairs notes in linear time while keeping
//     mid2agb's exact rule — first same-channel same-key note end after the
//     note-on, shared ends and unterminated note-ons included — so a compact
//     file of unterminated note-ons cannot freeze the editor.
//  3. Defense in depth at the engine boundary: program/key values above 127
//     are ignored by the engine API instead of indexing its 128-entry
//     voicegroup, keysplit, and rhythm tables.

namespace {

void appendU32(QByteArray &out, uint32_t v)
{
    out.append(char(v >> 24));
    out.append(char(v >> 16));
    out.append(char(v >> 8));
    out.append(char(v));
}

void appendU16(QByteArray &out, uint16_t v)
{
    out.append(char(v >> 8));
    out.append(char(v));
}

// A single-chunk format-0 file around the given raw track body. Built by
// hand because these cases need byte sequences SmfFile::write can never
// produce.
QByteArray smfBytes(const QByteArray &trackBody)
{
    QByteArray out("MThd");
    appendU32(out, 6);
    appendU16(out, 0);  // format
    appendU16(out, 1);  // tracks
    appendU16(out, 24); // division
    out.append("MTrk");
    appendU32(out, uint32_t(trackBody.size()));
    out.append(trackBody);
    return out;
}

int checkEvent(const std::vector<SmfEvent> &evs, size_t n, uint8_t status, uint8_t data0,
               uint8_t data1)
{
    if (n >= evs.size()) {
        std::fprintf(stderr, "smfcheck: FAIL: event %zu missing\n", n);
        return 1;
    }
    const SmfEvent &ev = evs[n];
    if (ev.status != status || ev.data0 != data0 || ev.data1 != data1) {
        std::fprintf(stderr, "smfcheck: FAIL: event %zu = %02x %02x %02x, want %02x %02x %02x\n", n,
                     ev.status, ev.data0, ev.data1, status, data0, data1);
        return 1;
    }
    return 0;
}

SmfEvent channelEvent(uint64_t tick, uint8_t status, uint8_t data0, uint8_t data1)
{
    SmfEvent ev;
    ev.tick = tick;
    ev.status = status;
    ev.data0 = data0;
    ev.data1 = data1;
    return ev;
}

// Writes smf into dir and loads it back through SongDocument.
bool loadDocument(const SmfFile &smf, const QTemporaryDir &dir, const char *name, SongDocument *doc,
                  QString *error)
{
    SongInfo song;
    song.label = QString::fromLatin1(name);
    song.midPath = dir.filePath(QString::fromLatin1(name) + QStringLiteral(".mid"));
    song.hasMid = true;
    if (!smf.writeFile(song.midPath, error))
        return false;
    return doc->load(song, error);
}

int engineTrackForChunk(const SongDocument &doc, int chunk)
{
    for (int t = 0; t < doc.engineTrackCount(); t++) {
        if (doc.smfTrackFor(t) == chunk)
            return t;
    }
    return -1;
}

int checkNote(const std::vector<DocNote> &notes, size_t n, size_t onIndex, size_t endIndex,
              uint32_t duration, uint8_t key, uint8_t velocity, uint8_t channel)
{
    if (n >= notes.size()) {
        std::fprintf(stderr, "smfcheck: FAIL: note %zu missing\n", n);
        return 1;
    }
    const DocNote &note = notes[n];
    if (note.onIndex != onIndex || note.endIndex != endIndex || note.duration != duration ||
        note.key != key || note.velocity != velocity || note.channel != channel) {
        std::fprintf(stderr,
                     "smfcheck: FAIL: note %zu = on %zu end %zu dur %u key %u vel %u ch %u, "
                     "want on %zu end %zu dur %u key %u vel %u ch %u\n",
                     n, note.onIndex, note.endIndex, note.duration, note.key, note.velocity,
                     note.channel, onIndex, endIndex, duration, key, velocity, channel);
        return 1;
    }
    return 0;
}

} // namespace

int runSmfCheck()
{
    int failures = 0;

    // 1. Well-formed events, including data bytes at the 127 boundary, parse.
    {
        SmfFile smf;
        QString error;
        const QByteArray body = QByteArray::fromHex("00903c40" // note on, key 60 vel 64
                                                    "10803c40" // note off
                                                    "00c07f"   // program change 127
                                                    "00ff2f00");
        if (!SmfFile::read(smfBytes(body), &smf, &error)) {
            std::fprintf(stderr, "smfcheck: FAIL: valid file rejected: %s\n",
                         qUtf8Printable(error));
            failures++;
        }
    }

    // 2. Data bytes with bit 7 set are consumed as data — value preserved,
    // stream alignment kept, never reinterpreted as a status byte — in every
    // position they can appear. (A high first data byte under running status
    // is impossible by construction: it would be a status byte.) The events
    // after each bad byte prove the reader never desynced.
    {
        SmfFile smf;
        QString error;
        const QByteArray body = QByteArray::fromHex("00c080"   // program change, data0 0x80
                                                    "00903c40" // note on, aligned after it
                                                    "00b00780" // CC 7 = 0x80 (real-world shape)
                                                    "000a40"   // running status: CC 10 = 0x40
                                                    "00ff2f00");
        if (!SmfFile::read(smfBytes(body), &smf, &error)) {
            std::fprintf(stderr, "smfcheck: FAIL: high data bytes rejected: %s\n",
                         qUtf8Printable(error));
            failures++;
        } else {
            const std::vector<SmfEvent> &evs = smf.tracks[1].events;
            if (evs.size() != 4) {
                std::fprintf(stderr, "smfcheck: FAIL: %zu events, want 4\n", evs.size());
                failures++;
            }
            failures += checkEvent(evs, 0, 0xC0, 0x80, 0);
            failures += checkEvent(evs, 1, 0x90, 0x3C, 0x40);
            failures += checkEvent(evs, 2, 0xB0, 0x07, 0x80);
            failures += checkEvent(evs, 3, 0xB0, 0x0A, 0x40);
        }
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "smfcheck: FAIL: no temp dir\n");
        std::printf("smfcheck: FAIL\n");
        return 1;
    }

    // 3. Note pairing keeps mid2agb's rule through the linear-time pass:
    // shared ends, a vel-0 note-on as an end, cross-channel isolation within
    // one chunk, an unterminated note-on, and raw 8-bit key equality (a
    // preserved out-of-range key must not pair with its masked twin).
    {
        SmfFile smf;
        smf.format = 1;
        smf.division = 24;
        smf.tracks.resize(2);
        smf.tracks[0].endTick = 40; // conductor

        SmfTrack &t = smf.tracks[1];
        t.events.push_back(channelEvent(0, 0x90, 60, 100));   // 0: on A
        t.events.push_back(channelEvent(0, 0x90, 62, 80));    // 1: on C
        t.events.push_back(channelEvent(0, 0x91, 60, 70));    // 2: on E (channel 1)
        t.events.push_back(channelEvent(5, 0x90, 62, 0));     // 3: ends C (vel-0 form)
        t.events.push_back(channelEvent(7, 0x81, 60, 0));     // 4: ends E only
        t.events.push_back(channelEvent(10, 0x90, 60, 90));   // 5: on B, shares A's end
        t.events.push_back(channelEvent(20, 0x80, 60, 0));    // 6: ends A and B
        t.events.push_back(channelEvent(30, 0x90, 64, 50));   // 7: on D, unterminated
        t.events.push_back(channelEvent(31, 0x90, 0x83, 60)); // 8: on F, out-of-range key
        t.events.push_back(channelEvent(33, 0x80, 0x03, 0));  // 9: F's masked twin, no pair
        t.events.push_back(channelEvent(39, 0x80, 0x83, 0));  // 10: ends F
        t.endTick = 40;

        SongDocument doc;
        QString error;
        if (!loadDocument(smf, dir, "pairing", &doc, &error)) {
            std::fprintf(stderr, "smfcheck: FAIL: pairing song load: %s\n", qUtf8Printable(error));
            failures++;
        } else {
            const int engineTrack = engineTrackForChunk(doc, 1);
            const std::vector<DocNote> notes = doc.notesForTrack(engineTrack);
            if (notes.size() != 6) {
                std::fprintf(stderr, "smfcheck: FAIL: %zu notes, want 6\n", notes.size());
                failures++;
            }
            failures += checkNote(notes, 0, 0, 6, 20, 60, 100, 0);      // A
            failures += checkNote(notes, 1, 1, 3, 5, 62, 80, 0);        // C
            failures += checkNote(notes, 2, 2, 4, 7, 60, 70, 1);        // E
            failures += checkNote(notes, 3, 5, 6, 10, 60, 90, 0);       // B
            failures += checkNote(notes, 4, 7, SIZE_MAX, 0, 64, 50, 0); // D
            failures += checkNote(notes, 5, 8, 10, 8, 0x83, 60, 0);     // F
        }
    }

    // 4. The hostile shape from the security report: many unterminated
    // note-ons. Quadratic pairing would need ~4.5e10 comparisons here
    // (minutes); the linear pass must stay effectively instant.
    {
        constexpr int kNoteOns = 300000;
        SmfFile smf;
        smf.format = 1;
        smf.division = 24;
        smf.tracks.resize(2);
        smf.tracks[0].endTick = kNoteOns;
        SmfTrack &t = smf.tracks[1];
        t.events.reserve(kNoteOns);
        for (int i = 0; i < kNoteOns; i++)
            t.events.push_back(channelEvent(uint64_t(i), 0x90, 60, 100));
        t.endTick = kNoteOns;

        SongDocument doc;
        QString error;
        if (!loadDocument(smf, dir, "unterminated", &doc, &error)) {
            std::fprintf(stderr, "smfcheck: FAIL: unterminated song load: %s\n",
                         qUtf8Printable(error));
            failures++;
        } else {
            QElapsedTimer timer;
            timer.start();
            const std::vector<DocNote> notes = doc.notesForTrack(engineTrackForChunk(doc, 1));
            const qint64 ms = timer.elapsed();
            if (notes.size() != kNoteOns) {
                std::fprintf(stderr, "smfcheck: FAIL: %zu notes, want %d\n", notes.size(),
                             kNoteOns);
                failures++;
            } else if (!notes.front().unterminated() || !notes.back().unterminated()) {
                std::fprintf(stderr, "smfcheck: FAIL: unterminated note gained an end\n");
                failures++;
            }
            if (ms > 10000) {
                std::fprintf(stderr,
                             "smfcheck: FAIL: pairing %d unterminated note-ons took %lld ms\n",
                             kNoteOns, static_cast<long long>(ms));
                failures++;
            }
        }
    }

    // 5. Engine boundary: out-of-range program/key are ignored. The keysplit
    // voice makes a wild key reach for splitTable[key]/subGroup[idx]; under
    // ASAN an unguarded call aborts here.
    {
        static ToneData voices[128];
        static ToneData sub[128];
        static uint8_t splitTable[128];
        voices[5].type = VOICE_KEYSPLIT;
        voices[5].subGroup = sub;
        voices[5].keySplitTable = splitTable;

        M4AEngine engine;
        m4a_engine_init(&engine, 48000.0f);
        m4a_engine_set_voicegroup(&engine, voices);

        m4a_engine_program_change(&engine, 0, 5);
        if (engine.tracks[0].currentProgram != 5) {
            std::fprintf(stderr, "smfcheck: FAIL: in-range program change ignored\n");
            failures++;
        }
        m4a_engine_program_change(&engine, 0, 128);
        m4a_engine_program_change(&engine, 0, 255);
        if (engine.tracks[0].currentProgram != 5) {
            std::fprintf(stderr, "smfcheck: FAIL: out-of-range program %u applied\n",
                         engine.tracks[0].currentProgram);
            failures++;
        }

        m4a_engine_note_on(&engine, 0, 128, 100);
        m4a_engine_note_on(&engine, 0, 255, 100);
        m4a_engine_note_off(&engine, 0, 255);
        for (int i = 0; i < TOTAL_PCM_CHANNELS; i++) {
            if (engine.pcmChannels[i].status & CHN_ON) {
                std::fprintf(stderr, "smfcheck: FAIL: out-of-range key started a channel\n");
                failures++;
                break;
            }
        }
        m4a_engine_destroy(&engine);
    }

    std::printf("smfcheck: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
