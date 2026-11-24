#pragma once

#include "AudioEngine.h"
#include "Theme.h"

class PluginWindowManager;

constexpr int kHeaderWidth = 260;   // compact list-style track headers
constexpr int kTrackHeight = 60;
constexpr int kMixerStripWidth = 76;
constexpr int kSlotPanelWidth = 330;
constexpr int kBrowserWidth = 230;

enum class EditTool { Move, Trim, Cut };

// Magnet snap sizes, in beats ("step" = a 16th note = a quarter beat).
struct SnapOption { const char* name; double beats; };
inline const std::array<SnapOption, 12> kSnapOptions { {
    { "(none)", 0.0 },
    { "1/6 step", 0.25 / 6.0 }, { "1/4 step", 0.0625 }, { "1/3 step", 0.25 / 3.0 }, { "1/2 step", 0.125 }, { "Step", 0.25 },
    { "1/6 beat", 1.0 / 6.0 },  { "1/4 beat", 0.25 },   { "1/3 beat", 1.0 / 3.0 },  { "1/2 beat", 0.5 },   { "Beat", 1.0 },
    { "Bar", 4.0 },
} };

struct TimelineView
{
    double pixelsPerSecond = 40.0;
    double startSeconds = 0.0;
    EditTool tool = EditTool::Move;
    int snapIndex = 10; // Beat

    double snapBeats() const { return kSnapOptions[(size_t) juce::jlimit (0, (int) kSnapOptions.size() - 1, snapIndex)].beats; }

    // Snap a time to the grid. Holding Shift (bypass) turns the magnet off for that drag.
    double snap (double seconds, double bpm, bool bypass) const
    {
        const double beats = snapBeats();
        if (bypass || beats <= 0.0)
            return seconds;
        const double unit = beats * 60.0 / juce::jmax (20.0, bpm);
        return std::round (seconds / unit) * unit;
    }

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
    std::function<bool()> isPro;                                   // AMETHYST Pro (or Ultra) active?
    std::function<int()> tier;                                     // 0 Free, 1 Pro, 2 Ultra Pro
    std::function<void (const juce::String& what)> checkpoint;     // call before any clip edit (undo)
    std::function<void()> discardUnchangedCheckpoint;
    std::function<void (const juce::String& feature, int requiredTier)> showProPrompt;
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
