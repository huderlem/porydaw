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

inline bool operator==(const SmfEvent &a, const SmfEvent &b)
{
    return a.tick == b.tick && a.status == b.status && a.metaType == b.metaType
        && a.data0 == b.data0 && a.data1 == b.data1 && a.blob == b.blob;
}

inline bool operator!=(const SmfEvent &a, const SmfEvent &b)
{
    return !(a == b);
}

struct SmfTrack {
    std::vector<SmfEvent> events; // non-decreasing ticks, original in-file order
    uint64_t endTick = 0;         // tick of the End-of-track meta
};

// The marker vocabulary mid2agb reads from ANY text meta (0x01-0x07) in the
// seq chunk — loop start/end and the segment separators — Channel Prefix
// notwithstanding. porydaw carves out one exception mid2agb doesn't: a
// chunk's first unprefixed 0x03 is its NAME even with marker text (imported
// files can carry such names; renameTrack refuses creating them). A
// PREFIXED 0x03 with marker text has no name position at all, so every
// reader classifies it as a marker, agreeing with mid2agb.
bool smfTextIsMarker(const QString &text);
// smfTextIsMarker over a meta event's payload (Latin-1, capped at 64 chars,
// trimmed — the same reading every name consumer uses). False for non-text
// metas.
bool smfMetaIsMarker(const SmfEvent &ev);

// MIDI Channel Prefix (meta 0x20) scoping state, shared by every reader and
// the format-0 conversion so the rule cannot drift between them: a prefix
// scopes the name metas that follow to its channel, live until the next
// channel event or prefix. Call observe() on every event in file order and
// read `channel` (-1 when no prefix is live) when classifying the event.
struct SmfChannelPrefix {
    int channel = -1;
    void observe(const SmfEvent &ev)
    {
        if (ev.isChannel())
            channel = -1;
        else if (ev.isMeta() && ev.metaType == 0x20 && ev.blob.size() >= 1)
            channel = ev.blob[0] & 0x0F;
    }
};

struct SmfFile {
    uint16_t format = 1;   // always 1 after parse; see wasFormat0
    uint16_t division = 24; // ticks per quarter note (metrical only)
    std::vector<SmfTrack> tracks;
    bool wasFormat0 = false; // parsed as format 0, coerced by convertToFormat1

    // Strict parse: any malformed track data is an error rather than a
    // truncated best-effort load — an editor that half-parses a file would
    // silently drop the rest on save. (mid2agb is equally strict.)
    // Coerces format 0 to format 1 (convertToFormat1) before returning, so
    // an unconverted file cannot escape the parse layer; wasFormat0 records
    // that it happened.
    static bool read(const QByteArray &bytes, SmfFile *out, QString *error);
    static bool readFile(const QString &path, SmfFile *out, QString *error);

    // Canonical encoding: running status for channel events, reset after any
    // meta/sysex because mid2agb's reader clears its running status there and
    // would reject a carried one.
    QByteArray write() const;
    bool writeFile(const QString &path, QString *error) const;
};

// Rewrites a format-0 file as format 1: a conductor chunk 0 carrying every
// non-channel event (tempo, time signatures, loop markers — mid2agb reads
// seq events from the first chunk exclusively), then one chunk per used
// channel in ascending channel order — the same order mid2agb emits agb
// tracks for a format-0 file (it scans each chunk for channels 0-15 in
// turn), so the compiled .s output is unchanged. Channel-Prefix-scoped text
// metas (names, instrument names, lyrics) travel to their channel's chunk;
// the 0x20 prefixes themselves are dropped, the per-channel chunk structure
// now carrying their meaning. Two preservation rules: marker text stays in
// the conductor chunk even when prefixed (mid2agb reads markers from the
// seq chunk regardless of prefixes, so moving one would change the .s; a
// prefixed marker 0x03 keeps a prefix so readers never mistake it for the
// conductor's name), and a channel with scoped text but no channel events
// still emits a chunk (name-only — it never occupies an engine slot or
// produces an agb track, but dropping it would lose foreign data on
// save). No-op on format-1
// input. SmfFile::read calls this, so everything past the parse layer deals
// in format 1; it stays callable directly for in-memory files.
void convertToFormat1(SmfFile *smf);
