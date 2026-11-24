#pragma once

#include "UiHelpers.h"
#include "Widgets.h"

// Per-clip properties: gain/pan/pitch, time stretching and rendered ("baked") effects.
class ClipEditor : public juce::Component, private juce::Timer
{
public:
    ClipEditor (AppContext&, Track&, std::shared_ptr<AudioClip>);
    ~ClipEditor() override;

    bool isValid() const;
    bool isFor (const AudioClip* c) const { return clip.lock().get() == c; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void loadFromSettings();
    void storeToSettings();
    void changed();
    void showPage (int);
    void updateThumbnail();
    void replaceFile();

    AppContext& ctx;
    Track& track;
    std::weak_ptr<AudioClip> clip;

    // header
    LedButton onLed;
    LabeledKnob panK  { "Pan", -1.0, 1.0, 0.0, {}, 0.01 };
    LabeledKnob volK  { "Vol", -60.0, 12.0, 0.0, " dB", 0.1, -12.0 };
    LabeledKnob pitchK { "Pitch", -24.0, 24.0, 0.0, " st", 1.0 };

    // file
    IconButton replaceBtn { IconButton::Icon::Folder }, revealBtn { IconButton::Icon::Reveal }, deleteBtn { IconButton::Icon::Trash };

    // render options
    juce::ComboBox declickBox;
    juce::TextButton resetBtn { "Reset all" };

    // time stretching
    juce::ComboBox modeBox;
    LabeledKnob fineK  { "Fine", -100.0, 100.0, 0.0, " ct", 1.0 };
    LabeledKnob speedK { "Speed", 0.5, 2.0, 1.0, "x", 0.01, 1.0 };

    // effects pages
    juce::TextButton shapeTab { "Shape" }, colorTab { "Color" };
    juce::ToggleButton removeDC { "Remove DC offset" }, normalize { "Normalize" }, reverse { "Reverse" },
                       invert { "Invert polarity" }, stereoFade { "Stereo fade" }, swapStereo { "Swap stereo" };
    LabeledKnob startK   { "Start", 0.0, 100.0, 0.0, " %", 0.1 };
    LabeledKnob lengthK  { "Length", 0.0, 100.0, 100.0, " %", 0.1 };
    LabeledKnob fadeInK  { "Fade in", 0.0, 2000.0, 0.0, " ms", 1.0, 200.0 };
    LabeledKnob fadeOutK { "Fade out", 0.0, 5000.0, 0.0, " ms", 1.0, 300.0 };
    LabeledKnob trimK    { "Trim", -90.0, -20.0, -90.0, " dB", 0.5 };

    LabeledKnob boostK    { "Boost", -12.0, 24.0, 0.0, " dB", 0.1 };
    LabeledKnob eqK       { "Tone", -1.0, 1.0, 0.0, {}, 0.01 };
    LabeledKnob ringMixK  { "Ring mix", 0.0, 1.0, 0.0, {}, 0.01 };
    LabeledKnob ringFreqK { "Ring freq", 20.0, 5000.0, 440.0, " Hz", 1.0, 400.0 };
    LabeledKnob cutK      { "Cut", 100.0, 20000.0, 20000.0, " Hz", 1.0, 2000.0 };
    LabeledKnob resK      { "Res", 0.0, 1.0, 0.0, {}, 0.01 };
    juce::ToggleButton softClip { "Soft clip" };
    juce::TextButton revA { "A" }, revB { "B" };
    LabeledKnob reverbK { "Reverb", 0.0, 1.0, 0.0, {}, 0.01 };
    LabeledKnob delayK  { "Delay", 0.0, 1.0, 0.0, {}, 0.01 };
    LabeledKnob pogoK   { "Pogo", -1.0, 1.0, 0.0, {}, 0.01 };

    juce::Array<juce::Component*> shapePage, colorPage;
    int page = 0;

    juce::AudioThumbnail thumbnail;
    const juce::AudioBuffer<float>* thumbBuffer = nullptr;
    int renderCountdown = 0;
    bool loading = false;

    juce::Image backgroundCache;
    juce::Rectangle<int> headerArea, fileCard, renderCard, timeCard, fxCard, waveCard;
    juce::Rectangle<int> shapeContent, colorContent;

    JUCE_DECLARE_NON_COPYABLE (ClipEditor)
};

class ClipEditorWindow : public juce::DocumentWindow
{
public:
    ClipEditorWindow (AppContext&, Track&, std::shared_ptr<AudioClip>);
    void closeButtonPressed() override { setVisible (false); }
    bool isValid() const { return editor != nullptr && editor->isValid(); }
    bool isFor (const AudioClip* c) const { return editor != nullptr && editor->isFor (c); }

private:
    ClipEditor* editor = nullptr;
};
