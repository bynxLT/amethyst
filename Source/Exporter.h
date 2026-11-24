#pragma once

#include "AudioEngine.h"

// Song export: renders the full song offline (fast, with a progress window) and writes
// WAV, FLAC, OGG, MP3 or M4A.
namespace Exporter
{
    enum class Format { Wav = 0, Mp3, Ogg, Flac, M4a };

    juce::String displayName (Format);
    juce::String extension (Format);   // includes the dot
    bool isAvailable (Format);

    // Writes an already-rendered stereo buffer to `file`.
    juce::Result write (const juce::AudioBuffer<float>&, double sampleRate, Format, const juce::File& file);

    // Renders the song and writes it, showing a progress window with Cancel.
    void exportSong (AudioEngine&, Format, const juce::File&, juce::Component* centreAround, std::function<void (bool ok)> onDone = {});
}
