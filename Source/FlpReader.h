#pragma once

#include <JuceHeader.h>

// Reads the parts of an FL Studio project (.flp) that AMETHYST can use:
// tempo, audio channels, playlist audio clips, and the mixer (names, levels, effect slots).
// Own implementation, written from the file's event layout as observed in real projects.
namespace Flp
{
    struct Channel
    {
        int iid = -1;
        int type = 0;
        juce::String name, samplePath;   // samplePath as stored (may contain %FLStudioFactoryData%)
        int insert = -1;                 // mixer insert it plays through; -1 = not stored
        float volume = 1.0f;             // linear gain relative to FL's default (10000)
        float pan = 0.0f;                // -1..+1
    };

    struct PlaylistItem
    {
        juce::int64 position = 0, length = 0;   // ticks
        int channel = -1;                       // channel iid (audio clip); -1 for patterns
        int pattern = -1;                       // pattern iid; -1 for channel clips
        int track = 0;                          // 0-based playlist track
        double startOffsetMs = -1.0;            // into the sample; < 0 = not set
        bool muted = false;
    };

    struct Plugin
    {
        juce::String internalName;   // e.g. "Fruity Reeverb 2" or "Fruity Wrapper"
        juce::String displayName;    // what the user called it
        juce::String vstName, vstPath, vstVendor;   // Fruity Wrapper only
        juce::MemoryBlock vstState;
        juce::MemoryBlock state;     // native FL effect settings (event 213), translated by FlpImport
        int slot = -1;
        bool enabled = true;
        float mix = 1.0f;
        bool isWrapper() const { return internalName == "Fruity Wrapper"; }
        juce::String getName() const { return vstName.isNotEmpty() ? vstName : internalName; }
    };

    struct Insert
    {
        int index = 0;               // 0 = master
        juce::String name;
        std::vector<Plugin> plugins;
        float volume = 1.0f;         // linear (12800 = unity)
        float pan = 0.0f;            // -1..+1
        bool muted = false;
    };

    struct Project
    {
        juce::String version, title;
        double tempo = 120.0;
        int ppq = 96;
        std::map<int, Channel> channels;
        std::vector<PlaylistItem> items;      // current arrangement
        std::map<int, juce::String> trackNames; // 0-based playlist track -> name
        std::vector<Insert> inserts;           // index == position

        int eventCount = 0;
        bool used172Fix = false;

        double ticksToSeconds (juce::int64 ticks) const { return (double) ticks * 60.0 / (juce::jmax (1.0, tempo) * juce::jmax (1, ppq)); }
        const Insert* getInsert (int index) const
        {
            for (auto& i : inserts) if (i.index == index) return &i;
            return nullptr;
        }
    };

    // Parses `file`. On failure the Result says why.
    juce::Result read (const juce::File& file, Project& out);
    juce::Result read (const juce::MemoryBlock& data, Project& out);

    // An FL Studio preset (.fst): a saved mixer track state (a whole effect chain, with fader and pan)
    // or a single effect preset. Either way it comes back as one Insert.
    struct Preset
    {
        juce::String version;
        Insert insert;
        bool hasLevels = false;      // fader / pan were stored (mixer track states only)
        bool isMixerState = false;   // a whole chain rather than one effect's preset
    };
    juce::Result readPreset (const juce::File& file, Preset& out);
    juce::Result readPreset (const juce::MemoryBlock& data, Preset& out);

    // Turns a stored sample path into a real file (expands %FLStudioFactoryData% and similar,
    // and falls back to the project's folder / its Audio sub-folder by file name).
    juce::File resolveSamplePath (const juce::String& stored, const juce::File& projectFile);
}
