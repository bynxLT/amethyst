#pragma once

#include "AppContext.h"

// AMETHYST 2026 feature hub. Owned by MainComponent and laid over it (transparent to clicks except
// for its own panels). Holds the command registry, command palette (Ctrl+K), shortcut sheet (F1),
// toasts, the lyrics panel, a status pill, and the actions behind the 2026 features:
// loop & markers, monitor modes, metronome options, retrospective capture, bounce in place, stems,
// loudness-matched export, reference A/B, Smart Vocal Chain, reverse-reverb swells, input level check,
// user templates and project version history.
class Studio2026 : public juce::Component, private juce::Timer
{
public:
    struct Host
    {
        std::function<void()> refresh;                                   // rebuild track rows + mixer
        std::function<Track*()> selectedTrack;                           // the track tools act on (may be null)
        std::function<Track* (const juce::String&)> addTrack;
        std::function<juce::File()> projectFile;
        std::function<void (const juce::File& file, const juce::File& saveAs)> openAs;   // saveAs empty = untitled
        std::function<void (const juce::String& title, const juce::String& text)> report;
    };

    struct Command
    {
        juce::String name, group, keys;
        std::function<void()> run;
        int tier = 0;   // 1 Pro, 2 Ultra (shown in the palette)
    };

    Studio2026 (AppContext&, juce::ApplicationProperties&, Host);
    ~Studio2026() override;

    void addCommand (Command);
    juce::StringArray getShortcutLines() const;   // "Ctrl+K    Command palette" ...
    bool keyPressed (const juce::KeyPress&) override;   // the 2026 shortcuts; MainComponent asks first
    void toast (const juce::String& text, bool warning = false);
    void showPalette();
    void showShortcuts();
    void toggleLyrics();
    bool isLyricsOpen() const;
    void sessionChanged();                    // after open / new: reload lyrics etc.
    void setStatusArea (juce::Rectangle<int> r) { statusArea = r; repaint(); }

    // for --ui-test
    bool isPaletteOpen() const;
    bool isShortcutsOpen() const;
    int getNumCommands() const { return (int) commands.size(); }
    int getNumToasts() const { return toasts.size(); }
    void closeOverlays();
    void setQuiet (bool q) { quiet = q; }   // tests: no prompts or report windows, everything unlocked

    // Menus (items carry their own actions)
    void addToToolsMenu (juce::PopupMenu&);
    void addToFileMenu (juce::PopupMenu&);
    void addToSettingsMenu (juce::PopupMenu&);
    void addToExportMenu (juce::PopupMenu&);
    void addMetronomeMenu (juce::PopupMenu&);

    // Project versions: call before writing `file` so the previous save is kept.
    void archiveBeforeSave (const juce::File& file);

    // Feature actions
    void toggleLoop();
    void addMarkerAtPlayhead();
    void jumpMarker (int direction);
    void cycleMonitorMode();
    void captureRetro();
    void trackAction (Track&, int action);    // 1 bounce, 2 smart chain, 3 input level, 4 retro
    void clipAction (Track&, std::shared_ptr<AudioClip>, int action);
    void loadReference();
    void toggleReference();
    void exportStems();
    void smartVocalChain (Track&);
    void bounceInPlace (Track&);
    void reverseReverbSwell (Track&, std::shared_ptr<AudioClip>);
    void startInputLevelCheck (Track&);
    void saveAsTemplate();

    // Clipboard (Ctrl+C / X / V / D, Delete): the selected clips, or the highlighted range of a track
    bool copySelection (bool cut);
    void paste();
    void duplicateSelection();
    void deleteSelection();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool hitTest (int x, int y) override;

private:
    void timerCallback() override;
    bool requireTier (int tier, const juce::String& feature);
    void registerOwnCommands();
    juce::File templatesFolder() const;
    juce::File versionsFolderFor (const juce::File& project) const;
    void layoutToasts();

    AppContext& ctx;
    juce::ApplicationProperties& props;
    Host host;
    std::vector<Command> commands;
    juce::Rectangle<int> statusArea;

    struct Toast;
    juce::OwnedArray<Toast> toasts;
    class Palette;
    std::unique_ptr<Palette> palette;
    class ShortcutSheet;
    std::unique_ptr<ShortcutSheet> sheet;
    class LyricsPanel;
    std::unique_ptr<LyricsPanel> lyrics;

    bool quiet = false;

    // input level check
    Track* levelTrack = nullptr;
    bool levelWasArmed = false;
    float levelPeak = 0.0f;
    double levelEndsAt = 0.0;
    int levelLastShown = -1;
};
