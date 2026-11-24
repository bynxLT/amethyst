#pragma once

#include "UiHelpers.h"

// Channel header (left) + timeline lane with the track's clips (right).
class TrackComponent : public juce::Component
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
    void splitAt (const std::shared_ptr<AudioClip>&, juce::int64 at);

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

// Bar ruler above the lanes; click or drag to move the playhead.
class RulerComponent : public juce::Component
{
public:
    explicit RulerComponent (AppContext& c) : ctx (c) {}

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { seek (e); }
    void mouseDrag (const juce::MouseEvent& e) override { seek (e); }

private:
    void seek (const juce::MouseEvent&);
    AppContext& ctx;
};
