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
    Track* addBus (const juce::String& name);   // nullptr when all buses are in use
    Track* getBus (int index) const;
    void removeTrack (Track*);
    void clearTracks();
    const juce::OwnedArray<Track>& getTracks() const noexcept { return tracks; }
    Track& getMaster() noexcept { return master; }
    bool isValidChannel (const Track*) const;

    //==== Automation ==========================================================
    // Adds an automation lane for a channel control, starting flat at the control's current value.
    Track* addAutomationTrack (Track& target, Automation::Kind, int slot = -1, int param = -1);
    static float getControlValue (const Automation&);          // the automated control's current value (0..1)
    static juce::String describeTarget (const Automation&);    // "Track 1 - Volume", "protect - Tape Stop: Play"
    static float volumeDbToNormalised (float db);
    static float normalisedToVolumeDb (float v);

    //==== Transport ===========================================================
    void play();
    void stop();
    // Starts recording armed tracks. countInSamples > 0 rolls the transport in from that far before
    // the record point with a click. With punch on, only the punch region is recorded.
    bool record (juce::int64 countInSamples = 0);
    bool isPunchRecording() const noexcept { return punchRecording.load(); }
    // After a punch take: trim it to the punch region and cut the old audio inside the region out of that track.
    void applyPunchTake (Track&, const std::shared_ptr<AudioClip>& take);

    // Punch region (timeline samples). Used when punchEnabled is on and out > in.
    std::atomic<bool> punchEnabled { false };
    std::atomic<juce::int64> punchIn { -1 }, punchOut { -1 };
    bool hasPunchRegion() const noexcept { return punchIn.load() >= 0 && punchOut.load() > punchIn.load(); }
    bool isPlaying() const noexcept   { return playing.load(); }
    bool isRecording() const noexcept { return recording.load(); }
    juce::int64 getTransportPosition() const noexcept { return position.load(); }
    void setTransportPosition (juce::int64 samplePos);
    juce::int64 getRecordStart() const noexcept { return recordStart; }
    double getSampleRate() const noexcept { return sampleRate.load(); }
    juce::int64 getSessionLengthSamples() const;

    std::atomic<bool>   metronomeOn { false };
    std::atomic<double> bpm { 120.0 };

    // Metronome options: volume 0..1.5, sound 0 = beep, 1 = woodblock, 2 = hi-hat; accent the first beat of the bar
    std::atomic<float> metronomeVolume { 1.0f };
    std::atomic<int>   metronomeSound { 0 };
    std::atomic<bool>  metronomeAccent { true };

    // Stopping jumps back to where playback / recording started (Settings > Return to start on stop)
    std::atomic<bool> returnOnStop { true };

    // Clips on one track that overlap a little (not fully - those are doubles) crossfade over the overlap
    std::atomic<bool> autoCrossfade { true };
    // Crossfade lengths for a clip: how much of its start / end overlaps a neighbour on the same track
    // (partial overlaps only, at most 2 s and half of either clip - fully stacked doubles don't count).
    void crossfadeLengths (const Track&, const AudioClip&, juce::int64& in, juce::int64& out) const;

    //==== Session notes (message thread only): markers and lyrics, saved with the project ====
    struct Marker { double seconds = 0.0; juce::String name; };
    std::vector<Marker> markers;   // kept sorted by time
    juce::String lyrics;
    void clearSessionNotes() { markers.clear(); lyrics.clear(); loopEnabled = false; loopStart = -1; loopEnd = -1; }

    //==== Loop playback =========================================================
    // While playing (not recording), reaching loopEnd jumps back to loopStart, sample-accurately.
    std::atomic<bool> loopEnabled { false };
    std::atomic<juce::int64> loopStart { -1 }, loopEnd { -1 };
    bool hasLoopRegion() const noexcept { return loopStart.load() >= 0 && loopEnd.load() > loopStart.load() + 64; }

    //==== Monitoring ============================================================
    // How the master is heard (never affects exports): 0 stereo, 1 mono, 2 side (L-R), 3 left only, 4 right only.
    std::atomic<int> monitorMode { 0 };
    // While recording, monitored + armed tracks skip plug-ins that add latency (and delay compensation),
    // so the singer hears themselves with no extra delay. The take itself is unaffected.
    std::atomic<bool> lowLatencyMonitoring { true };

    //==== Plug-in delay compensation ===============================================
    // Every channel is delayed to line up with the channel whose plug-ins add the most latency.
    int getMaxChannelLatency() const noexcept { return maxLatency.load(); }

    //==== Plug-in crash protection ==============================================
    // A third-party plug-in that crashes while processing is switched off (its slot is marked crashed);
    // the count goes up so the UI can tell the user which one.
    std::atomic<int> pluginCrashes { 0 };

    //==== Live master loudness (for the toolbar readout and reference matching) ===
    std::atomic<float> masterShortTermLufs { -100.0f }, masterIntegratedLufs { -100.0f };
    void resetMasterLoudness() { resetLoudness = true; }

    //==== Reference track A/B ===================================================
    // A song to compare the mix against. While referenceOn is set and the transport plays, the output is
    // the reference (at the same timeline position) instead of the mix. Gain matches loudness if asked.
    void setReference (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double integratedLufs);
    bool hasReference() const noexcept { return referenceLufs.load() > -99.0; }
    std::atomic<bool>  referenceOn { false }, referenceMatch { true };
    std::atomic<double> referenceLufs { -100.0 };
    float getReferenceGainDb() const;   // the gain applied when matching

    //==== Retrospective record ==================================================
    // The armed (or else monitored) track's input is always kept for the last minute, even when not
    // recording. captureRetro() turns it into a take: placed where it was played if the transport was
    // rolling, or at the playhead. Returns nullptr if nothing was heard.
    std::pair<Track*, std::shared_ptr<AudioClip>> captureRetro();
    double getRetroSeconds() const;     // how much is buffered right now

    //==== Offline channel render (bounce in place, stems) =========================
    // Renders one channel - its clips and take lanes through its inserts - from 0 to the end of the
    // song plus `tailSeconds`. postFader = with its fader and pan. Mute / solo are ignored.
    juce::Result renderChannel (Track&, bool postFader, juce::AudioBuffer<float>& result,
                                std::function<bool (float)> onProgress, double tailSeconds = 3.0);

    //==== Export ===============================================================
    // Renders the whole song (plus a 3 s tail) as fast as the CPU allows into `result`.
    // Call from a background thread. onProgress gets 0..1 and returns false to cancel.
    juce::Result renderSong (juce::AudioBuffer<float>& result, std::function<bool (float)> onProgress);
    bool isExporting() const noexcept { return offlineActive.load(); }

    // (older real-time bounce, kept for compatibility)
    bool startBounce (const juce::File& wavFile);
    bool isBouncing() const noexcept { return bounceWriter != nullptr; }
    bool bounceReachedEnd() const noexcept { return bounceDone.load(); }

    //==== Audition player (tool windows such as the Nightcore Maker) ===========
    // Plays a stereo buffer straight to the output, bypassing the mixer. Independent of the transport.
    void startAudition (std::shared_ptr<const juce::AudioBuffer<float>>, juce::int64 fromSample = 0);
    void stopAudition();
    void swapAuditionBuffer (std::shared_ptr<const juce::AudioBuffer<float>>);   // keeps the position
    bool isAuditioning() const noexcept { return auditionOn.load(); }
    juce::int64 getAuditionPosition() const noexcept { return auditionPos.load(); }
    void setAuditionPosition (juce::int64 p) noexcept { auditionPos = juce::jmax<juce::int64> (0, p); }
    std::atomic<float> auditionGain { 1.0f };

    //==== Plug-ins ============================================================
    std::unique_ptr<juce::AudioPluginInstance> createPlugin (const juce::PluginDescription&, juce::String& error);
    std::function<int()> unlockedTier;     // account tier (0 Free, 1 Pro, 2 Ultra); unset = everything allowed (self-tests)

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

    // Whole clip list of a track (for undo). setClips swaps it in under the lock.
    std::vector<std::shared_ptr<AudioClip>> getClips (const Track& t) const { return t.clips; }
    void setClips (Track&, std::vector<std::shared_ptr<AudioClip>>);

    // Play one clip on its own from its start (Stop ends the preview).
    void previewClipSolo (const AudioClip&);
    bool isPreviewing() const noexcept { return previewClip.load() != nullptr; }

    // Route a take lane through another track's channel (nullptr = its own channel). Never chains.
    void setPlayThrough (Track& lane, Track* channel);

    // Where should a new take go? Returns `source` if nothing overlaps it there, else the next track
    // below that's free for the take's time span (not armed / muted), else nullptr (caller adds a track).
    Track* findTrackForTake (Track* source, const AudioClip& take) const;

    // Takes created by the last stop() after recording (track, clip). Cleared when read.
    std::vector<std::pair<Track*, std::shared_ptr<AudioClip>>> consumeNewTakes() { return std::exchange (newTakes, {}); }

    // Non-destructive editing. Times are timeline samples.
    void trimClipStart (AudioClip&, juce::int64 newStart);   // keeps the end where it is
    void trimClipEnd (AudioClip&, juce::int64 newEnd);       // keeps the start where it is
    std::shared_ptr<AudioClip> splitClip (Track&, const std::shared_ptr<AudioClip>&, juce::int64 at); // returns the new right-hand clip

    // Take comping: within [from, to) keep the part of `chosen` (on `track`) and mute every other take that
    // plays through the same channel (the channel and its take lanes, and other takes on the same row).
    // Clips are split at the edges. Returns how many other takes were muted there.
    int compTake (Track& track, const std::shared_ptr<AudioClip>& chosen, juce::int64 from, juce::int64 to);
    static constexpr int minClipSamples = 256;
    bool canImport (const juce::File&);
    bool replaceClipSource (AudioClip&, const juce::File&);

    // Re-render a clip's effects on a background thread; the result is swapped in on the message thread.
    void requestRender (const std::shared_ptr<AudioClip>&);
    void renderNow (AudioClip&);   // synchronous (used when loading projects)
    bool isRendering() const noexcept { return pendingRenders.load() > 0; }
    double getCpuUsage() const { return deviceManager.getCpuUsage(); }

    // MP3 gapless info (Xing/Info + LAME/Lavc header). trimStart / validLength are in decoded samples for
    // JUCE's decoder: the encoder delay plus the decoder's own 529-sample delay is skipped, the padding cut.
    struct Mp3Gapless { bool found = false; juce::int64 frames = 0; int encoderDelay = 0, padding = 0, trimStart = 0; juce::int64 validLength = -1; };
    static Mp3Gapless readMp3Gapless (const juce::File&);

    // Import settings (Settings > Audio files): lossless files (WAV, AIFF, FLAC) play exactly as recorded -
    // no automatic edge fades except where you cut or trim; MP3s get their encoder padding trimmed so they line up.
    std::atomic<bool> losslessUntouched { true }, trimMp3Padding { true };
    static bool isLossless (const juce::File& f) { return f.hasFileExtension ("wav;wave;aif;aiff;flac;bwf"); }
    ClipSettings renderSettingsFor (const AudioClip&) const;

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
    void processChannel (Track&, int numSamples, bool skipLatent = false);
    int channelLatency (const Track&, bool skipLatent) const;
    void delayChannel (Track&, int numSamples, int delay);   // PDC delay line on trackBuffer[0..1]
    int computeMaxLatency (bool recordingNow) const;
    void processSegment (const float* const* input, int numInputs, float* const* output, int numOutputs, int numSamples);
    std::atomic<int> maxLatency { 0 };
    juce::int64 recordPdc = 0;

    void writeRetro (const float* inL, const float* inR, int numSamples, float gain, bool playingNow, juce::int64 pos);
    juce::AudioBuffer<float> retroRing;
    juce::int64 retroWritten = 0;          // samples written since the last discontinuity (audio thread, under lock)
    int retroPos = 0;
    juce::int64 retroTimelineEnd = -1;     // timeline position of the newest sample, -1 = transport stopped
    Track* retroTrack = nullptr;

    std::shared_ptr<const juce::AudioBuffer<float>> referenceBuffer;   // swapped under the lock

    juce::uint32 metroNoise = 12345;
    float metroPrev = 0.0f;
    std::atomic<bool> resetLoudness { false };
    struct LoudnessHolder;
    std::unique_ptr<LoudnessHolder> masterLoudness;
    void applyAutomation (double seconds);   // inserts + fader/pan + meters on trackBuffer[0..1]
    void renderClips (Track&, juce::int64 pos, int numSamples);   // adds the track's clips into trackBuffer[0..1]
    void processAudio (const float* const* input, int numInputs, float* const* output, int numOutputs, int numSamples);
    std::atomic<bool> offlineActive { false };
    std::shared_ptr<const juce::AudioBuffer<float>> auditionBuffer;   // swapped under the lock
    std::atomic<bool> auditionOn { false };
    std::atomic<juce::int64> auditionPos { 0 };
    std::atomic<const AudioClip*> previewClip { nullptr };
    template <typename Fn> void forEachChannel (Fn&& fn) { fn (master); for (auto* t : tracks) fn (*t); }

    juce::CriticalSection lock;
    Track master { 0, "Master", true };
    juce::OwnedArray<Track> tracks;
    int nextTrackId = 1;

    std::atomic<bool> playing { false }, recording { false }, bounceDone { false };
    std::atomic<juce::int64> position { 0 };
    juce::int64 recordStart = 0;
    juce::int64 recordEnd = std::numeric_limits<juce::int64>::max();
    juce::int64 countInFrom = 0;              // click forced from here until recordStart
    std::atomic<bool> punchRecording { false };
    juce::int64 bounceEnd = 0;
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> blockSize { 512 };

    juce::TimeSliceThread writerThread { "VocalDAW disk writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> bounceWriter;

    juce::AudioBuffer<float> trackBuffer, masterBuffer, dryBuffer, inputScratch, busBuffers;
    static constexpr int kMaxSidechainTracks = 64;
    juce::AudioBuffer<float> sidechainBus;   // 2 channels per track: each track's output from the latest block
    juce::HeapBlock<float> silence;
    juce::HeapBlock<juce::uint8> coverage;
    int scratchSize = 0;
    juce::MidiBuffer midiScratch;

    void setClipBuffer (AudioClip&, AudioClip::BufferPtr);
    std::vector<std::pair<Track*, std::shared_ptr<AudioClip>>> newTakes;
    juce::ThreadPool renderPool { 1 };
    std::shared_ptr<int> aliveToken = std::make_shared<int> (0);
    std::atomic<int> pendingRenders { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
