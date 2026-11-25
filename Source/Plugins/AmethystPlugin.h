#pragma once

#include "../Widgets.h"

// Base class for Amethyst's built-in effects. They are real AudioPluginInstances,
// so they live in the same insert slots as VST3s and save/restore the same way.
class AmethystPlugin : public juce::AudioPluginInstance
{
public:
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    struct PresetValue
    {
        PresetValue (const char* paramId, double v) : id (paramId), value ((float) v) {}
        juce::String id;
        float value;
    };

    struct Preset
    {
        juce::String name;
        std::vector<PresetValue> values; // real-unit values; unlisted params use defaults
    };

    // Editor layout: a titled glass card of controls. Floats become knobs, bools toggles,
    // choices segmented buttons (<= 4 options) or a drop-down.
    struct Section
    {
        juce::String title;
        juce::StringArray params;
        int row = 0;
        int knobSize = 58;
    };

    AmethystPlugin (const juce::String& id, const juce::String& displayName, const juce::String& categoryName,
                    const juce::String& tagline, Layout layout);

    //==========================================================================
    const juce::String getName() const override { return name; }
    void fillInPluginDescription (juce::PluginDescription&) const override;
    double getTailLengthSeconds() const override { return tailSeconds; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override    { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    int getNumPrograms() override { return juce::jmax (1, (int) presets.size()); }
    int getCurrentProgram() override { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void releaseResources() override {}

    // Measures input / output level for the editor's meters around process().
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) final
    {
        const int n = buffer.getNumSamples();
        storeMaxLevel (inLevel, buffer.getMagnitude (0, n));
        process (buffer, midi);
        storeMaxLevel (outLevel, buffer.getMagnitude (0, n));
    }
    virtual void process (juce::AudioBuffer<float>&, juce::MidiBuffer&) = 0;
    std::atomic<float> inLevel { 0.0f }, outLevel { 0.0f };   // peak since the editor last read it

    // Sidechain: the engine publishes every track's output (track numbers as in the track list, from 1).
    // Called on the audio thread. A track later in the list arrives one block late, which is fine for ducking.
    using SidechainSource = std::function<bool (int trackNumber, int numSamples, const float*& left, const float*& right)>;
    static SidechainSource& sidechainSource() { static SidechainSource s; return s; }
    static void*& sidechainOwner() { static void* o = nullptr; return o; }

    // A/B compare: two complete settings snapshots; switching stores the current one first.
    void selectAB (int slot);
    void copyCurrentToOtherAB();
    int getABSlot() const noexcept { return abSlot; }

    //==========================================================================
    void applyPreset (const Preset&);
    void randomize();
    void setParam (const juce::String& id, float realValue);
    juce::File getUserPresetFolder() const;
    bool saveUserPreset (const juce::File&);
    bool loadUserPreset (const juce::File&);

    // Optional custom display shown between the header and the controls.
    virtual std::unique_ptr<juce::Component> createVisual() { return {}; }
    virtual int getVisualHeight() const { return 0; }
    virtual int getMinimumEditorWidth() const { return 0; }

    float get (const char* id) const   { return params.at (id)->load(); }
    int   getInt (const char* id) const { return (int) std::lround (params.at (id)->load()); }
    bool  getBool (const char* id) const { return params.at (id)->load() > 0.5f; }

    const juce::String pluginId, name, category, tagline;
    juce::UndoManager undoManager;            // parameter undo/redo (used by editors with undo buttons)
    juce::AudioProcessorValueTreeState apvts;
    std::vector<Preset> presets;
    std::vector<Section> sections;
    juce::String currentPresetName { "Default" };
    int currentPreset = 0;
    double tailSeconds = 0.0;

protected:
    double sampleRate = 48000.0;
    double getHostBpm() const;

private:
    std::map<std::string, std::atomic<float>*> params;
    std::array<juce::MemoryBlock, 2> abStates;
    int abSlot = 0;
    static void storeMaxLevel (std::atomic<float>& a, float v) { if (v > a.load()) a.store (v); }
};

//==============================================================================
// Parameter helpers
namespace AmParams
{
    std::unique_ptr<juce::AudioParameterFloat> num (const juce::String& id, const juce::String& name, float min, float max, float def,
                                                    const juce::String& unit = {}, int decimals = 1, float skewCentre = 0.0f, float interval = 0.0f);
    std::unique_ptr<juce::AudioParameterChoice> choice (const juce::String& id, const juce::String& name, const juce::StringArray& options, int def);
    std::unique_ptr<juce::AudioParameterBool> toggle (const juce::String& id, const juce::String& name, bool def);
}

//==============================================================================
// Scrolling level / gain-reduction display used by dynamics-type effects.
class MeterHistory : public juce::Component, private juce::Timer
{
public:
    MeterHistory (std::atomic<float>& inputPeakDb, std::atomic<float>& gainReductionDb, std::function<float()> thresholdDb);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    std::atomic<float>& input;
    std::atomic<float>& gr;
    std::function<float()> threshold;
    std::vector<float> inHist, grHist;
    float lastGr = 0.0f;
};

// Peak-hold helper for meters written on the audio thread and read (and reset) by the UI.
inline void storeMax (std::atomic<float>& a, float v)
{
    if (v > a.load())
        a.store (v);
}

// Gentle ceiling: transparent below 80% of the ceiling, smooth tanh knee above.
inline float softCeiling (float x, float ceiling)
{
    const float knee = 0.8f * ceiling;
    const float ax = std::abs (x);
    if (ax <= knee)
        return x;
    const float range = ceiling - knee;
    return std::copysign (knee + range * std::tanh ((ax - knee) / range), x);
}
