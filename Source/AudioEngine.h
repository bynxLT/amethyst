#pragma once

#include <JuceHeader.h>
#include "Track.h"

// Owns the audio device, tracks, master bus, transport, recording and plug-in hosting.
class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::AudioPlayHead
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager       deviceManager;
    juce::AudioPluginFormatManager pluginFormats;
    juce::AudioFormatManager       audioFormats;
    juce::KnownPluginList          knownPlugins;

    juce::CriticalSection& getLock() noexcept { return lock; }

    //==== Tracks ==============================================================
    Track* addTrack (const juce::String& name);
    void removeTrack (Track*);
    void clearTracks();
    const juce::OwnedArray<Track>& getTracks() const noexcept { return tracks; }
    Track& getMaster() noexcept { return master; }
    bool isValidChannel (const Track*) const;

    //==== Transport ===========================================================
    void play();
    void stop();
    bool record();   // returns false if no track is armed
    bool isPlaying() const noexcept   { return playing.load(); }
    bool isRecording() const noexcept { return recording.load(); }
    juce::int64 getTransportPosition() const noexcept { return position.load(); }
    void setTransportPosition (juce::int64 samplePos);
    juce::int64 getRecordStart() const noexcept { return recordStart; }
    double getSampleRate() const noexcept { return sampleRate.load(); }
    juce::int64 getSessionLengthSamples() const;

    std::atomic<bool>   metronomeOn { false };
    std::atomic<double> bpm { 120.0 };

    //==== Export (real-time bounce of the master bus) =========================
    bool startBounce (const juce::File& wavFile);
    bool isBouncing() const noexcept { return bounceWriter != nullptr; }
    bool bounceReachedEnd() const noexcept { return bounceDone.load(); }

    //==== Plug-ins ============================================================
    std::unique_ptr<juce::AudioPluginInstance> createPlugin (const juce::PluginDescription&, juce::String& error);
    std::function<bool()> isProUnlocked;   // built-in AMETHYST effects require Pro (unset = allowed, e.g. self-tests)

    // Puts a plug-in into slot `index`. Returns the old slot (destroy it on the message thread).
    std::unique_ptr<InsertSlot> insertPlugin (Track&, int index, std::unique_ptr<juce::AudioPluginInstance>);
    std::unique_ptr<InsertSlot> removeInsert (Track&, int index);
    void moveInsert (Track&, int from, int to);   // swaps the two slots
    std::vector<std::unique_ptr<InsertSlot>> replaceInserts (Track&, std::vector<std::unique_ptr<InsertSlot>>);
    int firstFreeSlot (const Track&) const;

    //==== Clips ===============================================================
    std::shared_ptr<AudioClip> loadClip (const juce::File&, juce::int64 startSample);
    void addClip (Track&, std::shared_ptr<AudioClip>);
    std::shared_ptr<AudioClip> removeClip (Track&, const AudioClip*);
    bool canImport (const juce::File&);
    bool replaceClipSource (AudioClip&, const juce::File&);

    // Re-render a clip's effects on a background thread; the result is swapped in on the message thread.
    void requestRender (const std::shared_ptr<AudioClip>&);
    void renderNow (AudioClip&);   // synchronous (used when loading projects)
    bool isRendering() const noexcept { return pendingRenders.load() > 0; }
    double getCpuUsage() const { return deviceManager.getCpuUsage(); }

    static juce::File getAppFolder();
    static juce::File getTakesFolder()    { return getAppFolder().getChildFile ("Takes"); }
    static juce::File getProjectsFolder() { return getAppFolder().getChildFile ("Projects"); }

    //==== AudioIODeviceCallback ===============================================
    void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                           float* const* output, int numOutputs,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    //==== AudioPlayHead (tempo / position for plug-ins) =======================
    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override;

private:
    void preparePlugin (juce::AudioPluginInstance&, double sr, int bs);
    static int channelsFor (juce::AudioPluginInstance&);
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> createWavWriter (const juce::File&, int numChannels);
    void ensureScratch (int numSamples);
    void processChannel (Track&, int numSamples);   // inserts + fader/pan + meters on trackBuffer[0..1]
    template <typename Fn> void forEachChannel (Fn&& fn) { fn (master); for (auto* t : tracks) fn (*t); }

    juce::CriticalSection lock;
    Track master { 0, "Master", true };
    juce::OwnedArray<Track> tracks;
    int nextTrackId = 1;

    std::atomic<bool> playing { false }, recording { false }, bounceDone { false };
    std::atomic<juce::int64> position { 0 };
    juce::int64 recordStart = 0;
    juce::int64 bounceEnd = 0;
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> blockSize { 512 };

    juce::TimeSliceThread writerThread { "VocalDAW disk writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> bounceWriter;

    juce::AudioBuffer<float> trackBuffer, masterBuffer, dryBuffer;
    juce::HeapBlock<float> silence;
    juce::HeapBlock<juce::uint8> coverage;
    int scratchSize = 0;
    juce::MidiBuffer midiScratch;

    void setClipBuffer (AudioClip&, AudioClip::BufferPtr);
    juce::ThreadPool renderPool { 1 };
    std::shared_ptr<int> aliveToken = std::make_shared<int> (0);
    std::atomic<int> pendingRenders { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
