#pragma once

#include "AppContext.h"
#include "PluginWindows.h"
#include "TrackComponent.h"
#include "Mixer.h"
#include "Transport.h"
#include "ClipEditor.h"
#include "AccountUI.h"
#include "EditHistory.h"
#include "Exporter.h"
#include "DiscordPresence.h"
#include "Updater.h"

class Studio2026;

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ChangeListener,
                      private juce::ScrollBar::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isTransportPlaying() const { return engine.isPlaying(); }
    void openFileFromSystem (const juce::File&);   // from the command line / another launch
    static bool& isTestInstance() { static bool b = false; return b; }   // test windows never touch autosave
    int runUiSelfTest (const juce::File& snapshotFolder);                 // --ui-test; returns failures

    // Audio file drag & drop
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override { updateDropHighlight (x, y); }
    void fileDragMove (const juce::StringArray&, int x, int y) override  { updateDropHighlight (x, y); }
    void fileDragExit (const juce::StringArray&) override                { dropHighlight = {}; repaint(); }
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void importAudioFiles (const juce::StringArray&, Track* target, double startSeconds);

private:
    bool pluginKeyHookInstalled = false;
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    Track* addTrack (const juce::String& name = {});
    void removeTrack (Track*);
    void rebuildTrackComponents();
    void layoutTracks();
    void setMixerVisible (bool);
    void openClipEditor (Track*, std::shared_ptr<AudioClip>);

    TrackComponent* trackComponentAt (juce::Point<int> posInThis) const;
    void updateDropHighlight (int x, int y);
    void importFiles (const juce::StringArray&, Track* target, double startSeconds);

    void togglePlay();
    void toggleRecord();
    void stopOrReturn();
    void stopTransport();     // stops; after recording, records an undo step and places the new takes
    juce::int64 playStartPos = -1;   // where the current playback / recording started (return on stop)
    void returnToPlayStart();
    void placeNewTakes();
    void undoEdit (bool redo);

    void showAudioSettings();
    void showPluginManager();
    void saveProject (bool forceChooser);
    void addAutomation (Track& target, Automation::Kind, int slot, int param);
    bool locatePlugin (juce::AudioProcessor&, Track*& track, int& slot);
    TrackComponent* findAutomationLane (Track& target, Automation::Kind, int slot, int param);
    void automationAction (juce::AudioProcessor&, int param, int action);
    void addLastTweakedMenu (juce::PopupMenu&);
    void newProject();        // asks first if the current one has work in it
    void startNewProject (const juce::StringArray& trackNames = {});
    void openProject();
    void showTextReport (const juce::String& title, const juce::String& text);
    void exportMix();             // shows the export format menu
    void exportAs (int format);   // Exporter::Format index

    // Declaration order matters: the engine must outlive everything that holds plug-ins or thumbnails.
    AudioEngine engine;
    EditHistory history { engine };
    juce::ApplicationProperties appProperties;
    std::unique_ptr<Account> account;   // after appProperties: it writes to the settings file
    PluginWindowManager pluginWindows;
    TimelineView view;
    juce::AudioThumbnailCache thumbCache { 64 };
    AppContext ctx { engine, pluginWindows, view, thumbCache };

    // Transport bar
    IconButton rtzBtn { IconButton::Icon::ReturnToStart }, stopBtn { IconButton::Icon::Stop },
               playBtn { IconButton::Icon::Play, juce::Colour (0xff7d5cff) }, recBtn { IconButton::Icon::Record, Theme::record },
               metroBtn { IconButton::Icon::Metronome, Theme::accent };
    TempoDisplay tempo { engine };
    TimeDisplay timeDisplay { engine };
    SystemMeter systemMeter { engine };
    juce::TextButton arrangeBtn { "Arrange" }, mixerBtn { "Mixer" };
    juce::TextButton toolsBtn { "Tools" };
    IconButton countInBtn { IconButton::Icon::CountIn, Theme::accent }, punchBtn { IconButton::Icon::Punch, Theme::record };
    IconButton settingsBtn { IconButton::Icon::Gear, Theme::accent };
    juce::TextButton fileBtn { "File" };
    void showFileMenu();
    void showWelcome();
    void startTemplate (int kind);   // 0 empty, 1 vocal session, 2 mix a beat
    void startTemplate (int kind, bool confirmed);
    std::unique_ptr<juce::Component> welcome;
    bool recoveryOffered = false, skipWelcome = false;
    void showSettingsMenu();
    std::unique_ptr<DiscordPresence> discord;
    std::unique_ptr<Updater> updater;
    void checkForUpdates (bool userAsked);
    void startUpdate (const Updater::Release&);
    bool keepAutosaveOnExit = false;   // set before an update so the session can be restored afterwards
    int updateTicks = 0;
    void updateDiscord();
    void askDiscordAppId();
    std::unique_ptr<juce::AlertWindow> discordIdBox;
    int discordTicks = 0;
    void applyColourTheme (int index, bool save);
    juce::RecentlyOpenedFilesList recentProjects;
    void noteRecentProject (const juce::File&);
    void openProjectFile (const juce::File&);
    juce::Rectangle<int> displayArea;   // the centred glass display (tempo + time)
    int countInBars = 0;
    void setCountIn (int bars);

    // Autosave + crash recovery: a copy of the session every 2 minutes. A lock file marks a running
    // session; if it's still there at the next start, AMETHYST didn't close properly and offers the copy.
    static juce::File autosaveFolder() { return AudioEngine::getAppFolder().getChildFile ("Autosave"); }
    static juce::File autosaveFile()   { return autosaveFolder().getChildFile ("session.amethyst"); }
    static juce::File autosaveInfo()   { return autosaveFolder().getChildFile ("session.info"); }
    static juce::File lockFile()       { return autosaveFolder().getChildFile ("running.lock"); }
    void autosaveNow();
    void checkForCrashRecovery();
    void restoreAutosave (const juce::File& originalProject);
    int autosaveTicks = 0;
    bool sessionHasWork();
    void finishPunch (std::vector<std::pair<Track*, std::shared_ptr<AudioClip>>>& takes);
    void showToolsMenu();
    void analyseSong();
    void applyMasterPreset (int style);   // -1 = remove Polish from the master
    void addAudioFileAsTrack (const juce::File&);
    juce::TextButton newBtn { "New" }, openBtn { "Open" }, saveBtn { "Save" }, exportBtn { "Export" };
    juce::TextButton audioBtn { "Audio" }, pluginsBtn { "Plug-ins" };
    AccountBadge accountBtn;   // profile picture + plan
    int accountRecheckTicks = 0;
    void updateAccountButton();
    LabeledKnob masterK { "Master", -60.0, 24.0, 0.0, " dB", 0.1, -6.0 };
    juce::Slider zoomSlider;
    juce::TextButton moveToolBtn { "Move" }, trimToolBtn { "Trim" }, cutToolBtn { "Cut" };
    MagnetButton snapBtn;
    void setTool (EditTool);
    void setSnap (int index);
    void showSnapMenu();
    juce::Rectangle<int> masterMeterArea, logoArea;
    std::vector<juce::Rectangle<int>> toolbarGroups;
    bool showWordmark = true;
    float masterLevelL = 0.0f, masterLevelR = 0.0f;

    RulerComponent ruler { ctx };
    juce::Viewport viewport;
    juce::Component trackHolder;
    juce::OwnedArray<TrackComponent> trackComps;
    juce::TextButton addTrackRowBtn { "+   Add track" };
    juce::ScrollBar hScroll { false };
    MixerView mixer { ctx };
    juce::TooltipWindow tooltips { this, 600 };

    std::unique_ptr<ClipEditorWindow> clipEditor;
    std::unique_ptr<Studio2026> studio;   // AMETHYST 2026 hub, laid over everything
    std::unique_ptr<juce::Component> splash;   // startup animation (StartupSplash)
    Track* selectedTrackForTools();
    void registerCommands();

    juce::Image backgroundCache;
    juce::Image customBackground;      // user's picture, downscaled; empty = the default glass gradient
    float backgroundHaze = 0.6f;       // 0 = clear picture, 1 = mostly purple haze
    void loadCustomBackground();
    void chooseBackground();
    void setBackgroundHaze (float);
    void paintHazedBackground (juce::Graphics&, juce::Rectangle<int>);
    std::set<void*> styledWindows;

    // Shortcuts from any AMETHYST window (plug-in editors, dialogs) come here when nothing there uses the key.
    struct ForwardKeys : public juce::KeyListener
    {
        explicit ForwardKeys (MainComponent& m) : owner (m) {}
        bool keyPressed (const juce::KeyPress& k, juce::Component*) override { return owner.keyPressed (k); }
        MainComponent& owner;
    };
    ForwardKeys forwardKeys { *this };
    int titleBarTick = 14;     // native windows whose title bar we've coloured
    juce::Rectangle<int> dropHighlight;
    juce::File projectFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
