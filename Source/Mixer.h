#pragma once

#include "UiHelpers.h"
#include "Widgets.h"

class MixerView;
class SlotPanel;

//==============================================================================
// Searchable list of scanned effect plug-ins. Drag an entry onto a slot, or double-click it.
class PluginBrowser : public juce::Component,
                      private juce::ListBoxModel
{
public:
    explicit PluginBrowser (AppContext&);

    void refresh();
    std::function<void (const juce::PluginDescription&)> onPluginDoubleClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return filtered.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    AppContext& ctx;
    juce::TextEditor search;
    juce::TextButton scanBtn { "Scan..." };
    juce::ListBox list;
    juce::Array<juce::PluginDescription> filtered;
};

//==============================================================================
// One insert slot row: plug-in name, wet/dry knob, enable LED. Accepts plug-in drags and .vst3 files.
class SlotRow : public juce::Component,
                public juce::DragAndDropTarget,
                public juce::FileDragAndDropTarget
{
public:
    SlotRow (SlotPanel&, int index);

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override  { dragOver = false; repaint(); }
    void itemDropped (const SourceDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override            { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    InsertSlot* getSlot() const;

    SlotPanel& owner;
    const int index;
    juce::Slider mixKnob;
    LedButton led;
    bool dragOver = false, dragStarted = false;
};

//==============================================================================
// Right-hand panel: the selected channel's 10 insert slots plus channel presets.
class SlotPanel : public juce::Component
{
public:
    explicit SlotPanel (AppContext&);

    void setTrack (Track*);
    Track* getTrack() const noexcept { return track; }
    void refresh();

    void slotClicked (int index, juce::Component& target);
    void showSlotMenu (int index, juce::Component& target);
    void dropPlugin (int index, const juce::String& description);
    void dropVst3File (int index, const juce::File&);
    void loadFirstFree (const juce::PluginDescription&);

    void paint (juce::Graphics&) override;
    void resized() override;

    AppContext& ctx;
    std::function<void()> onChannelChanged;

private:
    Track* track = nullptr;
    juce::Label inputLabel { {}, "Input" };
    juce::ComboBox inputBox;
    juce::OwnedArray<SlotRow> rows;
    juce::TextButton savePresetBtn { "Save preset..." }, loadPresetBtn { "Presets" };
    juce::Label hint;
};

//==============================================================================
// A vertical channel strip: name, record arm, pan, fader, meters, mute/solo.
class MixerStrip : public juce::Component
{
public:
    MixerStrip (MixerView&, Track&, int number);

    Track& getTrack() noexcept { return track; }
    void setSelected (bool);
    void tick();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    MixerView& owner;
    Track& track;
    const int number;
    bool selected = false;
    float levelL = 0.0f, levelR = 0.0f;

    LedButton armLed { Theme::record };
    ValueSlider panKnob { "Pan" }, fader { "Volume" };
    juce::TextButton muteBtn { "M" }, soloBtn { "S" };
    juce::Rectangle<int> meterArea, nameArea;
};

//==============================================================================
class MixerView : public juce::Component
{
public:
    explicit MixerView (AppContext&);

    void rebuild();
    void select (Track*);
    Track* getSelected() const noexcept { return slotPanel.getTrack(); }
    void pluginListChanged() { browser.refresh(); }
    void tick();

    void paint (juce::Graphics&) override;
    void resized() override;

    AppContext& ctx;
    std::function<void()> onAddTrack;

private:
    PluginBrowser browser;
    juce::Viewport stripViewport;
    juce::Component stripHolder;
    juce::OwnedArray<MixerStrip> strips;
    juce::TextButton addBtn { "+" };
    SlotPanel slotPanel;
};
