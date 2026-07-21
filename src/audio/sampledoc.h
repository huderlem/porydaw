#pragma once

#include "sampledata.h"

// SampleDocument: an immutable ImportedSample plus the current
// SampleEditParams, rendered on demand through the deterministic DSP.md §1
// pipeline into a cached ProcessedSample. Undo inside the editor is a params
// swap; the decoded buffer is never mutated (PLAN.md §1 design stances).
class SampleDocument
{
public:
    explicit SampleDocument(ImportedSample source);

    const ImportedSample &source() const { return m_source; }

    // The initial parameter set for a fresh import. Prepared GBA-ready
    // sources (8-bit unsigned mono .wav) get a no-op pipeline — identity
    // rate, no gain, no fades, source agbp carried verbatim — so a
    // phase-1-style import commits byte-faithful data. Hi-res sources get
    // the DSP.md ship defaults (auto normalize, auto DC removal, micro
    // fades).
    static SampleEditParams defaultParams(const ImportedSample &source);

    const SampleEditParams &params() const { return m_params; }
    void setParams(const SampleEditParams &params);

    // The cached render; re-renders only when the params changed.
    const ProcessedSample &processed();

private:
    ProcessedSample render() const;

    ImportedSample m_source;
    SampleEditParams m_params;
    ProcessedSample m_processed;
    bool m_dirty = true;
};
