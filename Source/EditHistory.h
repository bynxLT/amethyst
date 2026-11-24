#pragma once

#include "AudioEngine.h"

// Undo / redo for arrangement edits (move, trim, cut, delete, record, import, enable).
// Each step is a snapshot of every track's clip list plus each clip's position/trim.
// Snapshots hold shared_ptrs, so deleted clips come back intact on undo.
class EditHistory
{
public:
    explicit EditHistory (AudioEngine& e) : engine (e) {}

    // Call *before* changing anything.
    void checkpoint (const juce::String& what)
    {
        undoStack.push_back (capture (what));
        if (undoStack.size() > maxSteps)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
    }

    // Call after a gesture that may not have changed anything (e.g. a click without a drag).
    void discardIfUnchanged()
    {
        if (! undoStack.empty() && sameLayout (undoStack.back(), capture ({})))
            undoStack.pop_back();
    }

    bool undo() { return step (undoStack, redoStack); }
    bool redo() { return step (redoStack, undoStack); }

    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    juce::String nextUndoName() const { return undoStack.empty() ? juce::String() : undoStack.back().what; }
    juce::String nextRedoName() const { return redoStack.empty() ? juce::String() : redoStack.back().what; }

    void clear() { undoStack.clear(); redoStack.clear(); }

private:
    struct ClipState
    {
        std::shared_ptr<AudioClip> clip;
        juce::int64 start, offset, length;
        bool enabled;
        ClipSettings settings;
        juce::String name;
        juce::Colour colour;

        bool operator== (const ClipState& o) const
        {
            return clip == o.clip && start == o.start && offset == o.offset && length == o.length && enabled == o.enabled
                   && settings == o.settings && name == o.name && colour == o.colour;
        }
    };

    struct TrackState
    {
        Track* track;
        int trackId;   // pointers can be reused after a delete; the id can't
        std::vector<ClipState> clips;
        bool operator== (const TrackState& o) const { return track == o.track && trackId == o.trackId && clips == o.clips; }
    };

    struct Snapshot
    {
        juce::String what;
        std::vector<TrackState> tracks;
    };

    Snapshot capture (const juce::String& what) const
    {
        Snapshot s { what, {} };
        for (auto* t : engine.getTracks())
        {
            TrackState ts { t, t->id, {} };
            for (auto& c : t->clips)
                ts.clips.push_back ({ c, c->startSample.load(), c->trimOffset.load(), c->trimLength.load(), c->enabled.load(),
                                      c->settings, c->name, c->colour });
            s.tracks.push_back (std::move (ts));
        }
        return s;
    }

    void restore (const Snapshot& s)
    {
        for (auto& ts : s.tracks)
        {
            if (! engine.isValidChannel (ts.track) || ts.track->id != ts.trackId) // the track was deleted since
                continue;

            std::vector<std::shared_ptr<AudioClip>> list;
            for (auto& cs : ts.clips)
            {
                cs.clip->startSample = cs.start;
                cs.clip->trimOffset = cs.offset;
                cs.clip->trimLength = cs.length;
                cs.clip->enabled = cs.enabled;
                cs.clip->name = cs.name;
                cs.clip->colour = cs.colour;
                if (cs.clip->settings != cs.settings)
                {
                    cs.clip->settings = cs.settings;
                    engine.requestRender (cs.clip); // effects changed: re-render in the background
                }
                list.push_back (cs.clip);
            }
            engine.setClips (*ts.track, std::move (list));
        }
    }

    static bool sameLayout (const Snapshot& a, const Snapshot& b) { return a.tracks == b.tracks; }

    bool step (std::vector<Snapshot>& from, std::vector<Snapshot>& to)
    {
        if (from.empty())
            return false;
        auto target = std::move (from.back());
        from.pop_back();
        to.push_back (capture (target.what));
        restore (target);
        return true;
    }

    AudioEngine& engine;
    std::vector<Snapshot> undoStack, redoStack;
    static constexpr size_t maxSteps = 200;
};
