#pragma once

#include <QStringList>

class MidiTimeline;
class SongView;

QStringList playheadOverlayCheckFailures(SongView &view, const MidiTimeline &timeline);
