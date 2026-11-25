#pragma once

#include "AudioEngine.h"
#include "Theme.h"

// One tabbed Settings window (Ctrl+,): Audio, Recording, Appearance, Files & export, Online, Shortcuts.
// Every change applies straight away and is saved in the same settings the menus use.
namespace SettingsWindow
{
    struct Host
    {
        AudioEngine* engine = nullptr;
        juce::ApplicationProperties* props = nullptr;
        std::function<int()> tier;
        std::function<void (const juce::String& feature, int tier)> proPrompt;
        std::function<void (int)> setTheme;            // colour theme index (already allowed for the tier)
        std::function<int()> getIconStyle;             // 0 gem, 1 anime angel
        std::function<void (int)> setIconStyle;
        std::function<void()> chooseBackground, removeBackground;
        std::function<void (float)> setHaze;
        std::function<int()> getCountIn;
        std::function<void (int)> setCountIn;
        std::function<void()> checkUpdates, openPluginManager;
        std::function<void()> discordChanged;          // re-read the Discord settings
        std::function<juce::String()> shortcutsText;
    };

    void show (Host, juce::Component* centreAround, int initialTab = 0);

    // The window's content on its own (for offscreen snapshots in the UI test).
    std::unique_ptr<juce::Component> create (Host, int initialTab = 0);

    // "What's new in <version>" card shown once after an update.
    void showWhatsNew (const juce::String& version, const juce::StringArray& items, juce::Component* centreAround);
}
