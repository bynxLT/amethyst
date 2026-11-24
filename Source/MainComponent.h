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

    // Audio file drag & drop
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override { updateDropHighlight (x, y); }
    void fileDragMove (const juce::StringArray&, int x, int y) override  { updateDropHighlight (x, y); }
    void fileDragExit (const juce::StringArray&) override                { dropHighlight = {}; repaint(); }
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
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
    void placeNewTakes();
    void undoEdit (bool redo);

    void showAudioSettings();
    void showPluginManager();
    void saveProject (bool forceChooser);
    void openProject();
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
    juce::TextButton openBtn { "Open" }, saveBtn { "Save" }, exportBtn { "Export" };
    juce::TextButton audioBtn { "Audio" }, pluginsBtn { "Plug-ins" };
    AccountBadge accountBtn;   // profile picture + plan
    int accountRecheckTicks = 0;
    void updateAccountButton();
    LabeledKnob masterK { "Master", -60.0, 12.0, 0.0, " dB", 0.1, -12.0 };
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

    juce::Image backgroundCache;
    juce::Rectangle<int> dropHighlight;
    juce::File projectFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
