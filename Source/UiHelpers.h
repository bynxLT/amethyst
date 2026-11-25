#pragma once

#include "AppContext.h"

// Small round on/off button (used for slot enable LEDs and record-arm).
class LedButton : public juce::Button
{
public:
    explicit LedButton (juce::Colour onColour = Theme::ledOn) : juce::Button ({}), colour (onColour)
    {
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const float d = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (d, d);
        if (getToggleState())
        {
            g.setColour (colour.withAlpha (0.35f));
            g.fillEllipse (r.expanded (2.5f));
        }
        g.setColour (getToggleState() ? colour : juce::Colour (0xff2a2142));
        g.fillEllipse (r);
        g.setColour (juce::Colours::white.withAlpha (over ? 0.45f : 0.2f));
        g.drawEllipse (r, 1.0f);
        if (getToggleState())
        {
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.fillEllipse (r.reduced (d * 0.3f).translated (-d * 0.08f, -d * 0.08f));
        }
    }

private:
    juce::Colour colour;
};

namespace UiHelpers
{
    // Fills an input selector with the device's active inputs and selects the track's current one.
    void fillInputBox (juce::ComboBox&, AudioEngine&, Track&);
    void applyInputSelection (const juce::ComboBox&, Track&);

    // Channel preset menu: save / load / browse. onChanged runs after a preset is loaded.
    void showPresetMenu (AppContext&, Track&, juce::Component& target, std::function<void()> onChanged);
    void savePresetAs (AppContext&, Track&);
    void loadPreset (AppContext&, Track&, const juce::File&, std::function<void()> onChanged);   // also takes FL .fst presets
    juce::File flPresetFolder();   // where FL Studio keeps the user's presets (or Downloads)

    // "Detected tempo" dialog: sets the exact tempo, or its half / double (beats can be heard either way).
    void offerTempo (AudioEngine&, double bpm, const juce::String& title, const juce::String& extraText = {});
    juce::String formatBpm (double bpm);   // "140" or "87.5"

    // Pops up the scanned plug-in list; calls back with the chosen one.
    void choosePlugin (AppContext&, juce::Component& target, std::function<void (const juce::PluginDescription&)> onChosen);

    // Creates the plug-in and puts it in the slot (closing the editor of whatever was there).
    bool loadPluginIntoSlot (AppContext&, Track&, int slot, const juce::PluginDescription&, bool openEditor);

    // Reads a .vst3 file/bundle and returns its first plug-in description (also adds it to the known list).
    bool describeVst3File (AppContext&, const juce::File&, juce::PluginDescription& out);
}
