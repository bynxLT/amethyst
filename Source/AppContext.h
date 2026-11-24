#pragma once

#include "AudioEngine.h"
#include "Theme.h"

class PluginWindowManager;

constexpr int kHeaderWidth = 440;
constexpr int kTrackHeight = 96;
constexpr int kMixerStripWidth = 76;
constexpr int kSlotPanelWidth = 330;
constexpr int kBrowserWidth = 230;

struct TimelineView
{
    double pixelsPerSecond = 40.0;
    double startSeconds = 0.0;

    float timeToX (double seconds, juce::Rectangle<int> lane) const
    {
        return (float) (lane.getX() + (seconds - startSeconds) * pixelsPerSecond);
    }

    double xToTime (float x, juce::Rectangle<int> lane) const
    {
        return juce::jmax (0.0, startSeconds + (x - (float) lane.getX()) / pixelsPerSecond);
    }
};

// Shared services handed to the UI components.
struct AppContext
{
    AudioEngine& engine;
    PluginWindowManager& pluginWindows;
    TimelineView& view;
    juce::AudioThumbnailCache& thumbCache;
    std::function<void (Track*)> removeTrack;
    std::function<void (Track*)> showInMixer;
    std::function<void()> showPluginManager;
    std::function<void (Track*, std::shared_ptr<AudioClip>)> openClipEditor;
    std::function<bool()> isPro;                                   // AMETHYST Pro active?
    std::function<void (const juce::String& feature)> showProPrompt;
    std::unique_ptr<juce::FileChooser> chooser; // keeps async file dialogs alive
};

// Stop child widgets stealing keyboard focus so Space / R / Home always reach the transport.
inline void disableFocusOnChildren (juce::Component& parent)
{
    for (auto* c : parent.getChildren())
    {
        c->setWantsKeyboardFocus (false);
        c->setMouseClickGrabsKeyboardFocus (false);
    }
}
