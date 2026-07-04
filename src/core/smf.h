#pragma once

#include <QByteArray>
#include <QString>
#include <cstdint>
#include <vector>

// Full-fidelity Standard MIDI File model. Unlike MidiTimeline (a lossy,
// playback-oriented view), this preserves every event and its in-file order
// so that a load -> save with no edits is semantically identical through
// mid2agb (SPEC.md §8 M2). It is the storage layer under SongDocument.

// One event in an MTrk chunk at an absolute tick. Channel-voice events keep
// their original status byte — a note-on with velocity 0 is NOT rewritten as
// a note-off — and meta/sysex events keep their full payload bytes.
// End-of-track metas are not stored as events; SmfTrack::endTick carries
// their position (mid2agb uses the EOT tick when merging tracks, so it must
// survive a round-trip).
struct SmfEvent {
    uint64_t tick = 0;
    uint8_t status = 0;   // 0x80-0xEF channel voice; 0xF0/0xF7 sysex; 0xFF meta
    uint8_t metaType = 0; // valid when status == 0xFF
    uint8_t data0 = 0;    // channel-voice data bytes
    uint8_t data1 = 0;
    QByteArray blob;      // meta/sysex payload

    bool isMeta() const { return status == 0xFF; }
    bool isSysEx() const { return status == 0xF0 || status == 0xF7; }
    bool isChannel() const { return status >= 0x80 && status < 0xF0; }
    uint8_t typeNibble() const { return status >> 4; }
    uint8_t channel() const { return status & 0x0F; }
    bool isNoteOn() const { return typeNibble() == 0x9 && data1 != 0; }
    // Anything that ends a sounding note: note-off or note-on with velocity 0
    // (mid2agb accepts both interchangeably).
    bool isNoteEnd() const
    {
        return typeNibble() == 0x8 || (typeNibble() == 0x9 && data1 == 0);
    }
};

struct SmfTrack {
    std::vector<SmfEvent> events; // non-decreasing ticks, original in-file order
    uint64_t endTick = 0;         // tick of the End-of-track meta
};

struct SmfFile {
    uint16_t format = 1;   // 0 or 1
    uint16_t division = 24; // ticks per quarter note (metrical only)
    std::vector<SmfTrack> tracks;

    // Strict parse: any malformed track data is an error rather than a
    // truncated best-effort load — an editor that half-parses a file would
    // silently drop the rest on save. (mid2agb is equally strict.)
    static bool read(const QByteArray &bytes, SmfFile *out, QString *error);
    static bool readFile(const QString &path, SmfFile *out, QString *error);

    // Canonical encoding: running status for channel events, reset after any
    // meta/sysex because mid2agb's reader clears its running status there and
    // would reject a carried one.
    QByteArray write() const;
    bool writeFile(const QString &path, QString *error) const;
};
