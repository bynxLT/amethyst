#pragma once

#include "UiHelpers.h"

// Channel header (left) + timeline lane with the track's clips (right).
class TrackComponent : public juce::Component, private juce::Timer
{
public:
    TrackComponent (AppContext&, Track&);

    Track& getTrack() noexcept { return track; }

    void refreshInputs();
    void tick();

    bool isInLane (juce::Point<int> p) const { return laneBounds().contains (p); }
    double timeAtX (int x) const { return ctx.view.xToTime ((float) x, laneBounds()); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Thumb
    {
        const juce::AudioBuffer<float>* buffer = nullptr;
        std::unique_ptr<juce::AudioThumbnail> thumbnail;
    };

    juce::Rectangle<int> laneBounds() const { return getLocalBounds().withTrimmedLeft (kHeaderWidth); }
    juce::Rectangle<float> clipBounds (const AudioClip&) const;
    std::shared_ptr<AudioClip> clipAt (juce::Point<int>) const;
    int edgeAt (const AudioClip&, juce::Point<int>) const;   // -1 left edge, +1 right edge, 0 none
    juce::int64 sampleAtX (int x, bool snap) const;
    void updateCursor (juce::Point<int>);
    void syncThumbnails();
    void showClipMenu (std::shared_ptr<AudioClip>, juce::int64 clickSample);
    void showLaneMenu();
    void handleClipMenu (const std::shared_ptr<AudioClip>&, int result, juce::int64 clickSample, juce::int64 playhead);
    void showRenameBox (const std::shared_ptr<AudioClip>&);
    void splitAtSilences (const std::shared_ptr<AudioClip>&, bool removeSilentParts);
    std::unique_ptr<juce::AlertWindow> renameBox;
    void splitAt (const std::shared_ptr<AudioClip>&, juce::int64 at);

    void timerCallback() override;               // auto-scroll while dragging a clip into an edge
    void applyDrag (int x, bool bypassSnap);
    int lastDragX = 0;
    bool lastBypassSnap = false;

    enum class Drag { None, Move, TrimStart, TrimEnd };
    Drag dragMode = Drag::None;
    float hoverX = -1.0f;                     // cut-tool preview line
    const AudioClip* hoverEdgeClip = nullptr; // trim handle highlight
    int hoverEdgeSide = 0;

    AppContext& ctx;
    Track& track;

    // Compact list-style header: name, on/off + record-arm lights, "..." menu.
    juce::Label nameLabel;
    LedButton onLed { Theme::ledOn }, armLed { Theme::record };
    juce::TextButton menuBtn { "..." };
    juce::ComboBox inputBox;   // hidden; holds the device's input list for the menu
    void showTrackMenu();

    std::map<const AudioClip*, Thumb> thumbs;
    std::weak_ptr<AudioClip> draggingClip;
    double dragGrabOffsetSec = 0.0;

    float levelL = 0.0f, levelR = 0.0f;

    JUCE_DECLARE_NON_COPYABLE (TrackComponent)
};

// Bar ruler above the lanes; click or drag to move the playhead. Dragging into either
// edge scrolls the timeline; Ctrl + wheel zooms, Shift + wheel scrolls sideways.
class RulerComponent : public juce::Component, private juce::Timer
{
public:
    explicit RulerComponent (AppContext& c) : ctx (c) {}

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { dragX = e.x; bypassSnap = e.mods.isShiftDown(); seekTo (e.x); startTimerHz (40); }
    void mouseDrag (const juce::MouseEvent& e) override { dragX = e.x; bypassSnap = e.mods.isShiftDown(); seekTo (e.x); }
    void mouseUp (const juce::MouseEvent&) override     { stopTimer(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    void seekTo (int x);
    AppContext& ctx;
    int dragX = 0;
    bool bypassSnap = false;
};

// Ctrl + wheel = zoom at the mouse, Shift + wheel (or a sideways swipe) = scroll. Returns true if handled.
bool handleTimelineWheel (TimelineView&, const juce::MouseEvent&, const juce::MouseWheelDetails&, juce::Rectangle<int> lane);
