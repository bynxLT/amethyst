#pragma once

#include "AudioEngine.h"
#include "FlpReader.h"

// Builds an AMETHYST session from an FL Studio project.
//  - tempo, playlist audio clips (position + trims), mixer inserts as channels (name, volume, pan)
//  - FL native effects become the closest AMETHYST effect (default settings for now)
//  - VST plug-ins load by name when installed and scanned (their FL settings aren't carried over yet)
//  - patterns / MIDI / instruments are skipped and listed in the report
namespace FlpImport
{
    struct Report
    {
        juce::String projectName, flVersion;
        double tempo = 0.0;
        int tracksCreated = 0, clipsImported = 0;
        juce::StringArray translated;      // "Fruity Reeverb 2 -> Geode Reverb (protect, slot 10)"
        juce::StringArray notTranslated;   // effects we couldn't bring over, with the reason
        juce::StringArray missingAudio;    // sample files that couldn't be found
        juce::StringArray skipped;         // whole categories we don't import (patterns, ...)
        juce::String presetTarget;         // set for an .fst preset: the channel it went onto

        juce::String toText() const;
    };

    // The AMETHYST effect id an FL effect maps to ("" = no equivalent).
    juce::String mapNativeEffect (const juce::String& flInternalName);

    // Replaces the current session. Call on the message thread.
    juce::Result importProject (AudioEngine&, const juce::File& flpFile, Report&);

    // Loads an FL Studio preset (.fst) onto a channel. A mixer track state replaces the channel's
    // effects (and sets its fader / pan); a single effect preset goes into the first free slot.
    juce::Result importPreset (AudioEngine&, Track&, const juce::File& fstFile, Report&);

    bool isFlPreset (const juce::File&);
}
