#pragma once

#include "Widgets.h"

// Start screen shown over the main window: start something new, reopen a recent project,
// see what's new. Dismissed by any action, Escape, or the close button.
class WelcomeScreen : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void()> newEmpty, vocalSession, beatSession, open, importFl, close;
        std::function<void (const juce::File&)> openRecent;
        std::function<void (bool)> setShowAtStart;
    };

    WelcomeScreen (Callbacks, juce::Array<juce::File> recent, int tier, juce::String planName, bool showAtStart);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;   // clicking outside the card closes it

private:
    juce::Rectangle<int> card() const;

    struct Tile : public juce::Button
    {
        Tile (const juce::String& t, const juce::String& sub, int glyph, bool locked = false)
            : juce::Button (t), title (t), subtitle (sub), glyphKind (glyph), isLocked (locked) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
        juce::String title, subtitle;
        int glyphKind;
        bool isLocked;
    };

    struct RecentRow : public juce::Button
    {
        explicit RecentRow (const juce::File& f) : juce::Button (f.getFileName()), file (f) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
        juce::File file;
    };

    Callbacks cb;
    int tier;
    juce::String plan;
    juce::OwnedArray<Tile> tiles;
    juce::OwnedArray<RecentRow> recentRows;
    juce::TextButton openBtn { "Open project..." }, importBtn { "Import FL Studio..." }, closeBtn { "Close" };
    juce::ToggleButton showToggle { "Show at start" };
};
