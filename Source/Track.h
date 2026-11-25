#pragma once

#include <JuceHeader.h>
#include "ClipFx.h"

// One plug-in slot in a channel's insert chain. Empty when plugin == nullptr.
struct InsertSlot
{
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::atomic<bool>  bypassed { false };
    std::atomic<float> mix { 1.0f };   // wet/dry, 0..1
    std::atomic<bool>  crashed { false };   // the plug-in crashed while processing: kept off until reloaded
    bool crashReported = false;             // message thread: the user has been told
    int numChannels = 2;               // channels handed to processBlock (set by the engine)
};

// A recorded or imported piece of audio on the timeline.
struct AudioClip
{
    using BufferPtr = std::shared_ptr<const juce::AudioBuffer<float>>;

    BufferPtr source;                          // decoded file, resampled to the device rate (message thread)
    BufferPtr buffer;                          // rendered with `settings`; what plays (swapped under the engine lock)
    std::atomic<juce::int64> startSample { 0 };  // timeline position of the first *played* sample
    std::atomic<juce::int64> trimOffset { 0 };   // samples skipped at the start of `buffer` (non-destructive trim)
    std::atomic<juce::int64> trimLength { -1 };  // samples played; -1 = to the end of `buffer`
    std::atomic<bool> enabled { true };
    juce::File file;

    ClipSettings settings;                     // message thread only
    int renderGeneration = 0;                  // message thread only
    juce::String name;                         // display name (empty = file name)
    juce::Colour colour;                       // transparent = default clip colour
    bool lossless = false;
    std::atomic<float> handleGainDb { 0.0f };  // clip volume handle (applied live while playing)                     // WAV / AIFF / FLAC: original edges play untouched (see AudioEngine::losslessUntouched)

    juce::String getDisplayName() const { return name.isNotEmpty() ? name : file.getFileNameWithoutExtension(); }

    juce::int64 getBufferLength() const noexcept { return buffer != nullptr ? buffer->getNumSamples() : 0; }

    juce::int64 getOffset() const noexcept
    {
        return juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, getBufferLength() - 1), trimOffset.load());
    }

    // Number of samples that actually play (after trimming).
    juce::int64 getLength() const noexcept
    {
        const auto available = getBufferLength() - getOffset();
        const auto len = trimLength.load();
        return juce::jmax<juce::int64> (0, len < 0 ? available : juce::jmin (len, available));
    }

    juce::int64 getEnd() const noexcept { return startSample.load() + getLength(); }
};

class Track;

// Automation clip: an envelope over time that drives one control of a channel
// (its volume, its pan, or one parameter of a plug-in in one of its slots).
struct AutomationPoint
{
    double seconds = 0.0;
    float value = 0.0f;   // normalised 0..1
};

struct Automation
{
    enum class Kind { Volume, Pan, Param };
    Kind kind = Kind::Param;
    Track* target = nullptr;   // the channel (may be the master); nullptr once that channel is deleted
    int slot = -1;             // insert slot (Kind::Param)
    int param = -1;            // parameter index in that plug-in (Kind::Param)
    std::vector<AutomationPoint> points;   // sorted by time; edited only under the engine lock

    float valueAt (double seconds) const
    {
        if (points.empty())
            return 0.0f;
        if (seconds <= points.front().seconds) return points.front().value;
        if (seconds >= points.back().seconds)  return points.back().value;
        auto hi = std::upper_bound (points.begin(), points.end(), seconds,
                                    [] (double s, const AutomationPoint& p) { return s < p.seconds; });
        auto lo = hi - 1;
        const double span = hi->seconds - lo->seconds;
        const float f = span > 1.0e-9 ? (float) ((seconds - lo->seconds) / span) : 1.0f;
        return lo->value + (hi->value - lo->value) * f;
    }
};

// A mixer channel (+ timeline lane for normal tracks). The master bus is also a Track.
//
// Threading: the atomics are read by the audio thread and written by the UI.
// clips / inserts / recorder are only *modified* on the message thread while holding
// AudioEngine::getLock(); the audio thread reads them while holding the same lock.
class Track
{
public:
    static constexpr int maxInserts = 10;

    Track (int uid, const juce::String& trackName, bool isMasterBus = false)
        : id (uid), isMaster (isMasterBus), name (trackName)
    {
        for (int i = 0; i < maxInserts; ++i)
            inserts.push_back (std::make_unique<InsertSlot>());
    }

    int countPlugins() const
    {
        int n = 0;
        for (auto& s : inserts)
            if (s->plugin != nullptr)
                ++n;
        return n;
    }

    const int id;
    const bool isMaster;
    juce::String name;

    std::atomic<float> volumeDb { 0.0f };
    std::atomic<float> pan { 0.0f };          // -1 (L) .. +1 (R)
    std::atomic<bool>  mute { false }, solo { false }, armed { false }, monitor { false };
    std::atomic<bool>  layerTakes { true };    // overlapping clips: true = hear all, false = newest on top
    bool takesToFreeTrack = true;              // message thread: a new take that overlaps moves to the next free track

    // Take lane: when set, this track's clips play through that track's mixer channel
    // (its plug-ins, fader and pan) instead of its own. Changed only via AudioEngine::setPlayThrough.
    Track* playThrough = nullptr;
    std::atomic<int>   inputChannel { 0 };     // index into the device's *active* input channels
    std::atomic<float> inputGainDb { 0.0f };   // mic boost before recording and monitoring (-12 .. +24 dB)
    std::atomic<bool>  stereoInput { false };

    std::atomic<float> meterL { 0.0f }, meterR { 0.0f }; // peak since last UI read
    std::atomic<bool>  clipped { false };                 // output went over 0 dBFS (UI clears it)
    std::atomic<float> inputPeak { 0.0f };                // input level (after input gain) of armed / monitored tracks; UI reads and resets

    // Channel strip extras (before the inserts)
    std::atomic<bool>  phaseInvert { false };
    std::atomic<float> lowCutHz { 0.0f };      // 0 = off, else a 12 dB/oct high-pass at this frequency
    juce::Colour colour;                       // message thread; transparent = default look

    // Send buses: a bus is a channel with no clips that other channels send to (post-fader).
    static constexpr int maxBuses = 4;
    bool isBus = false;                        // message thread (set once when created)
    int busIndex = -1;                         // 0..maxBuses-1 for a bus
    std::array<std::atomic<float>, maxBuses> sends {};   // send level to each bus (linear 0..1)

    // Plug-in delay compensation: total latency of this channel's inserts (published for the UI)
    std::atomic<int> latencySamples { 0 };

    std::vector<std::shared_ptr<AudioClip>> clips;     // oldest first; newer clips win where they overlap
    std::vector<std::unique_ptr<InsertSlot>> inserts;  // always maxInserts entries

    // Set = this row is an automation clip lane (no audio of its own).
    std::unique_ptr<Automation> automation;
    bool isAutomation() const noexcept { return automation != nullptr; }

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recorder;
    juce::File recordingFile;

    // audio-thread-only state
    float smoothedGainL = 1.0f, smoothedGainR = 1.0f;
    juce::IIRFilter lowCutFilter[2];
    float lowCutDesigned = -1.0f;
    static constexpr int pdcSize = 1 << 16;     // up to ~1.3 s of compensation at 48 kHz
    std::vector<float> pdcLine[2] { std::vector<float> ((size_t) pdcSize, 0.0f), std::vector<float> ((size_t) pdcSize, 0.0f) };
    int pdcPos = 0;

    JUCE_DECLARE_NON_COPYABLE (Track)
};
