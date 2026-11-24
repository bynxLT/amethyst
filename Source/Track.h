#pragma once

#include <JuceHeader.h>
#include "ClipFx.h"

// One plug-in slot in a channel's insert chain. Empty when plugin == nullptr.
struct InsertSlot
{
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::atomic<bool>  bypassed { false };
    std::atomic<float> mix { 1.0f };   // wet/dry, 0..1
    int numChannels = 2;               // channels handed to processBlock (set by the engine)
};

// A recorded or imported piece of audio on the timeline.
struct AudioClip
{
    using BufferPtr = std::shared_ptr<const juce::AudioBuffer<float>>;

    BufferPtr source;                          // decoded file, resampled to the device rate (message thread)
    BufferPtr buffer;                          // rendered with `settings`; what plays (swapped under the engine lock)
    std::atomic<juce::int64> startSample { 0 };
    std::atomic<bool> enabled { true };
    juce::File file;

    ClipSettings settings;                     // message thread only
    int renderGeneration = 0;                  // message thread only

    juce::int64 getLength() const noexcept { return buffer != nullptr ? buffer->getNumSamples() : 0; }
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
    std::atomic<int>   inputChannel { 0 };     // index into the device's *active* input channels
    std::atomic<bool>  stereoInput { false };

    std::atomic<float> meterL { 0.0f }, meterR { 0.0f }; // peak since last UI read

    std::vector<std::shared_ptr<AudioClip>> clips;     // oldest first; newer clips win where they overlap
    std::vector<std::unique_ptr<InsertSlot>> inserts;  // always maxInserts entries

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recorder;
    juce::File recordingFile;

    // audio-thread-only state
    float smoothedGainL = 1.0f, smoothedGainR = 1.0f;

    JUCE_DECLARE_NON_COPYABLE (Track)
};
