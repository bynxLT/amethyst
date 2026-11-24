#pragma once

#include "AudioEngine.h"

// Channel presets: a track's fader/pan plus its full insert chain
// (plug-in identity, bypass state and each plug-in's saved state).
namespace ChannelPresets
{
    constexpr const char* fileExtension = ".vchp";

    juce::File getFolder();
    juce::Array<juce::File> list();

    std::unique_ptr<juce::XmlElement> toXml (const Track&, const juce::String& presetName);
    juce::Result applyXml (AudioEngine&, Track&, const juce::XmlElement&);

    juce::Result save (const Track&, const juce::File&);
    juce::Result load (AudioEngine&, Track&, const juce::File&);
}

// Whole-session save/load (.vdaw): tracks, routing, channel strips and takes.
namespace ProjectIO
{
    constexpr const char* fileExtension = ".amethyst";
    constexpr const char* openWildcard = "*.amethyst;*.vdaw";   // older VocalDAW projects still open

    juce::Result save (AudioEngine&, const juce::File&);
    juce::Result load (AudioEngine&, const juce::File&);
}
