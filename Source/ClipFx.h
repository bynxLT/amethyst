#pragma once

#include <JuceHeader.h>

// Non-destructive per-clip processing. The clip keeps its decoded source;
// these settings are rendered offline into the buffer that actually plays.
#define VOCALDAW_CLIP_FLOATS(X) \
    X (gainDb, 0.0f) X (pan, 0.0f) X (pitchSemis, 0.0f) \
    X (fineCents, 0.0f) X (speed, 1.0f) \
    X (startPct, 0.0f) X (lengthPct, 100.0f) X (fadeInMs, 0.0f) X (fadeOutMs, 0.0f) X (trimDb, -90.0f) \
    X (boostDb, 0.0f) X (eqTilt, 0.0f) X (ringMix, 0.0f) X (ringFreq, 440.0f) X (cutHz, 20000.0f) X (resonance, 0.0f) \
    X (reverb, 0.0f) X (delay, 0.0f) X (pogo, 0.0f)

#define VOCALDAW_CLIP_BOOLS(X) \
    X (removeDC, false) X (normalize, false) X (reverse, false) X (invert, false) \
    X (stereoFade, false) X (swapStereo, false) X (softClip, false)

#define VOCALDAW_CLIP_INTS(X) \
    X (timeMode, 0) X (declick, 1) X (reverbType, 0)

struct ClipSettings
{
   #define X(name, def) float name = def;
    VOCALDAW_CLIP_FLOATS (X)
   #undef X
   #define X(name, def) bool name = def;
    VOCALDAW_CLIP_BOOLS (X)
   #undef X
   #define X(name, def) int name = def;
    VOCALDAW_CLIP_INTS (X)
   #undef X

    // timeMode: 0 = Resample (speed changes pitch), 1 = Stretch (keeps pitch)
    // declick:  0 = off, 1 = short (3 ms), 2 = smooth (20 ms)
    // reverbType: 0 = A (room), 1 = B (hall)

    bool operator== (const ClipSettings& o) const;
    bool operator!= (const ClipSettings& o) const { return ! (*this == o); }
    bool isNeutral() const { return *this == ClipSettings(); }

    void writeTo (juce::XmlElement&) const;
    void readFrom (const juce::XmlElement&);
};

namespace ClipFx
{
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, const ClipSettings&, double sampleRate, double bpm);

    // Estimated tempo of a region (onset-envelope autocorrelation), 70..180 BPM; 0 if unsure.
    double detectTempo (const juce::AudioBuffer<float>&, int start, int length, double sampleRate);

    // Estimated musical key (chroma + Krumhansl-Schmuckler key profiles). tonic: 0 = C .. 11 = B, -1 if unsure.
    struct KeyResult
    {
        int tonic = -1;
        bool minor = false;
        float confidence = 0.0f;   // 0..1, how clearly this key beat the runner-up
        juce::String name() const;
    };
    KeyResult detectKey (const juce::AudioBuffer<float>&, int start, int length, double sampleRate);

    // Silent stretches (below thresholdDb for at least minSilenceMs) as [start, end) sample ranges within the region.
    std::vector<std::pair<int, int>> findSilences (const juce::AudioBuffer<float>&, int start, int length, double sampleRate,
                                                   float thresholdDb = -45.0f, double minSilenceMs = 200.0);
}
