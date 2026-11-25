#pragma once

#include "AudioEngine.h"

// Undo / redo for arrangement edits (move, trim, cut, delete, record, import, enable) and the mixer
// (faders, pan, mute / solo, low cut, polarity, sends and every plug-in slot: which plug-in, bypass, mix
// and its full settings). Each step is a snapshot of both. Snapshots hold shared_ptrs, so deleted clips
// come back intact on undo; plug-ins that were removed or swapped are recreated from their saved state.
// Mixer changes are picked up by pollMixer() (call it a few times a second): once a change has settled
// and no mouse button is down, it becomes one "Mixer change" step.
class EditHistory
{
public:
    explicit EditHistory (AudioEngine& e) : engine (e) {}

    // Called before plug-ins on a channel are replaced by undo / redo (close their editors there).
    std::function<void (Track&)> beforeReplacingPlugins;

    // Call *before* changing anything.
    void checkpoint (const juce::String& what)
    {
        flushMixer();
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

    bool undo() { flushMixer(); return step (undoStack, redoStack); }
    bool redo() { flushMixer(); return step (redoStack, undoStack); }

    // Watches the mixer; a settled change becomes an undo step. userBusy = a mouse button is down.
    void pollMixer (bool userBusy)
    {
        const auto sigs = mixerSignatures();
        if (! baselineValid)
        {
            rebaseline (sigs);
            return;
        }
        if (sigs == baselineSigs)
        {
            pendingSigs.clear();
            return;
        }
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (sigs != pendingSigs)
        {
            pendingSigs = sigs;
            pendingSince = now;
            return;
        }
        if (! userBusy && now - pendingSince > 0.6)
            commitMixer (sigs);
    }

    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    juce::String nextUndoName() const { return undoStack.empty() ? juce::String() : undoStack.back().what; }
    juce::String nextRedoName() const { return redoStack.empty() ? juce::String() : redoStack.back().what; }

    void clear() { undoStack.clear(); redoStack.clear(); baselineValid = false; }

private:
    struct ClipState
    {
        std::shared_ptr<AudioClip> clip;
        juce::int64 start, offset, length;
        bool enabled;
        float gainDb;
        ClipSettings settings;
        juce::String name;
        juce::Colour colour;

        bool operator== (const ClipState& o) const
        {
            return clip == o.clip && start == o.start && offset == o.offset && length == o.length && enabled == o.enabled && gainDb == o.gainDb
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

    struct SlotState
    {
        juce::AudioPluginInstance* plugin = nullptr;   // identity (with the identifier, in case an address is reused)
        juce::String identifier;
        juce::PluginDescription desc;
        juce::MemoryBlock state;
        bool bypassed = false;
        float mix = 1.0f;
    };

    struct ChannelState
    {
        Track* track = nullptr;
        int trackId = 0;
        float volumeDb = 0, pan = 0, lowCut = 0;
        bool mute = false, solo = false, phase = false;
        std::vector<float> sends;
        std::vector<SlotState> slots;
    };

    struct Snapshot
    {
        juce::String what;
        std::vector<TrackState> tracks;
        std::vector<ChannelState> mixer;
        bool hasMixer = false;
    };

    // ---- mixer capture / restore -------------------------------------------------
    std::vector<Track*> channels() const
    {
        std::vector<Track*> list { &engine.getMaster() };
        for (auto* t : engine.getTracks())
            if (! t->isAutomation())
                list.push_back (t);
        return list;
    }

    std::vector<ChannelState> captureMixer() const
    {
        std::vector<ChannelState> out;
        for (auto* t : channels())
        {
            ChannelState cs;
            cs.track = t; cs.trackId = t->id;
            cs.volumeDb = t->volumeDb.load(); cs.pan = t->pan.load(); cs.lowCut = t->lowCutHz.load();
            cs.mute = t->mute.load(); cs.solo = t->solo.load(); cs.phase = t->phaseInvert.load();
            for (auto& s : t->sends) cs.sends.push_back (s.load());
            for (auto& slot : t->inserts)
            {
                SlotState ss;
                ss.bypassed = slot->bypassed.load();
                ss.mix = slot->mix.load();
                if (auto* p = slot->plugin.get())
                {
                    ss.plugin = p;
                    ss.desc = p->getPluginDescription();
                    ss.identifier = ss.desc.createIdentifierString();
                    p->getStateInformation (ss.state);
                }
                cs.slots.push_back (std::move (ss));
            }
            out.push_back (std::move (cs));
        }
        return out;
    }

    void restoreMixer (const std::vector<ChannelState>& mixer)
    {
        for (auto& cs : mixer)
        {
            auto* t = cs.track;
            if (! engine.isValidChannel (t) || t->id != cs.trackId)
                continue;
            t->volumeDb = cs.volumeDb; t->pan = cs.pan; t->lowCutHz = cs.lowCut;
            t->mute = cs.mute; t->solo = cs.solo; t->phaseInvert = cs.phase;
            for (size_t i = 0; i < cs.sends.size() && i < t->sends.size(); ++i)
                t->sends[i] = cs.sends[i];

            bool closedEditors = false;
            for (size_t i = 0; i < cs.slots.size() && i < t->inserts.size(); ++i)
            {
                const auto& ss = cs.slots[i];
                auto* current = t->inserts[i]->plugin.get();
                const bool same = current != nullptr && current == ss.plugin && current->getPluginDescription().createIdentifierString() == ss.identifier;
                if (same)
                {
                    juce::MemoryBlock now;
                    current->getStateInformation (now);
                    if (now != ss.state)
                        current->setStateInformation (ss.state.getData(), (int) ss.state.getSize());
                }
                else
                {
                    if (! closedEditors && beforeReplacingPlugins) { beforeReplacingPlugins (*t); closedEditors = true; }
                    if (ss.plugin == nullptr)
                        engine.removeInsert (*t, (int) i).reset();
                    else
                    {
                        juce::String err;
                        if (auto p = engine.createPlugin (ss.desc, err))
                        {
                            p->setStateInformation (ss.state.getData(), (int) ss.state.getSize());
                            engine.insertPlugin (*t, (int) i, std::move (p)).reset();
                        }
                    }
                }
                t->inserts[i]->bypassed = ss.bypassed;
                t->inserts[i]->mix = ss.mix;
            }
        }
    }

    // One cheap fingerprint per channel: strip values, slot identities, bypass / mix and every parameter
    // value - except values that automation is moving (playback would otherwise look like edits).
    std::vector<juce::uint64> mixerSignatures() const
    {
        struct Auto { const Track* t; Automation::Kind kind; int slot, param; };
        std::vector<Auto> automated;
        for (auto* t : engine.getTracks())
            if (auto* a = t->automation.get(); a != nullptr && a->target != nullptr)
                automated.push_back ({ a->target, a->kind, a->slot, a->param });
        auto isAutomated = [&] (const Track* t, Automation::Kind k, int slot, int param)
        {
            for (auto& a : automated)
                if (a.t == t && a.kind == k && (k != Automation::Kind::Param || (a.slot == slot && a.param == param)))
                    return true;
            return false;
        };

        std::vector<juce::uint64> sigs;
        for (auto* t : channels())
        {
            juce::uint64 h = 1469598103934665603ull;
            auto mixIn = [&h] (double v) { juce::uint64 bits; float f = (float) v; std::uint32_t u; std::memcpy (&u, &f, 4); bits = u; h = (h ^ bits) * 1099511628211ull; };
            if (! isAutomated (t, Automation::Kind::Volume, -1, -1)) mixIn (t->volumeDb.load());
            if (! isAutomated (t, Automation::Kind::Pan, -1, -1))    mixIn (t->pan.load());
            mixIn (t->mute.load()); mixIn (t->solo.load()); mixIn (t->lowCutHz.load()); mixIn (t->phaseInvert.load());
            for (auto& s : t->sends) mixIn (s.load());
            for (int i = 0; i < (int) t->inserts.size(); ++i)
            {
                auto& slot = t->inserts[(size_t) i];
                mixIn ((double) (juce::pointer_sized_int) slot->plugin.get());
                mixIn (slot->bypassed.load()); mixIn (slot->mix.load());
                if (auto* p = slot->plugin.get())
                {
                    const auto& params = p->getParameters();
                    for (int k = 0; k < params.size(); ++k)
                        if (! isAutomated (t, Automation::Kind::Param, i, k))
                            mixIn (params[k]->getValue());
                }
            }
            sigs.push_back (h ^ (juce::uint64) t->id);
        }
        return sigs;
    }

    void rebaseline (const std::vector<juce::uint64>& sigs)
    {
        baseline = captureMixer();
        baselineSigs = sigs;
        pendingSigs.clear();
        baselineValid = true;
    }

    void commitMixer (const std::vector<juce::uint64>& sigs)
    {
        // name the step after the first channel that changed
        juce::String what = "Mixer change";
        const auto list = channels();
        for (size_t i = 0; i < list.size() && i < sigs.size(); ++i)
            if (i >= baselineSigs.size() || sigs[i] != baselineSigs[i])
            {
                what << " on " << (list[i]->isMaster ? juce::String ("Master") : list[i]->name);
                break;
            }
        Snapshot s = captureClipsOnly (what);
        s.mixer = baseline;
        s.hasMixer = true;
        undoStack.push_back (std::move (s));
        if (undoStack.size() > maxSteps)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
        rebaseline (sigs);
    }

    void flushMixer()
    {
        if (! baselineValid)
            return;
        const auto sigs = mixerSignatures();
        if (sigs != baselineSigs)
            commitMixer (sigs);
    }

    std::vector<ChannelState> baseline;
    std::vector<juce::uint64> baselineSigs, pendingSigs;
    double pendingSince = 0.0;
    bool baselineValid = false;

    Snapshot capture (const juce::String& what) const
    {
        auto s = captureClipsOnly (what);
        s.mixer = captureMixer();
        s.hasMixer = true;
        return s;
    }

    Snapshot captureClipsOnly (const juce::String& what) const
    {
        Snapshot s { what, {}, {}, false };
        for (auto* t : engine.getTracks())
        {
            TrackState ts { t, t->id, {} };
            for (auto& c : t->clips)
                ts.clips.push_back ({ c, c->startSample.load(), c->trimOffset.load(), c->trimLength.load(), c->enabled.load(), c->handleGainDb.load(),
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
                cs.clip->handleGainDb = cs.gainDb;
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
        if (s.hasMixer)
            restoreMixer (s.mixer);
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
        rebaseline (mixerSignatures());   // the restored mixer is the new starting point
        return true;
    }

    AudioEngine& engine;
    std::vector<Snapshot> undoStack, redoStack;
    static constexpr size_t maxSteps = 200;
};
