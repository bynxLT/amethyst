#include "MainComponent.h"
#include "Presets.h"
#include "Plugins/Builtins.h"

#ifndef AMETHYST_SUPABASE_URL
 #define AMETHYST_SUPABASE_URL ""
#endif
#ifndef AMETHYST_SUPABASE_ANON_KEY
 #define AMETHYST_SUPABASE_ANON_KEY ""
#endif

namespace
{
    constexpr int kToolbarHeight = 70;
}

MainComponent::MainComponent()
{
    ctx.removeTrack = [this] (Track* t) { removeTrack (t); };
    ctx.showInMixer = [this] (Track* t) { setMixerVisible (true); mixer.select (t); };
    ctx.showPluginManager = [this] { showPluginManager(); };
    ctx.openClipEditor = [this] (Track* t, std::shared_ptr<AudioClip> c) { openClipEditor (t, std::move (c)); };

    juce::PropertiesFile::Options opts;
    opts.applicationName = "AMETHYST";
    opts.filenameSuffix = ".settings";
    opts.folderName = "AMETHYST";
    opts.osxLibrarySubFolder = "Application Support";

    {
        // carry the plug-in list and audio device over from the app's old name
        auto oldOpts = opts;
        oldOpts.applicationName = "VocalDAW";
        oldOpts.folderName = "VocalDAW";
        const auto oldFile = oldOpts.getDefaultFile(), newFile = opts.getDefaultFile();
        if (oldFile.existsAsFile() && ! newFile.exists())
        {
            newFile.getParentDirectory().createDirectory();
            oldFile.copyFileTo (newFile);
        }
    }
    appProperties.setStorageParameters (opts);

    auto* settings = appProperties.getUserSettings();

    if (auto xml = settings->getXmlValue ("pluginList"))
        engine.knownPlugins.recreateFromXml (*xml);
    for (const auto& d : Builtins::getDescriptions())   // AMETHYST's own effects always show up
        engine.knownPlugins.addType (d);
    engine.knownPlugins.addChangeListener (this);

    // Account + Pro gate
    account = std::make_unique<Account> (*settings, AMETHYST_SUPABASE_URL, AMETHYST_SUPABASE_ANON_KEY);
    account->addChangeListener (this);
    engine.isProUnlocked = [this] { return account->isPro(); };
    ctx.isPro = [this] { return account->isPro(); };
    ctx.showProPrompt = [this] (const juce::String& feature)
    {
        ProPrompt::show (*account, feature, this, [this] { AccountPanel::show (*account, this); });
    };
    accountBtn.onClick = [this] { AccountPanel::show (*account, this); };
    accountBtn.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    addAndMakeVisible (accountBtn);
    updateAccountButton();
    account->restoreSession();

    auto savedDevice = settings->getXmlValue ("audioDevice");
    const auto err = engine.deviceManager.initialise (2, 2, savedDevice.get(), true);
    if (err.isNotEmpty())
        juce::MessageManager::callAsync ([err]
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Audio device problem",
                                                    err + "\n\nOpen Audio settings to pick a device.");
        });
    engine.deviceManager.addAudioCallback (&engine);
    engine.deviceManager.addChangeListener (this);

    // Transport
    rtzBtn.setTooltip ("Return to start (Home)");
    rtzBtn.onClick = [this] { engine.setTransportPosition (0); view.startSeconds = 0.0; };
    stopBtn.setTooltip ("Stop (press again to return to start)");
    stopBtn.onClick = [this] { stopOrReturn(); };
    playBtn.setTooltip ("Play / pause (Space)");
    playBtn.onClick = [this] { togglePlay(); };
    recBtn.setTooltip ("Record armed tracks (R)");
    recBtn.onClick = [this] { toggleRecord(); };
    metroBtn.setTooltip ("Metronome");
    metroBtn.setClickingTogglesState (true);
    metroBtn.onClick = [this] { engine.metronomeOn = metroBtn.getToggleState(); };
    timeDisplay.setMouseCursor (juce::MouseCursor::PointingHandCursor);

    arrangeBtn.setTooltip ("Tracks and recordings (Tab toggles)");
    mixerBtn.setTooltip ("Mixer with plug-in slots (Tab toggles)");
    arrangeBtn.onClick = [this] { setMixerVisible (false); };
    mixerBtn.onClick = [this] { setMixerVisible (true); };
    arrangeBtn.setToggleState (true, juce::dontSendNotification);

   #if JUCE_MAC
    const juce::String cmdKey ("Cmd");
   #else
    const juce::String cmdKey ("Ctrl");
   #endif
    openBtn.setTooltip ("Open project (" + cmdKey + "+O)");
    openBtn.onClick = [this] { openProject(); };
    saveBtn.setTooltip ("Save project (" + cmdKey + "+S). Shift-click for Save As");
    saveBtn.onClick = [this] { saveProject (juce::ModifierKeys::currentModifiers.isShiftDown()); };
    exportBtn.setTooltip ("Export the mix to a WAV file (real-time)");
    exportBtn.onClick = [this] { exportMix(); };
    audioBtn.setTooltip ("Audio device, inputs, buffer size");
    audioBtn.onClick = [this] { showAudioSettings(); };
    pluginsBtn.setTooltip ("Scan and manage VST3 plug-ins");
    pluginsBtn.onClick = [this] { showPluginManager(); };

    masterK.slider.onValueChange = [this] { engine.getMaster().volumeDb = (float) masterK.slider.getValue(); };

    zoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    zoomSlider.setRange (5.0, 400.0, 0.1);
    zoomSlider.setSkewFactorFromMidPoint (60.0);
    zoomSlider.setValue (view.pixelsPerSecond, juce::dontSendNotification);
    zoomSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.setTooltip ("Timeline zoom");
    zoomSlider.onValueChange = [this] { view.pixelsPerSecond = zoomSlider.getValue(); };

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &rtzBtn, &stopBtn, &playBtn, &recBtn, &tempo, &metroBtn, &timeDisplay, &systemMeter,
             &arrangeBtn, &mixerBtn, &openBtn, &saveBtn, &exportBtn, &audioBtn, &pluginsBtn, &masterK,
             &ruler, &viewport, &hScroll, &zoomSlider })
        addAndMakeVisible (c);

    addChildComponent (mixer);
    mixer.onAddTrack = [this] { addTrack(); };

    viewport.setViewedComponent (&trackHolder, false);
    viewport.setScrollBarsShown (true, false);
    hScroll.setAutoHide (false);
    hScroll.addListener (this);

    addTrackRowBtn.setTooltip ("Add a new track");
    addTrackRowBtn.onClick = [this] { addTrack(); };
    trackHolder.addAndMakeVisible (addTrackRowBtn);

    disableFocusOnChildren (*this);
    addTrackRowBtn.setWantsKeyboardFocus (false);
    setWantsKeyboardFocus (true);

    addTrack ("Lead Vocal");

    setSize (1640, 880);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    clipEditor = nullptr;
    engine.stop();
    engine.knownPlugins.removeChangeListener (this);
    engine.deviceManager.removeChangeListener (this);
    account->removeChangeListener (this);
    engine.isProUnlocked = nullptr;
    engine.deviceManager.removeAudioCallback (&engine);

    pluginWindows.closeAll();
    trackComps.clear();
    appProperties.saveIfNeeded();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    if (backgroundCache.isNull() || backgroundCache.getWidth() != getWidth() || backgroundCache.getHeight() != getHeight())
    {
        backgroundCache = juce::Image (juce::Image::RGB, juce::jmax (1, getWidth()), juce::jmax (1, getHeight()), false);
        juce::Graphics bg (backgroundCache);
        Glass::background (bg, getLocalBounds());

        // toolbar glass strip + groups baked into the cache
        Glass::panel (bg, getLocalBounds().removeFromTop (kToolbarHeight).toFloat().reduced (6.0f, 4.0f), 16.0f, 0.9f);
        for (auto& grp : toolbarGroups)
            Glass::panel (bg, grp.toFloat(), 12.0f, 0.8f);

        drawLogoMark (bg, logoArea.withWidth (40).toFloat().reduced (2.0f));
        if (showWordmark)
        {
            auto word = logoArea.withTrimmedLeft (46);
            bg.setColour (Theme::text);
            bg.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            bg.drawText ("AMETHYST", word.removeFromTop (word.getHeight() / 2 + 4).withTrimmedTop (2), juce::Justification::bottomLeft);
            bg.setColour (Theme::accent);
            bg.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            bg.drawText ("VOCAL STUDIO", word, juce::Justification::topLeft);
        }
    }
    g.drawImageAt (backgroundCache, 0, 0);

    auto m = masterMeterArea.toFloat();
    drawMeterH (g, m.removeFromTop (m.getHeight() / 2.0f - 2.0f), masterLevelL);
    m.removeFromTop (4.0f);
    drawMeterH (g, m, masterLevelR);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    if (! dropHighlight.isEmpty())
    {
        g.setColour (Theme::accent.withAlpha (0.16f));
        g.fillRoundedRectangle (dropHighlight.toFloat(), 8.0f);
        g.setColour (Theme::accent);
        g.drawRoundedRectangle (dropHighlight.toFloat().reduced (1.0f), 8.0f, 2.0f);
    }
}

void MainComponent::resized()
{
    backgroundCache = {};
    toolbarGroups.clear();

    auto r = getLocalBounds();
    auto bar = r.removeFromTop (kToolbarHeight).reduced (18, 13);

    // Decide what fits
    const int fixedWidth = 44 + 12 + 190 + 12 + 118 + 8 + 44 + 12 + 196 + 12 + 150 + 30 + 146 + 12 + 190 + 12 + 150 + 12 + 140 + 94;
    showWordmark = bar.getWidth() >= fixedWidth + 110;
    const bool showSystem = bar.getWidth() >= fixedWidth - 40;
    systemMeter.setVisible (showSystem);

    auto group = [this] (juce::Rectangle<int> area) { toolbarGroups.push_back (area.expanded (5, 4)); };

    // Left side
    logoArea = bar.removeFromLeft (showWordmark ? 150 : 44);
    bar.removeFromLeft (12);

    {
        auto t = bar.removeFromLeft (190);
        group (t);
        const int w = (t.getWidth() - 12) / 4;
        for (auto* b : { &rtzBtn, &stopBtn, &playBtn, &recBtn })
        {
            b->setBounds (t.removeFromLeft (w));
            t.removeFromLeft (4);
        }
    }
    bar.removeFromLeft (16);

    tempo.setBounds (bar.removeFromLeft (118));
    bar.removeFromLeft (8);
    metroBtn.setBounds (bar.removeFromLeft (44));
    bar.removeFromLeft (16);
    timeDisplay.setBounds (bar.removeFromLeft (196));
    bar.removeFromLeft (12);
    if (showSystem)
    {
        systemMeter.setBounds (bar.removeFromLeft (150));
        bar.removeFromLeft (12);
    }

    // Right side
    {
        auto a = bar.removeFromRight (78);
        group (a);
        accountBtn.setBounds (a);
    }
    bar.removeFromRight (16);
    {
        auto m = bar.removeFromRight (140);
        group (m);
        masterK.setBounds (m.removeFromLeft (52));
        m.removeFromLeft (6);
        masterMeterArea = m.withSizeKeepingCentre (m.getWidth(), 18);
    }
    bar.removeFromRight (16);
    {
        auto s = bar.removeFromRight (150);
        group (s);
        pluginsBtn.setBounds (s.removeFromRight (80));
        s.removeFromRight (4);
        audioBtn.setBounds (s);
    }
    bar.removeFromRight (16);
    {
        auto f = bar.removeFromRight (190);
        group (f);
        exportBtn.setBounds (f.removeFromRight (66));
        f.removeFromRight (4);
        saveBtn.setBounds (f.removeFromRight (58));
        f.removeFromRight (4);
        openBtn.setBounds (f);
    }
    bar.removeFromRight (16);
    {
        auto v = bar.removeFromRight (146);
        group (v);
        mixerBtn.setBounds (v.removeFromRight (66));
        v.removeFromRight (4);
        arrangeBtn.setBounds (v);
    }

    // Content
    r.reduce (8, 4);
    mixer.setBounds (r);

    auto rulerArea = r.removeFromTop (28);
    ruler.setBounds (rulerArea);
    zoomSlider.setBounds (rulerArea.withWidth (kHeaderWidth).reduced (16, 6).withTrimmedLeft (60));
    zoomSlider.toFront (false);

    hScroll.setBounds (r.removeFromBottom (14).withTrimmedLeft (kHeaderWidth));
    viewport.setBounds (r);
    layoutTracks();
}

void MainComponent::layoutTracks()
{
    const int w = viewport.getMaximumVisibleWidth();
    const int contentH = kTrackHeight * trackComps.size() + 56;
    trackHolder.setSize (w, juce::jmax (contentH, viewport.getHeight()));
    for (int i = 0; i < trackComps.size(); ++i)
        trackComps[i]->setBounds (0, i * kTrackHeight, w, kTrackHeight);
    addTrackRowBtn.setBounds (12, trackComps.size() * kTrackHeight + 12, kHeaderWidth - 24, 32);
}

void MainComponent::setMixerVisible (bool shouldShow)
{
    mixer.setVisible (shouldShow);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &ruler, &viewport, &hScroll, &zoomSlider })
        c->setVisible (! shouldShow);

    mixerBtn.setToggleState (shouldShow, juce::dontSendNotification);
    arrangeBtn.setToggleState (! shouldShow, juce::dontSendNotification);
    if (shouldShow)
        mixer.rebuild();
}

void MainComponent::openClipEditor (Track* t, std::shared_ptr<AudioClip> c)
{
    if (t == nullptr || c == nullptr)
        return;

    if (clipEditor != nullptr && clipEditor->isFor (c.get()) && clipEditor->isVisible())
    {
        clipEditor->toFront (true);
        return;
    }

    clipEditor = std::make_unique<ClipEditorWindow> (ctx, *t, std::move (c));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)               { togglePlay(); return true; }
    if (key == juce::KeyPress::tabKey)                 { setMixerVisible (! mixer.isVisible()); return true; }
    if (key.getTextCharacter() == 'r' || key.getTextCharacter() == 'R') { toggleRecord(); return true; }
    if (key == juce::KeyPress::homeKey)                { engine.setTransportPosition (0); view.startSeconds = 0.0; return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0)) { saveProject (false); return true; }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { openProject(); return true; }
    return false;
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (engine.canImport (juce::File (f)))
            return true;
    return false;
}

TrackComponent* MainComponent::trackComponentAt (juce::Point<int> posInThis) const
{
    if (! viewport.isVisible() || ! viewport.getBounds().contains (posInThis))
        return nullptr;

    for (auto* tc : trackComps)
        if (tc->getLocalBounds().contains (tc->getLocalPoint (this, posInThis)))
            return tc;
    return nullptr;
}

void MainComponent::updateDropHighlight (int x, int y)
{
    const juce::Point<int> p (x, y);
    juce::Rectangle<int> newHighlight;

    if (mixer.isVisible())
    {
        newHighlight = mixer.getBounds().reduced (4);
    }
    else if (auto* tc = trackComponentAt (p))
    {
        newHighlight = getLocalArea (tc, tc->getLocalBounds().withTrimmedLeft (kHeaderWidth)).reduced (2);
    }
    else if (viewport.getBounds().contains (p))
    {
        const int lastBottom = trackComps.isEmpty() ? viewport.getY()
                                                    : getLocalArea (trackComps.getLast(), trackComps.getLast()->getLocalBounds()).getBottom();
        newHighlight = viewport.getBounds().withTop (juce::jmax (viewport.getY(), lastBottom)).withHeight (kTrackHeight).reduced (2);
    }

    if (newHighlight != dropHighlight)
    {
        dropHighlight = newHighlight;
        repaint();
    }
}

void MainComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropHighlight = {};
    repaint();

    const juce::Point<int> p (x, y);
    Track* target = nullptr;
    double startSec = (double) engine.getTransportPosition() / juce::jmax (1.0, engine.getSampleRate());

    if (mixer.isVisible())
    {
        auto* sel = mixer.getSelected();
        target = (sel != nullptr && ! sel->isMaster) ? sel : nullptr;
    }
    else if (auto* tc = trackComponentAt (p))
    {
        target = &tc->getTrack();
        const auto local = tc->getLocalPoint (this, p);
        if (tc->isInLane (local))
            startSec = tc->timeAtX (local.x);
    }
    else if (viewport.getBounds().contains (p) && p.x >= viewport.getX() + kHeaderWidth)
    {
        startSec = view.xToTime ((float) p.x, viewport.getBounds().withTrimmedLeft (kHeaderWidth));
    }

    importFiles (files, target, startSec);
}

void MainComponent::importFiles (const juce::StringArray& files, Track* target, double startSeconds)
{
    juce::StringArray failed;
    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const auto start = (juce::int64) std::llround (juce::jmax (0.0, startSeconds) * sr);

    for (auto& path : files)
    {
        juce::File file (path);
        if (! engine.canImport (file))
            continue;

        auto clip = engine.loadClip (file, start);
        if (clip == nullptr)
        {
            failed.add (file.getFileName());
            continue;
        }

        // first file goes onto the track it was dropped on, others get their own tracks
        Track* t = target != nullptr ? target : addTrack (file.getFileNameWithoutExtension());
        engine.addClip (*t, std::move (clip));
        target = nullptr;
    }

    if (mixer.isVisible())
        mixer.rebuild();

    if (! failed.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Some files couldn't be read",
                                                failed.joinIntoString ("\n"));
}

//==============================================================================
void MainComponent::timerCallback()
{
    if (engine.bounceReachedEnd())
    {
        engine.stop();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export finished", "The mix has been exported.");
    }

    if (clipEditor != nullptr && (! clipEditor->isVisible() || ! clipEditor->isValid()))
        clipEditor = nullptr;

    if (++accountRecheckTicks >= 30 * 60 * 30) // re-check the plan every 30 minutes
    {
        accountRecheckTicks = 0;
        account->refreshStatus();
    }

    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const auto pos = engine.getTransportPosition();

    playBtn.setIcon (engine.isPlaying() && ! engine.isRecording() ? IconButton::Icon::Pause : IconButton::Icon::Play);
    playBtn.setToggleState (engine.isPlaying(), juce::dontSendNotification);
    recBtn.setToggleState (engine.isRecording(), juce::dontSendNotification);
    metroBtn.setToggleState (engine.metronomeOn.load(), juce::dontSendNotification);
    exportBtn.setEnabled (! engine.isRecording());
    timeDisplay.repaint();
    tempo.repaint();
    systemMeter.tick();

    auto& master = engine.getMaster();
    if (! masterK.slider.isMouseButtonDown())
        masterK.slider.setValue (master.volumeDb.load(), juce::dontSendNotification);

    if (mixer.isVisible())
    {
        mixer.tick(); // the mixer strips consume the meter values
        masterLevelL *= 0.85f;
        masterLevelR *= 0.85f;
    }
    else
    {
        masterLevelL = juce::jmax (master.meterL.exchange (0.0f), masterLevelL * 0.85f);
        masterLevelR = juce::jmax (master.meterR.exchange (0.0f), masterLevelR * 0.85f);

        // Horizontal scroll + follow playhead
        const double visibleSecs = juce::jmax (1, hScroll.getWidth()) / view.pixelsPerSecond;
        const double posSecs = (double) pos / sr;

        if (engine.isPlaying() && (posSecs > view.startSeconds + visibleSecs * 0.95 || posSecs < view.startSeconds))
            view.startSeconds = juce::jmax (0.0, posSecs - visibleSecs * 0.05);

        const double total = juce::jmax ((double) engine.getSessionLengthSamples() / sr, posSecs) + visibleSecs;
        hScroll.setRangeLimits (0.0, total, juce::dontSendNotification);
        hScroll.setCurrentRange (view.startSeconds, visibleSecs, juce::dontSendNotification);

        for (auto* tc : trackComps)
            tc->tick();

        ruler.repaint();
    }

    repaint (masterMeterArea);

    // Keep transport shortcuts working - but only when the main window itself is the focused OS window.
    // Plug-in editors are native windows JUCE can't see into: stealing focus from them closes their
    // drop-down menus before a choice registers and pulls the main window in front of them.
    if (auto* peer = getPeer())
        if (peer->isFocused() && isShowing() && ! hasKeyboardFocus (true)
            && juce::Component::getCurrentlyFocusedComponent() == nullptr)
            grabKeyboardFocus();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    auto* settings = appProperties.getUserSettings();

    if (source == account.get())
    {
        updateAccountButton();
        mixer.pluginListChanged(); // lock badges in the browser
        return;
    }

    if (source == &engine.knownPlugins)
    {
        if (auto xml = engine.knownPlugins.createXml())
            settings->setValue ("pluginList", xml.get());
        settings->saveIfNeeded();
        mixer.pluginListChanged();
    }
    else if (source == &engine.deviceManager)
    {
        if (auto xml = engine.deviceManager.createStateXml())
            settings->setValue ("audioDevice", xml.get());
        settings->saveIfNeeded();

        for (auto* tc : trackComps)
            tc->refreshInputs();
        if (mixer.isVisible())
            mixer.rebuild();
    }
}

void MainComponent::updateAccountButton()
{
    if (! account->isSignedIn())
    {
        accountBtn.setButtonText ("Sign in");
        accountBtn.setToggleState (false, juce::dontSendNotification);
        accountBtn.setTooltip ("Sign in or create an AMETHYST account");
    }
    else
    {
        const bool pro = account->isPro();
        accountBtn.setButtonText (pro ? "PRO" : "FREE");
        accountBtn.setToggleState (pro, juce::dontSendNotification);
        accountBtn.setTooltip (account->getEmail() + "  -  " + account->getPlanText());
    }
}

void MainComponent::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    view.startSeconds = juce::jmax (0.0, newRangeStart);
}

//==============================================================================
Track* MainComponent::addTrack (const juce::String& name)
{
    auto* t = engine.addTrack (name.isNotEmpty() ? name : "Vocal " + juce::String (engine.getTracks().size() + 1));
    auto* tc = trackComps.add (new TrackComponent (ctx, *t));
    trackHolder.addAndMakeVisible (tc);
    layoutTracks();
    if (mixer.isVisible())
    {
        mixer.rebuild();
        mixer.select (t);
    }
    return t;
}

void MainComponent::removeTrack (Track* t)
{
    if (engine.isRecording())
        return;

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete track",
                                        "Delete \"" + t->name + "\", its clips and its plug-ins?", "Delete", "Cancel", nullptr,
                                        juce::ModalCallbackFunction::create ([safe, t] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        auto& self = *safe;
        for (int i = 0; i < self.trackComps.size(); ++i)
        {
            if (&self.trackComps[i]->getTrack() == t)
            {
                self.clipEditor = nullptr;
                self.pluginWindows.closeAllFor (*t);
                if (self.mixer.getSelected() == t)
                    self.mixer.select (&self.engine.getMaster());
                self.trackComps.remove (i);
                self.engine.removeTrack (t);
                break;
            }
        }
        self.layoutTracks();
        self.mixer.rebuild();
    }));
}

void MainComponent::rebuildTrackComponents()
{
    trackComps.clear();
    for (auto* t : engine.getTracks())
    {
        auto* tc = trackComps.add (new TrackComponent (ctx, *t));
        trackHolder.addAndMakeVisible (tc);
    }
    layoutTracks();
    mixer.rebuild();
}

//==============================================================================
void MainComponent::togglePlay()
{
    if (engine.isRecording())
        return;

    if (engine.isPlaying())
        engine.stop();
    else
        engine.play();
}

void MainComponent::toggleRecord()
{
    if (engine.isRecording())
    {
        engine.stop();
        return;
    }

    if (engine.isBouncing())
        return;

    if (engine.isPlaying())
        engine.stop();

    if (! engine.record())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Nothing to record",
                                                "Arm a track first with its red R button.");
}

void MainComponent::stopOrReturn()
{
    if (engine.isPlaying() || engine.isRecording())
        engine.stop();
    else
    {
        engine.setTransportPosition (0);
        view.startSeconds = 0.0;
    }
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 16, 2, 2,
                                                             false, false, false, false);
    selector->setSize (560, 480);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::showPluginManager()
{
    auto* settings = appProperties.getUserSettings();
    auto deadMansPedal = settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");

    auto* list = new juce::PluginListComponent (engine.pluginFormats, engine.knownPlugins, deadMansPedal, settings, true);
    list->setSize (780, 520);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (list);
    o.dialogTitle = "VST3 Plug-ins  -  use Options > Scan for new or updated VST3 plug-ins";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::saveProject (bool forceChooser)
{
    auto doSave = [this] (const juce::File& f)
    {
        auto result = ProjectIO::save (engine, f);
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save failed", result.getErrorMessage());
        else
        {
            projectFile = f;
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setName ("AMETHYST - " + f.getFileNameWithoutExtension());
        }
    };

    if (projectFile != juce::File() && ! forceChooser)
    {
        doSave (projectFile);
        return;
    }

    AudioEngine::getProjectsFolder().createDirectory();
    ctx.chooser = std::make_unique<juce::FileChooser> ("Save project", AudioEngine::getProjectsFolder().getChildFile (juce::String ("Untitled") + ProjectIO::fileExtension),
                                                       juce::String ("*") + ProjectIO::fileExtension);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [doSave] (const juce::FileChooser& fc)
    {
        if (fc.getResult() != juce::File())
            doSave (fc.getResult().withFileExtension (ProjectIO::fileExtension));
    });
}

void MainComponent::openProject()
{
    if (engine.isRecording())
        return;

    AudioEngine::getProjectsFolder().createDirectory();
    ctx.chooser = std::make_unique<juce::FileChooser> ("Open project", AudioEngine::getProjectsFolder(), ProjectIO::openWildcard);

    juce::Component::SafePointer<MainComponent> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (safe == nullptr || ! file.existsAsFile())
            return;

        auto& self = *safe;
        self.clipEditor = nullptr;
        self.pluginWindows.closeAll();
        self.mixer.select (&self.engine.getMaster());
        self.trackComps.clear();

        auto result = ProjectIO::load (self.engine, file);
        self.projectFile = file;

        self.metroBtn.setToggleState (self.engine.metronomeOn.load(), juce::dontSendNotification);
        self.view.startSeconds = 0.0;
        self.rebuildTrackComponents();

        if (auto* w = self.findParentComponentOfClass<juce::DocumentWindow>())
            w->setName ("AMETHYST - " + file.getFileNameWithoutExtension());

        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Project opened with problems", result.getErrorMessage());
    });
}

void MainComponent::exportMix()
{
    if (engine.getSessionLengthSamples() <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Nothing to export", "Record or import something first.");
        return;
    }

    auto defaultName = projectFile != juce::File() ? projectFile.getFileNameWithoutExtension() : juce::String ("Mix");
    ctx.chooser = std::make_unique<juce::FileChooser> ("Export mix", AudioEngine::getAppFolder().getChildFile (defaultName + ".wav"), "*.wav");

    juce::Component::SafePointer<MainComponent> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;
        if (! safe->engine.startBounce (fc.getResult().withFileExtension (".wav")))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export failed", "Couldn't create the output file.");
    });
}
