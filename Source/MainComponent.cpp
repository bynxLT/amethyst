#include "MainComponent.h"
#include "GlassLookAndFeel.h"
#include "FlpImport.h"
#include "Studio2026.h"
#include "StartupSplash.h"
#include "LicenseCheck.h"
#include "SettingsWindow.h"
#include "Plugins/Builtins.h"
#include "UiHelpers.h"
#include "NightcoreMaker.h"
#include "WelcomeScreen.h"
#include "LastTweaked.h"
#include "ClipFx.h"
#include "Plugins/Builtins.h"

bool amethystInstallPluginKeyHook (void* mainWindow, void (*handler) (void*), void* context);
void amethystStyleTitleBar (void* window, unsigned int caption, unsigned int text, unsigned int border);
void amethystRemovePluginKeyHook();
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
    ctx.checkpoint = [this] (const juce::String& what) { history.checkpoint (what); };
    ctx.discardUnchangedCheckpoint = [this] { history.discardIfUnchanged(); };

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
    engine.unlockedTier = [this] { return account->getTier(); };
    ctx.isPro = [this] { return account->isPro(); };
    ctx.tier = [this] { return account->getTier(); };
    ctx.showProPrompt = [this] (const juce::String& feature, int requiredTier)
    {
        ProPrompt::show (*account, feature, requiredTier, this, [this] { AccountPanel::show (*account, this); });
    };
    accountBtn.onClick = [this] { AccountPanel::show (*account, this); };
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
    fileBtn.setTooltip ("New, open, save, import FL Studio projects, recent projects");
    fileBtn.onClick = [this] { showFileMenu(); };
    settingsBtn.setTooltip ("Settings: audio device, plug-ins, colour theme, background");
    settingsBtn.onClick = [this] { showSettingsMenu(); };
    GlassLookAndFeel::makePrimary (exportBtn);
    mixerBtn.setClickingTogglesState (false);
    countInBtn.setTooltip ("Count-in before recording: click to change (off, 1 bar, 2 bars)");
    countInBtn.onClick = [this] { setCountIn ((countInBars + 1) % 3); };
    punchBtn.setTooltip ("Punch-in: only record inside the punch region. Ctrl-drag on the ruler to set it, or right-click the ruler");
    punchBtn.setClickingTogglesState (true);
    punchBtn.onClick = [this]
    {
        engine.punchEnabled = punchBtn.getToggleState();
        if (engine.punchEnabled.load() && ! engine.hasPunchRegion())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Punch-in",
                                                    "Ctrl-drag across the ruler (above the tracks) to mark the part you want to re-record.");
        ruler.repaint();
    };
    metroBtn.setTooltip ("Metronome");
    metroBtn.setClickingTogglesState (true);
    metroBtn.onClick = [this] { engine.metronomeOn = metroBtn.getToggleState(); };
    timeDisplay.setMouseCursor (juce::MouseCursor::PointingHandCursor);

    arrangeBtn.setTooltip ("Tracks and recordings (Tab toggles)");
    mixerBtn.setTooltip ("Mixer with plug-in slots (Tab toggles)");
    arrangeBtn.onClick = [this] { setMixerVisible (false); };
    mixerBtn.onClick = [this] { setMixerVisible (! mixer.isVisible()); };
    arrangeBtn.setToggleState (true, juce::dontSendNotification);

   #if JUCE_MAC
    const juce::String cmdKey ("Cmd");
   #else
    const juce::String cmdKey ("Ctrl");
   #endif
    toolsBtn.setTooltip ("Nightcore Maker, tempo & key finder, master presets, master automation");
    toolsBtn.onClick = [this] { showToolsMenu(); };
    newBtn.setTooltip ("New empty project (" + cmdKey + "+N)");
    newBtn.onClick = [this] { newProject(); };
    openBtn.setTooltip ("Open project or import an FL Studio .flp (" + cmdKey + "+O)");
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

    moveToolBtn.setTooltip ("Move tool (V): drag clips; drag clip edges to trim");
    trimToolBtn.setTooltip ("Trim tool (T): drag the left or right half of a clip to trim that edge. Shift snaps to beats");
    cutToolBtn.setTooltip ("Cut tool (C): click a clip to split it. Shift snaps to beats");
    moveToolBtn.onClick = [this] { setTool (EditTool::Move); };
    trimToolBtn.onClick = [this] { setTool (EditTool::Trim); };
    cutToolBtn.onClick = [this] { setTool (EditTool::Cut); };
    compToolBtn.setTooltip ("Comp tool (X): swipe across a take to keep that part - the other takes on this channel are muted there. Click keeps a whole take");
    compToolBtn.onClick = [this] { setTool (EditTool::Comp); };
    for (auto* b : { &moveToolBtn, &trimToolBtn, &cutToolBtn, &compToolBtn })
        addAndMakeVisible (b);
    setTool (EditTool::Move);

    snapBtn.setTooltip ("Magnet snap: clips, trims, cuts and the playhead snap to this grid. Hold Shift to drag freely");
    snapBtn.onClick = [this] { showSnapMenu(); };
    addAndMakeVisible (snapBtn);
    setSnap (appProperties.getUserSettings()->getIntValue ("snapIndex", 10));
    zoomSlider.onValueChange = [this] { view.pixelsPerSecond = zoomSlider.getValue(); };

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &rtzBtn, &stopBtn, &playBtn, &recBtn, &tempo, &metroBtn, &countInBtn, &punchBtn, &timeDisplay, &systemMeter,
             &arrangeBtn, &mixerBtn, &newBtn, &openBtn, &saveBtn, &exportBtn, &audioBtn, &pluginsBtn, &toolsBtn, &fileBtn, &settingsBtn, &masterK,
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

    ctx.createAutomation = [this] (Track& target, Automation::Kind kind, int slot, int param) { addAutomation (target, kind, slot, param); };
    ctx.showReport = [this] (const juce::String& title, const juce::String& text) { showTextReport (title, text); };

    // AMETHYST 2026 feature hub (command palette, toasts, lyrics, Pro tools)
    {
        Studio2026::Host host;
        host.refresh = [this] { rebuildTrackComponents(); if (mixer.isVisible()) mixer.rebuild(); repaint(); };
        host.selectedTrack = [this] { return selectedTrackForTools(); };
        host.addTrack = [this] (const juce::String& name) { return addTrack (name); };
        host.projectFile = [this] { return projectFile; };
        host.openAs = [this] (const juce::File& f, const juce::File& saveAs)
        {
            openProjectFile (f);
            projectFile = saveAs;
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setName (kAppTitle + " - " + (saveAs != juce::File() ? saveAs.getFileNameWithoutExtension() : juce::String ("Untitled")));
        };
        host.report = [this] (const juce::String& title, const juce::String& text) { showTextReport (title, text); };
        studio = std::make_unique<Studio2026> (ctx, appProperties, std::move (host));
        history.beforeReplacingPlugins = [this] (Track& t) { pluginWindows.closeAllFor (t); };
        addAndMakeVisible (*studio);
        registerCommands();
    }
    // right-click a knob in an AMETHYST effect -> Automation section of its menu
    ValueSlider::automationActions().hasClip = [this] (juce::AudioProcessor& p, int param)
    {
        Track* t = nullptr; int slot = -1;
        return locatePlugin (p, t, slot) && findAutomationLane (*t, Automation::Kind::Param, slot, param) != nullptr;
    };
    ValueSlider::automationActions().perform = [this] (juce::AudioProcessor& p, int param, int action) { automationAction (p, param, action); };

    disableFocusOnChildren (*this);
    addTrackRowBtn.setWantsKeyboardFocus (false);
    setWantsKeyboardFocus (true);

    for (int i = 1; i <= 8; ++i)   // start with a ready-made list of tracks
        addTrack ("Track " + juce::String (i));

    setSize (1640, 880);
    recentProjects.setMaxNumberOfItems (10);
    recentProjects.restoreFromString (appProperties.getUserSettings()->getValue ("recentProjects"));
    loadCustomBackground();
    setCountIn (appProperties.getUserSettings()->getIntValue ("countInBars", 0));
    checkForCrashRecovery();
    if (! isTestInstance())
    {
        updater = std::make_unique<Updater> (juce::String (AMETHYST_SUPABASE_URL));
        if (updater->isAvailable() && appProperties.getUserSettings()->getBoolValue ("autoUpdate", true))
        {
            juce::Component::SafePointer<MainComponent> safe (this);
            juce::Timer::callAfterDelay (8000, [safe] { if (safe != nullptr) safe->checkForUpdates (false); });
        }

        // Discord: an id typed in Settings wins over the one built in
        discord = std::make_unique<DiscordPresence>();
        const auto savedId = appProperties.getUserSettings()->getValue ("discordAppId");
        discord->setApplicationId (savedId.isNotEmpty() ? savedId : juce::String (AMETHYST_DISCORD_APP_ID));
        discord->setEnabled (appProperties.getUserSettings()->getBoolValue ("discordEnabled", true));
    }
    {
        // colour theme (only the ones this plan unlocks), then the welcome screen
        const int saved = juce::jlimit (0, Theme::kNumThemes - 1, appProperties.getUserSettings()->getIntValue ("colourTheme", 0));
        const int tier = ctx.tier ? ctx.tier() : 0;
        applyColourTheme (tier >= Theme::kThemes[saved].tier ? saved : 0, false);
        applyIconStyle (appProperties.getUserSettings()->getIntValue ("iconStyle", 0), false);
        addChildComponent (mascotPeek);
        logoHit.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        logoHit.onClick = [this]
        {
            const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
            logoClicks = now - lastLogoClick < 0.6 ? logoClicks + 1 : 1;
            lastLogoClick = now;
            if (logoClicks >= 5) { logoClicks = 0; peekMascot ("logo", true); }
        };
        addAndMakeVisible (logoHit);
        if (! isTestInstance())
        {
            const int hour = juce::Time::getCurrentTime().getHours();
            const auto today = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
            if (hour < 4 && appProperties.getUserSettings()->getValue ("mascotLateNight") != today)
            {
                appProperties.getUserSettings()->setValue ("mascotLateNight", today);
                juce::Component::SafePointer<MainComponent> safe (this);
                juce::Timer::callAfterDelay (9000, [safe] { if (safe != nullptr) safe->peekMascot ("late"); });
            }
        }
        if (! isTestInstance() && appProperties.getUserSettings()->getBoolValue ("startupAnimation", true))
        {
            auto s = std::make_unique<StartupSplash>();
            juce::Component::SafePointer<MainComponent> safe (this);
            s->onDone = [safe] { if (safe != nullptr) safe->splash = nullptr; };
            splash = std::move (s);
            addAndMakeVisible (*splash);
            splash->setBounds (getLocalBounds());   // the window already has its size by now (resized() ran earlier)
            splash->toFront (false);
        }
        if (! isTestInstance())
        {
            juce::Component::SafePointer<MainComponent> safe (this);
            juce::Timer::callAfterDelay (2800, [safe] { if (safe != nullptr) safe->maybeShowWhatsNew(); });
        }
        if (! recoveryOffered && ! isTestInstance() && appProperties.getUserSettings()->getBoolValue ("showWelcome", true))
        {
            juce::Component::SafePointer<MainComponent> safe (this);
            juce::Timer::callAfterDelay (350, [safe] { if (safe != nullptr && safe->welcome == nullptr && ! safe->skipWelcome) safe->showWelcome(); });
        }
    }
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    discord = nullptr;   // clears the Discord status
    // a clean exit: no recovery offer next time (unless we're closing for an update)
    if (! isTestInstance() && ! keepAutosaveOnExit)
    {
        autosaveFile().deleteFile();
        autosaveInfo().deleteFile();
        lockFile().deleteFile();
    }
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* c = juce::Desktop::getInstance().getComponent (i))
            c->removeKeyListener (&forwardKeys);
    ValueSlider::automationActions() = {};
    ctx.createAutomation = nullptr;
    ctx.showReport = nullptr;
    studio.reset();
    amethystRemovePluginKeyHook();
    stopTimer();
    clipEditor = nullptr;
    engine.stop();
    engine.knownPlugins.removeChangeListener (this);
    engine.deviceManager.removeChangeListener (this);
    account->removeChangeListener (this);
    engine.unlockedTier = nullptr;
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
        if (customBackground.isValid()) paintHazedBackground (bg, getLocalBounds());
        else                            Glass::background (bg, getLocalBounds());

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
            bg.drawText ("2026  VOCAL STUDIO", word, juce::Justification::topLeft);
        }
    }
    g.drawImageAt (backgroundCache, 0, 0);

    auto m = masterMeterArea.toFloat();
    drawMeterH (g, m.removeFromTop (m.getHeight() / 2.0f - 2.0f), masterLevelL);
    m.removeFromTop (4.0f);
    drawMeterH (g, m, masterLevelR);
}

// The user's picture, "frosted": blurred (downscale + smooth upscale), tinted with the theme's purple
// gradient and the glass light blobs, so the translucent panels keep reading clearly on top.
void MainComponent::paintHazedBackground (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto src = customBackground;
    const float scale = juce::jmax ((float) area.getWidth() / (float) src.getWidth(), (float) area.getHeight() / (float) src.getHeight());
    const auto cover = juce::Rectangle<float> ((float) src.getWidth() * scale, (float) src.getHeight() * scale).withCentre (area.toFloat().getCentre());

    // blur: shrink a lot, then let high-quality resampling smear it back up (twice for a softer result)
    const int smallW = juce::jmax (8, area.getWidth() / (10 + (int) (backgroundHaze * 14.0f)));
    const int smallH = juce::jmax (8, area.getHeight() * smallW / juce::jmax (1, area.getWidth()));
    juce::Image tiny (juce::Image::RGB, smallW, smallH, true);
    {
        juce::Graphics tg (tiny);
        tg.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        const float k = (float) smallW / (float) area.getWidth();
        tg.drawImage (src, cover.translated ((float) -area.getX(), (float) -area.getY()).transformedBy (juce::AffineTransform::scale (k)).toNearestInt().toFloat(),
                      juce::RectanglePlacement::stretchToFit);
    }
    juce::Image mid (juce::Image::RGB, smallW * 4, smallH * 4, true);
    {
        juce::Graphics mg (mid);
        mg.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        mg.drawImage (tiny, mid.getBounds().toFloat());
    }
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (mid, area.toFloat());

    // purple haze over it
    g.setGradientFill (juce::ColourGradient (Theme::bgTop.withAlpha (0.35f + 0.5f * backgroundHaze), (float) area.getX(), (float) area.getY(),
                                             Theme::bgBottom.withAlpha (0.55f + 0.4f * backgroundHaze), (float) area.getRight(), (float) area.getBottom(), false));
    g.fillRect (area);
    juce::Image blobs (juce::Image::ARGB, area.getWidth(), area.getHeight(), true);
    {
        juce::Graphics bg (blobs);
        Glass::background (bg, blobs.getBounds());
    }
    g.setOpacity (0.25f + 0.25f * backgroundHaze);
    g.drawImageAt (blobs, area.getX(), area.getY());
    g.setOpacity (1.0f);
}

void MainComponent::loadCustomBackground()
{
    customBackground = {};
    const juce::File f (appProperties.getUserSettings()->getValue ("backgroundImage"));
    backgroundHaze = (float) juce::jlimit (0.0, 1.0, appProperties.getUserSettings()->getDoubleValue ("backgroundHaze", 0.6));
    if (f.existsAsFile())
    {
        auto img = juce::ImageFileFormat::loadFrom (f);
        if (img.isValid())
        {
            // keep memory small: it's blurred anyway
            const float k = juce::jmin (1.0f, 1600.0f / (float) juce::jmax (img.getWidth(), img.getHeight()));
            customBackground = k < 1.0f ? img.rescaled (juce::roundToInt (img.getWidth() * k), juce::roundToInt (img.getHeight() * k), juce::Graphics::highResamplingQuality)
                                        : img;
        }
    }
    backgroundCache = {};
    repaint();
}

void MainComponent::chooseBackground()
{
    ctx.chooser = std::make_unique<juce::FileChooser> ("Choose a background picture", juce::File::getSpecialLocation (juce::File::userPicturesDirectory),
                                                       "*.png;*.jpg;*.jpeg;*.gif");
    juce::Component::SafePointer<MainComponent> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || ! fc.getResult().existsAsFile())
            return;
        safe->appProperties.getUserSettings()->setValue ("backgroundImage", fc.getResult().getFullPathName());
        safe->loadCustomBackground();
        if (! safe->customBackground.isValid())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Background", "That picture couldn't be opened.");
    });
}

void MainComponent::setBackgroundHaze (float haze)
{
    appProperties.getUserSettings()->setValue ("backgroundHaze", (double) haze);
    loadCustomBackground();
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
    auto group = [this] (juce::Rectangle<int> area) { toolbarGroups.push_back (area.expanded (5, 4)); };
    constexpr int gap = 14, inner = 4;

    // these live in menus now (File / Settings / the Mixer toggle)
    for (juce::Component* c : std::initializer_list<juce::Component*> { &newBtn, &openBtn, &saveBtn, &audioBtn, &pluginsBtn, &arrangeBtn })
        c->setVisible (false);

    // ---- left: logo, transport, recording helpers ----
    const int rightWidth = 112 + gap + 140 + gap + (64 + inner + 40) + gap + (54 + inner + 70 + inner + 64);
    const int leftBase = 44 + 12 + 190 + gap + (40 * 3 + inner * 2);
    const int spare = bar.getWidth() - rightWidth - leftBase - gap * 2 - (118 + 10 + 180);
    showWordmark = spare > 260;
    const bool showSystem = spare > 160 + (showWordmark ? 110 : 0);
    systemMeter.setVisible (showSystem);

    logoArea = bar.removeFromLeft (showWordmark ? 150 : 44);
    bar.removeFromLeft (12);
    {
        auto t = bar.removeFromLeft (190);
        group (t);
        const int w = (t.getWidth() - 12) / 4;
        for (auto* b : { &rtzBtn, &stopBtn, &playBtn, &recBtn })
        {
            b->setBounds (t.removeFromLeft (w));
            t.removeFromLeft (inner);
        }
    }
    bar.removeFromLeft (gap);
    {
        auto h = bar.removeFromLeft (40 * 3 + inner * 2);
        group (h);
        metroBtn.setBounds (h.removeFromLeft (40));
        h.removeFromLeft (inner);
        countInBtn.setBounds (h.removeFromLeft (40));
        h.removeFromLeft (inner);
        punchBtn.setBounds (h.removeFromLeft (40));
    }

    // ---- right: account, master, tools + settings, file / export / mixer ----
    {
        auto a = bar.removeFromRight (112);
        group (a);
        accountBtn.setBounds (a);
    }
    bar.removeFromRight (gap);
    {
        auto mz = bar.removeFromRight (140);
        group (mz);
        masterK.setBounds (mz.removeFromLeft (52));
        mz.removeFromLeft (6);
        masterMeterArea = mz.withSizeKeepingCentre (mz.getWidth(), 18);
    }
    bar.removeFromRight (gap);
    {
        auto s = bar.removeFromRight (64 + inner + 40);
        group (s);
        settingsBtn.setBounds (s.removeFromRight (40));
        s.removeFromRight (inner);
        toolsBtn.setBounds (s);
    }
    bar.removeFromRight (gap);
    {
        auto f = bar.removeFromRight (54 + inner + 70 + inner + 64);
        group (f);
        mixerBtn.setBounds (f.removeFromRight (64));
        f.removeFromRight (inner);
        exportBtn.setBounds (f.removeFromRight (70));
        f.removeFromRight (inner);
        fileBtn.setBounds (f);
    }

    // ---- centre: one glass display with tempo and time (and the CPU meter when there's room) ----
    bar.removeFromLeft (gap);
    bar.removeFromRight (gap);
    {
        const int want = 118 + 10 + 180 + (showSystem ? 10 + 150 : 0);
        auto d = bar.withSizeKeepingCentre (juce::jmin (want, bar.getWidth()), bar.getHeight());
        displayArea = d;
        group (d);
        tempo.setBounds (d.removeFromLeft (118));
        d.removeFromLeft (10);
        timeDisplay.setBounds (d.removeFromLeft (180));
        if (showSystem)
        {
            d.removeFromLeft (10);
            systemMeter.setBounds (d.removeFromLeft (150));
        }
    }

    // Content
    r.reduce (8, 4);
    mixer.setBounds (r);

    auto rulerArea = r.removeFromTop (28);
    ruler.setBounds (rulerArea);
    {
        auto tools = rulerArea.withWidth (kHeaderWidth).reduced (4, 3);
        for (auto* b : { &moveToolBtn, &trimToolBtn, &cutToolBtn, &compToolBtn })
        {
            b->setBounds (tools.removeFromLeft (42));
            tools.removeFromLeft (2);
            b->toFront (false);
        }
        tools.removeFromLeft (3);
        snapBtn.setBounds (tools.removeFromLeft (70));
        snapBtn.toFront (false);
    }

    // bottom strip: zoom under the track list, horizontal scroll under the lanes
    auto bottomStrip = r.removeFromBottom (20);
    zoomSlider.setBounds (bottomStrip.withWidth (kHeaderWidth).reduced (14, 2));
    hScroll.setBounds (bottomStrip.withTrimmedLeft (kHeaderWidth).reduced (0, 3));
    viewport.setBounds (r);
    layoutTracks();
    if (studio != nullptr)
    {
        studio->setStatusArea (mixer.isVisible() ? juce::Rectangle<int>() : juce::Rectangle<int> (rulerArea.getRight() - 520, rulerArea.getY() + 4, 512, rulerArea.getHeight() - 8));
        studio->setBounds (getLocalBounds());
        studio->toFront (false);
    }
    logoHit.setBounds (logoArea.withWidth (44));
    mascotPeek.setBounds (getLocalBounds().removeFromRight (440).removeFromBottom (330).translated (0, -24));
    if (welcome != nullptr)
    {
        welcome->setBounds (getLocalBounds());
        welcome->toFront (false);
    }
    if (splash != nullptr)
    {
        splash->setBounds (getLocalBounds());
        splash->toFront (false);
    }
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
    auto& anim = juce::Desktop::getInstance().getAnimator();
    if (shouldShow) { mixer.setAlpha (0.0f); anim.fadeIn (&mixer, 160); }
    else            { viewport.setAlpha (0.0f); anim.fadeIn (&viewport, 160); }
    for (juce::Component* c : std::initializer_list<juce::Component*> { &ruler, &viewport, &hScroll, &zoomSlider,
                                                                         &moveToolBtn, &trimToolBtn, &cutToolBtn, &compToolBtn, &snapBtn })
        c->setVisible (! shouldShow);

    mixerBtn.setToggleState (shouldShow, juce::dontSendNotification);
    arrangeBtn.setToggleState (! shouldShow, juce::dontSendNotification);
    if (shouldShow)
        mixer.rebuild();
    if (studio != nullptr)   // the status pill belongs to the arrangement's ruler
        studio->setStatusArea (shouldShow ? juce::Rectangle<int>() : juce::Rectangle<int> (ruler.getRight() - 520, ruler.getY() + 4, 512, ruler.getHeight() - 8));
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
    {
        // up up down down left right left right B A
        static const int code[] = { juce::KeyPress::upKey, juce::KeyPress::upKey, juce::KeyPress::downKey, juce::KeyPress::downKey,
                                    juce::KeyPress::leftKey, juce::KeyPress::rightKey, juce::KeyPress::leftKey, juce::KeyPress::rightKey, 'B', 'A' };
        const int kc = key.getKeyCode();
        const int want = code[konamiPos];
        const bool match = kc == want || (want >= 'A' && want <= 'Z' && kc == want + 32);
        konamiPos = match ? konamiPos + 1 : (kc == juce::KeyPress::upKey ? (konamiPos == 2 ? 2 : 1) : 0);
        if (konamiPos == 10) { konamiPos = 0; peekMascot ("konami", true); }
    }
    if (studio != nullptr && studio->keyPressed (key)) return true;
    if (key == juce::KeyPress (',', juce::ModifierKeys::commandModifier, 0)) { showSettingsWindow(); return true; }
    if (key == juce::KeyPress::spaceKey)               { togglePlay(); return true; }
    if (key == juce::KeyPress::tabKey)                 { setMixerVisible (! mixer.isVisible()); return true; }
    if (key == juce::KeyPress ('r', juce::ModifierKeys::commandModifier, 0)) { exportAs ((int) Exporter::Format::Wav); return true; }
    if (key == juce::KeyPress ('r', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { exportAs ((int) Exporter::Format::Mp3); return true; }
    if ((key.getTextCharacter() == 'r' || key.getTextCharacter() == 'R') && ! key.getModifiers().isCommandDown()) { toggleRecord(); return true; }
    if (! mixer.isVisible() && ! key.getModifiers().isAnyModifierKeyDown())
    {
        const auto ch = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        if (ch == 'v') { setTool (EditTool::Move); return true; }
        if (ch == 't') { setTool (EditTool::Trim); return true; }
        if (ch == 'c') { setTool (EditTool::Cut);  return true; }
        if (ch == 'x') { setTool (EditTool::Comp); return true; }
    }
    if (key == juce::KeyPress::homeKey)                { engine.setTransportPosition (0); view.startSeconds = 0.0; return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0)) { saveProject (false); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { saveProject (true); return true; }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) { undoEdit (false); return true; }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
        || key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)) { undoEdit (true); return true; }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { openProject(); return true; }
    if (key == juce::KeyPress ('n', juce::ModifierKeys::commandModifier, 0)) { newProject(); return true; }
    return false;
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (engine.canImport (juce::File (f)) || FlpImport::isFlPreset (juce::File (f)))
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
        // FL presets can go on the master; audio can't
        const bool presetDrop = files.size() > 0 && FlpImport::isFlPreset (juce::File (files[0]));
        target = (sel != nullptr && (presetDrop || ! sel->isMaster)) ? sel : nullptr;
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
    // FL Studio presets (.fst): the effect chain goes onto the channel they were dropped on, or a new one
    juce::StringArray audioFiles;
    for (auto& path : files)
    {
        const juce::File file (path);
        if (! FlpImport::isFlPreset (file))
        {
            audioFiles.add (path);
            continue;
        }
        Track* t = target != nullptr ? target : addTrack (file.getFileNameWithoutExtension());
        target = nullptr;
        if (t == nullptr)
            continue;
        juce::Component::SafePointer<MainComponent> safe (this);
        UiHelpers::loadPreset (ctx, *t, file, [safe] { if (safe != nullptr && safe->mixer.isVisible()) safe->mixer.rebuild(); });
    }
    if (audioFiles.isEmpty())
        return;
    importAudioFiles (audioFiles, target, startSeconds);
}

void MainComponent::importAudioFiles (const juce::StringArray& files, Track* target, double startSeconds)
{
    juce::StringArray failed;
    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const auto start = (juce::int64) std::llround (juce::jmax (0.0, startSeconds) * sr);
    history.checkpoint ("Import audio");

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
    if (! windowIconApplied && getPeer() != nullptr)
        applyWindowIcon();
    if (! mascotClipShown && engine.isPlaying() && ! engine.isRecording() && engine.getMaster().clipped.load())
    {
        mascotClipShown = true;   // once per session
        peekMascot ("clip");
    }
    if (++updateTicks >= 30 * 60 * 60 * 6)   // every 6 hours while open
    {
        updateTicks = 0;
        if (appProperties.getUserSettings()->getBoolValue ("autoUpdate", true))
            checkForUpdates (false);
    }
    if (++discordTicks >= 60)   // every 2 s
    {
        discordTicks = 0;
        updateDiscord();
    }

    // mixer / plug-in changes become undo steps once they settle
    if (++mixerPollTicks >= 10)
    {
        mixerPollTicks = 0;
        history.pollMixer (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown() || engine.isRecording() || engine.isExporting());
    }

    // a plug-in crashed on the audio thread: say which one (it has been switched off)
    if (const int crashes = engine.pluginCrashes.load(); crashes != lastPluginCrashes)
    {
        lastPluginCrashes = crashes;
        juce::StringArray names;
        auto scan = [&names] (Track& t)
        {
            for (int i = 0; i < (int) t.inserts.size(); ++i)
                if (auto& s = t.inserts[(size_t) i]; s->plugin != nullptr && s->crashed.load() && ! s->crashReported)
                {
                    s->crashReported = true;
                    names.add (s->plugin->getName() + " on " + (t.isMaster ? juce::String ("Master") : t.name) + " (slot " + juce::String (i + 1) + ")");
                }
        };
        scan (engine.getMaster());
        for (auto* t : engine.getTracks()) scan (*t);
        if (! names.isEmpty())
        {
            if (studio != nullptr) studio->toast ("A plug-in crashed and was switched off: " + names[0], true);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plug-in crashed",
                names.joinIntoString ("\n") + "\n\ncrashed while processing audio. AMETHYST switched it off so your session keeps running.\n\n"
                "Save your project. To use it again, remove it and load it fresh from the mixer (or restart AMETHYST).");
            if (mixer.isVisible()) mixer.rebuild();
        }
    }

    if (++autosaveTicks >= 30 * 120)   // every 2 minutes (timer runs at 30 Hz)
    {
        autosaveTicks = 0;
        autosaveNow();
    }

    // punch-out: stop by itself a moment after the region ends
    if (engine.isPunchRecording()
        && engine.getTransportPosition() > engine.punchOut.load() + (juce::int64) (0.25 * engine.getSampleRate()))
        stopTransport();

    // Colour the title bar of every AMETHYST window (main, plug-ins, dialogs) once it exists.
    if (++titleBarTick % 15 == 0)
    {
        auto& desktop = juce::Desktop::getInstance();
        std::set<void*> open;
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* c = desktop.getComponent (i))
                if (auto* handle = c->getWindowHandle())
                {
                    if (c != getTopLevelComponent())
                        c->addKeyListener (&forwardKeys);   // no-op if already added
                    open.insert (handle);
                    if (styledWindows.count (handle) == 0)
                        amethystStyleTitleBar (handle, 0x160b2b, 0xefe8ff, 0x7b4fd0);
                }
        styledWindows = std::move (open);   // closed windows drop out, so a reused handle gets styled again
    }

    // Space pressed inside a plug-in editor still plays / stops (see SystemInfo.cpp).
    if (! pluginKeyHookInstalled)
        if (auto* top = getTopLevelComponent(); top != nullptr && top->getPeer() != nullptr)
        {
            pluginKeyHookInstalled = true;   // try once; if Windows refuses, Space still works in the main window
            amethystInstallPluginKeyHook (top->getWindowHandle(), [] (void* ctx)
            {
                juce::Component::SafePointer<MainComponent> safe (static_cast<MainComponent*> (ctx));
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->togglePlay(); });
            }, this);
        }

    if (engine.bounceReachedEnd())
    {
        engine.stop();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export finished", "The mix has been exported.");
        if (juce::Random::getSystemRandom().nextInt (3) == 0) peekMascot ("export");
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
    punchBtn.setToggleState (engine.punchEnabled.load(), juce::dontSendNotification);
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
        if (! zoomSlider.isMouseButtonDown() && std::abs (zoomSlider.getValue() - view.pixelsPerSecond) > 0.01)
            zoomSlider.setValue (view.pixelsPerSecond, juce::dontSendNotification); // follows Ctrl + wheel zooming

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
    // If the main window is the active OS window but keyboard focus is nowhere useful (nothing,
    // a hidden or deleted component, or a component of another window), take it back.
    if (auto* peer = getPeer())
        if (peer->isFocused() && isShowing() && ! hasKeyboardFocus (true))
        {
            auto* focused = juce::Component::getCurrentlyFocusedComponent();
            if (focused == nullptr || ! focused->isShowing() || focused->getTopLevelComponent() != getTopLevelComponent())
                grabKeyboardFocus();
        }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    auto* settings = appProperties.getUserSettings();

    if (source == account.get())
    {
        updateAccountButton();
        {
            // themes follow the plan: re-apply the saved one if it is (now) unlocked, else Amethyst
            const int saved = juce::jlimit (0, Theme::kNumThemes - 1, appProperties.getUserSettings()->getIntValue ("colourTheme", 0));
            const int tier = ctx.tier ? ctx.tier() : 0;
            const int want = tier >= Theme::kThemes[saved].tier ? saved : 0;
            if (want != Theme::currentTheme())
                applyColourTheme (want, false);
        }
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

void MainComponent::setTool (EditTool tool)
{
    view.tool = tool;
    moveToolBtn.setToggleState (tool == EditTool::Move, juce::dontSendNotification);
    trimToolBtn.setToggleState (tool == EditTool::Trim, juce::dontSendNotification);
    cutToolBtn.setToggleState (tool == EditTool::Cut, juce::dontSendNotification);
    compToolBtn.setToggleState (tool == EditTool::Comp, juce::dontSendNotification);
    for (auto* tc : trackComps)
        tc->repaint();
}

void MainComponent::setSnap (int index)
{
    view.snapIndex = juce::jlimit (0, (int) kSnapOptions.size() - 1, index);
    snapBtn.setLabel (kSnapOptions[(size_t) view.snapIndex].name);
    snapBtn.setToggleState (view.snapBeats() > 0.0, juce::dontSendNotification);
    appProperties.getUserSettings()->setValue ("snapIndex", view.snapIndex);
    for (auto* tc : trackComps)
        tc->repaint();
}

void MainComponent::showSnapMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Snap to");
    for (int i = 0; i < (int) kSnapOptions.size(); ++i)
    {
        if (i == 1 || i == 6 || i == 11)
            m.addSeparator();
        m.addItem (1 + i, kSnapOptions[(size_t) i].name, true, i == view.snapIndex);
    }
    m.addSeparator();
    m.addItem (100, "Hold Shift while dragging to ignore snap", false, false);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&snapBtn), [safe] (int r)
    {
        if (safe != nullptr && r >= 1 && r <= (int) kSnapOptions.size())
            safe->setSnap (r - 1);
    });
}

void MainComponent::updateAccountButton()
{
    if (! account->isSignedIn())
    {
        accountBtn.setInfo ({}, {}, "Sign in", 0, false);
        accountBtn.setTooltip ("Sign in or create an AMETHYST account");
    }
    else
    {
        const bool pro = account->isPro();
        const int planTier = account->isUltra() ? 2 : (pro ? 1 : 0);
        accountBtn.setInfo (account->getAvatar(), account->getEmail(), planTier == 2 ? "ULTRA" : (pro ? "PRO" : "FREE"), planTier, true);
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
    auto* t = engine.addTrack (name.isNotEmpty() ? name : "Track " + juce::String (engine.getTracks().size() + 1));
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
        if (t->isBus)
            continue;   // FX buses live in the mixer only
        auto* tc = trackComps.add (new TrackComponent (ctx, *t));
        trackHolder.addAndMakeVisible (tc);
    }
    layoutTracks();
    mixer.rebuild();
}

//==============================================================================
void MainComponent::togglePlay()
{
    if (engine.isExporting())
        return;
    if (engine.isRecording())   // Space while recording stops and keeps the take
    {
        stopTransport();
        returnToPlayStart();
        return;
    }

    if (engine.isPlaying())
    {
        const bool previewing = engine.isPreviewing();
        engine.stop();
        if (! previewing)
            returnToPlayStart();
    }
    else
    {
        playStartPos = engine.getTransportPosition();
        engine.play();
    }
}

void MainComponent::returnToPlayStart()
{
    if (engine.returnOnStop.load() && playStartPos >= 0)
    {
        engine.setTransportPosition (playStartPos);
        // keep the start in view
        const double t = (double) playStartPos / juce::jmax (1.0, engine.getSampleRate());
        if (t < view.startSeconds)
            view.startSeconds = juce::jmax (0.0, t - 1.0);
        repaint();
    }
    playStartPos = -1;
}

void MainComponent::toggleRecord()
{
    if (engine.isExporting())
        return;
    if (engine.isRecording())
    {
        stopTransport();
        returnToPlayStart();
        return;
    }

    if (engine.isBouncing())
        return;

    if (engine.isPlaying())
        engine.stop();

    playStartPos = engine.getTransportPosition();
    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const auto countIn = (juce::int64) (countInBars * 4.0 * 60.0 / juce::jmax (20.0, engine.bpm.load()) * sr);
    if (! engine.record (countIn))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Nothing to record",
                                                "Arm a track first with its red R button.");
}

void MainComponent::stopOrReturn()
{
    if (engine.isPlaying() || engine.isRecording())
    {
        const bool previewing = engine.isPreviewing();
        stopTransport();
        if (! previewing)
            returnToPlayStart();
    }
    else
    {
        engine.setTransportPosition (0);
        view.startSeconds = 0.0;
    }
}

void MainComponent::stopTransport()
{
    const bool wasRecording = engine.isRecording();
    const bool wasPunch = engine.isPunchRecording();
    if (wasRecording)
        history.checkpoint (wasPunch ? "Punch-in" : "Record take");
    engine.stop();
    if (wasPunch)
    {
        auto takes = engine.consumeNewTakes();
        finishPunch (takes);
    }
    else if (wasRecording)
        placeNewTakes();
}

bool MainComponent::sessionHasWork()
{
    if (engine.getMaster().countPlugins() > 0)
        return true;
    for (auto* t : engine.getTracks())
        if (! t->clips.empty() || t->countPlugins() > 0 || t->isAutomation())
            return true;
    return false;
}

void MainComponent::autosaveNow()
{
    if (isTestInstance())
        return;
    if (engine.isRecording() || engine.isExporting())
    {
        autosaveTicks = 30 * 110;   // try again in ~10 s
        return;
    }
    autosaveFolder().createDirectory();
    if (! sessionHasWork())
    {
        autosaveFile().deleteFile();
        autosaveInfo().deleteFile();
        return;
    }
    // write to a temp file first so a crash mid-save never leaves a broken copy
    const auto temp = autosaveFolder().getChildFile ("session.tmp");
    if (ProjectIO::save (engine, temp).wasOk() && temp.moveFileTo (autosaveFile()))
    {
        juce::XmlElement info ("AUTOSAVE");
        info.setAttribute ("project", projectFile.getFullPathName());
        info.setAttribute ("time", juce::Time::getCurrentTime().toISO8601 (true));
        info.writeTo (autosaveInfo());
    }
}

void MainComponent::checkForCrashRecovery()
{
    if (isTestInstance())
        return;
    autosaveFolder().createDirectory();
    const bool crashed = lockFile().existsAsFile() && autosaveFile().existsAsFile();
    recoveryOffered = crashed;
    lockFile().replaceWithText (juce::String (juce::Time::currentTimeMillis()));   // this session is running now
    if (! crashed)
    {
        autosaveFile().deleteFile();
        autosaveInfo().deleteFile();
        return;
    }

    juce::File original;
    bool afterUpdate = false;
    juce::String when = autosaveFile().getLastModificationTime().formatted ("%H:%M on %d %b");
    if (auto info = juce::XmlDocument::parse (autosaveInfo()))
    {
        original = juce::File (info->getStringAttribute ("project"));
        afterUpdate = info->getStringAttribute ("reason") == "update";
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Timer::callAfterDelay (700, [safe, original, when, afterUpdate]
    {
        if (safe == nullptr)
            return;
        const auto name = original != juce::File() ? "\"" + original.getFileNameWithoutExtension() + "\"" : juce::String ("your unsaved session");
        const juce::String title = afterUpdate ? "Updated to version " + juce::String (ProjectInfo::versionString) : juce::String ("Restore your session?");
        const juce::String text = afterUpdate ? "AMETHYST 2026 was updated.\n\nPick up " + name + " where you left off?"
                                              : "AMETHYST didn't close properly last time.\n\nAn autosaved copy of " + name + " from " + when + " is available. Restore it?";
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, title, text,
                                            "Restore", "Start fresh", nullptr,
                                            juce::ModalCallbackFunction::create ([safe, original] (int ok)
        {
            if (safe == nullptr)
                return;
            if (ok == 1)
                safe->restoreAutosave (original);
            else
            {
                autosaveFile().deleteFile();
                autosaveInfo().deleteFile();
            }
        }));
    });
}

void MainComponent::restoreAutosave (const juce::File& original)
{
    // keep the copy safe while loading, in case this load is what crashes
    const auto keep = autosaveFolder().getChildFile ("restoring.amethyst");
    autosaveFile().copyFileTo (keep);

    clipEditor = nullptr;
    pluginWindows.closeAll();
    mixer.select (&engine.getMaster());
    trackComps.clear();
    auto result = ProjectIO::load (engine, keep);
    history.clear();
    projectFile = original.existsAsFile() ? original : juce::File();   // Ctrl+S saves back to your project
    metroBtn.setToggleState (engine.metronomeOn.load(), juce::dontSendNotification);
    view.startSeconds = 0.0;
    rebuildTrackComponents();
    keep.deleteFile();

    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName (kAppTitle + " - " + (original != juce::File() ? original.getFileNameWithoutExtension() : juce::String ("Untitled")) + " (recovered)");
    if (result.failed())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Restored with problems", result.getErrorMessage());
}

void MainComponent::setCountIn (int bars)
{
    countInBars = juce::jlimit (0, 2, bars);
    countInBtn.setBadge (countInBars > 0 ? juce::String (countInBars) : juce::String());
    countInBtn.setToggleState (countInBars > 0, juce::dontSendNotification);
    countInBtn.setTooltip (countInBars == 0 ? "Count-in: off (click for 1 bar)"
                                            : "Count-in: " + juce::String (countInBars) + (countInBars == 1 ? " bar (click for 2)" : " bars (click to turn off)"));
    appProperties.getUserSettings()->setValue ("countInBars", countInBars);
}

// Punch-in replaces only the punched part: the new take is trimmed to the region, and on that track
// the old audio inside the region is cut out (split at both edges, middle removed). Outside stays as it was.
void MainComponent::finishPunch (std::vector<std::pair<Track*, std::shared_ptr<AudioClip>>>& takes)
{
    for (auto& [track, take] : takes)
        engine.applyPunchTake (*track, take);
    for (auto* tc : trackComps)
        tc->repaint();
}

void MainComponent::placeNewTakes()
{
    bool changed = false;

    for (auto& [source, clip] : engine.consumeNewTakes())
    {
        if (! engine.isValidChannel (source) || ! source->takesToFreeTrack)
            continue;

        auto* target = engine.findTrackForTake (source, *clip);
        if (target == source)
            continue; // nothing underneath it - leave it where it was recorded

        if (target == nullptr) // no free track below: make one
            target = addTrack (source->name + " take");

        // The take lane plays through the source's mixer channel - same plug-ins, fader and pan,
        // so moving the take never changes how loud it is or how it sounds.
        engine.setPlayThrough (*target, source->playThrough != nullptr ? source->playThrough : source);

        if (auto moved = engine.removeClip (*source, clip.get()))
            engine.addClip (*target, std::move (moved));
        changed = true;
    }

    if (changed && mixer.isVisible())
        mixer.rebuild();
}

void MainComponent::undoEdit (bool redo)
{
    if (engine.isRecording())
        return;

    if (redo ? history.redo() : history.undo())
    {
        for (auto* tc : trackComps)
            tc->repaint();
        if (mixer.isVisible())
            mixer.rebuild();
        if (clipEditor != nullptr && ! clipEditor->isValid())
            clipEditor = nullptr;
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
        if (studio != nullptr)
            studio->archiveBeforeSave (f);   // the previous save is kept under "Restore earlier version"
        auto result = ProjectIO::save (engine, f);
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save failed", result.getErrorMessage());
        else
        {
            projectFile = f;
            noteRecentProject (f);
            if (studio != nullptr)
                studio->toast ("Saved " + f.getFileNameWithoutExtension());
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setName (kAppTitle + " - " + f.getFileNameWithoutExtension());
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

bool MainComponent::locatePlugin (juce::AudioProcessor& p, Track*& track, int& slot)
{
    auto findIn = [&] (Track& t)
    {
        for (int s = 0; s < (int) t.inserts.size(); ++s)
            if (t.inserts[(size_t) s]->plugin.get() == &p) { track = &t; slot = s; return true; }
        return false;
    };
    if (findIn (engine.getMaster())) return true;
    for (auto* t : engine.getTracks())
        if (! t->isAutomation() && findIn (*t)) return true;
    return false;
}

TrackComponent* MainComponent::findAutomationLane (Track& target, Automation::Kind kind, int slot, int param)
{
    for (auto* tc : trackComps)
        if (auto* a = tc->getTrack().automation.get())
            if (a->target == &target && a->kind == kind && (kind != Automation::Kind::Param || (a->slot == slot && a->param == param)))
                return tc;
    return nullptr;
}

void MainComponent::automationAction (juce::AudioProcessor& p, int param, int action)
{
    Track* target = nullptr;
    int slot = -1;
    if (! locatePlugin (p, target, slot))
        return;
    auto* lane = findAutomationLane (*target, Automation::Kind::Param, slot, param);

    switch (action)
    {
        case 1:   // create
            if (lane == nullptr) addAutomation (*target, Automation::Kind::Param, slot, param);
            else                 automationAction (p, param, 2);
            break;
        case 2:   // edit: bring the lane into view and flash it
            if (lane != nullptr)
            {
                viewport.setViewPosition (0, juce::jmax (0, lane->getY() - viewport.getHeight() / 3));
                lane->flash();
            }
            break;
        case 3:   // init song with this position: the clip starts at the control's current value
            if (lane != nullptr)
            {
                auto& a = *lane->getTrack().automation;
                const float v = AudioEngine::getControlValue (a);
                const juce::ScopedLock sl (engine.getLock());
                if (! a.points.empty() && a.points.front().seconds <= 1.0e-6)
                    a.points.front().value = v;
                else
                    a.points.insert (a.points.begin(), { 0.0, v });
                lane->repaint();
            }
            break;
        case 4:   // delete
            if (lane != nullptr) removeTrack (&lane->getTrack());
            break;
        default: break;
    }
}

void MainComponent::addLastTweakedMenu (juce::PopupMenu& m)
{
    const auto last = LastTweaked::get();
    Track* target = nullptr;
    int slot = -1;
    juce::AudioProcessorParameter* param = nullptr;
    if (last.processor != nullptr && locatePlugin (*last.processor, target, slot))
        if (juce::isPositiveAndBelow (last.index, last.processor->getParameters().size()))
            param = last.processor->getParameters()[last.index];

    if (param == nullptr)
    {
        m.addSectionHeader ("Last tweaked parameter");
        m.addItem (300, "(move a knob in any plug-in window first)", false);
        return;
    }

    const bool has = findAutomationLane (*target, Automation::Kind::Param, slot, last.index) != nullptr;
    m.addSectionHeader ("Last tweaked: " + target->name + " - " + target->inserts[(size_t) slot]->plugin->getName() + ": " + param->getName (32));
    m.addItem (301, "Create automation clip", ! has);
    m.addItem (302, "Edit automation clip", has);
    m.addItem (303, "Init song with this position", has);
    m.addItem (304, "Delete automation clip", has);
    m.addItem (305, "Randomize");
    m.addItem (306, "Reset to default");
}

void MainComponent::showToolsMenu()
{
    const int tier = ctx.tier ? ctx.tier() : 0;
    juce::PopupMenu m;
    addLastTweakedMenu (m);
    m.addSectionHeader ("Tools");
    {
        juce::PopupMenu::Item nc (juce::String ("Nightcore Maker...") + (tier >= 2 ? "" : "   (Ultra Pro)"));
        nc.itemID = 1;
        auto badge = std::make_unique<juce::DrawableImage>();
        badge->setImage (Mascot::badgeImage (48));
        nc.setImage (std::move (badge));
        m.addItem (std::move (nc));
    }
    m.addItem (2, "Find song tempo & key");
    m.addSeparator();

    juce::PopupMenu presets;
    const auto styles = Builtins::masterStyleNames();
    int current = -1;
    for (auto& slot : engine.getMaster().inserts)
        if (auto* am = dynamic_cast<AmethystPlugin*> (slot->plugin.get()); am != nullptr && am->pluginId == "amethyst.master")
            current = am->getInt ("style");
    for (int i = 0; i < styles.size(); ++i)
        presets.addItem (100 + i, styles[i], true, i == current);
    presets.addSeparator();
    presets.addItem (99, "Remove mastering from Master", current >= 0);
    m.addSubMenu (juce::String ("Master presets (Polish)") + (tier >= 1 ? "" : "   (Pro)"), presets);

    juce::PopupMenu automate;
    automate.addItem (200, "Master volume");
    automate.addItem (201, "Master pan");
    m.addSubMenu ("Automate Master", automate);



    if (studio != nullptr)
        studio->addToToolsMenu (m);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toolsBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0 || r >= 700000)
            return;
        auto& self = *safe;
        if (r == 1)
        {
            if ((self.ctx.tier ? self.ctx.tier() : 0) < 2)
            {
                if (self.ctx.showProPrompt) self.ctx.showProPrompt ("Nightcore Maker", 2);
                return;
            }
            NightcoreMaker::show (self.engine, &self, [safe] (const juce::File& f) { if (safe != nullptr) safe->addAudioFileAsTrack (f); });
        }
        else if (r >= 301 && r <= 306)
        {
            const auto last = LastTweaked::get();
            Track* t = nullptr; int slot = -1;
            if (last.processor == nullptr || ! self.locatePlugin (*last.processor, t, slot)
                || ! juce::isPositiveAndBelow (last.index, last.processor->getParameters().size()))
                return;
            auto* p = last.processor->getParameters()[last.index];
            if (r <= 304)      self.automationAction (*last.processor, last.index, r - 300);
            else if (r == 305) p->setValueNotifyingHost (juce::Random::getSystemRandom().nextFloat());
            else               p->setValueNotifyingHost (p->getDefaultValue());
        }
        else if (r == 600) self.checkForUpdates (true);
        else if (r == 601)
        {
            auto* st = self.appProperties.getUserSettings();
            st->setValue ("autoUpdate", ! st->getBoolValue ("autoUpdate", true));
        }
        else if (r == 500)
        {
            auto* st = self.appProperties.getUserSettings();
            const bool on = ! st->getBoolValue ("discordEnabled", true);
            st->setValue ("discordEnabled", on);
            if (self.discord != nullptr) self.discord->setEnabled (on);
        }
        else if (r == 501)
        {
            auto* st = self.appProperties.getUserSettings();
            st->setValue ("discordShowProject", ! st->getBoolValue ("discordShowProject", false));
            self.updateDiscord();
        }
        else if (r == 502) self.askDiscordAppId();
        else if (r == 400) self.chooseBackground();
        else if (r >= 401 && r <= 403) self.setBackgroundHaze (r == 401 ? 0.25f : r == 402 ? 0.6f : 0.9f);
        else if (r == 404) { self.appProperties.getUserSettings()->removeValue ("backgroundImage"); self.loadCustomBackground(); }
        else if (r == 2)  self.analyseSong();
        else if (r == 99) self.applyMasterPreset (-1);
        else if (r >= 100 && r < 200) self.applyMasterPreset (r - 100);
        else if (r == 200 || r == 201)
            self.addAutomation (self.engine.getMaster(), r == 200 ? Automation::Kind::Volume : Automation::Kind::Pan, -1, -1);
    });
}

void MainComponent::applyMasterPreset (int style)
{
    auto& master = engine.getMaster();
    int existing = -1;
    for (int i = 0; i < (int) master.inserts.size(); ++i)
        if (auto* am = dynamic_cast<AmethystPlugin*> (master.inserts[(size_t) i]->plugin.get()); am != nullptr && am->pluginId == "amethyst.master")
            existing = i;

    if (style < 0)
    {
        if (existing >= 0)
        {
            pluginWindows.close (*master.inserts[(size_t) existing]->plugin);
            engine.removeInsert (master, existing).reset();
            mixer.rebuild();
        }
        return;
    }

    if (existing >= 0)
    {
        if (auto* am = dynamic_cast<AmethystPlugin*> (master.inserts[(size_t) existing]->plugin.get()))
            am->setParam ("style", (float) style);
        return;
    }

    juce::PluginDescription desc;
    for (auto& d : Builtins::getDescriptions())
        if (d.fileOrIdentifier == "amethyst.master")
            desc = d;
    juce::String err;
    auto plugin = engine.createPlugin (desc, err);
    if (plugin == nullptr)
    {
        if (ctx.showProPrompt) ctx.showProPrompt ("Master presets", Builtins::requiredTier ("amethyst.master"));
        return;
    }
    if (auto* am = dynamic_cast<AmethystPlugin*> (plugin.get()))
        am->setParam ("style", (float) style);

    // mastering goes last in the chain
    int slot = -1;
    for (int i = (int) master.inserts.size(); --i >= 0;)
        if (master.inserts[(size_t) i]->plugin == nullptr) { slot = i; break; }
    if (slot < 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Master presets", "The Master's 10 slots are full. Free one up first.");
        return;
    }
    engine.insertPlugin (master, slot, std::move (plugin)).reset();
    mixer.rebuild();
}

void MainComponent::addAudioFileAsTrack (const juce::File& f)
{
    auto* t = addTrack (f.getFileNameWithoutExtension());
    if (auto clip = engine.loadClip (f, 0))
        engine.addClip (*t, std::move (clip));
    layoutTracks();
}

void MainComponent::analyseSong()
{
    // Mono mix of every playing clip (no effects), first 3 minutes.
    const double sr = juce::jmax (1.0, engine.getSampleRate());
    const auto total = (int) juce::jmin<juce::int64> (engine.getSessionLengthSamples(), (juce::int64) (sr * 180.0));
    if (total < (int) (sr * 3.0))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Find tempo & key", "Add at least a few seconds of audio first.");
        return;
    }
    juce::AudioBuffer<float> mix (1, total);
    mix.clear();
    {
        const juce::ScopedLock sl (engine.getLock());
        for (auto* t : engine.getTracks())
        {
            if (t->mute.load() || t->isAutomation())
                continue;
            for (auto& c : t->clips)
            {
                if (c->buffer == nullptr || ! c->enabled.load())
                    continue;
                const auto start = c->startSample.load();
                const auto len = c->getLength();
                const auto off = c->getOffset();
                for (int ch = 0; ch < c->buffer->getNumChannels(); ++ch)
                {
                    const int from = (int) juce::jmax<juce::int64> (0, start);
                    const int n = (int) juce::jmin<juce::int64> (len, (juce::int64) total - from);
                    if (n > 0)
                        mix.addFrom (0, from, *c->buffer, ch, (int) off, n, 1.0f / (float) c->buffer->getNumChannels());
                }
            }
        }
    }

    const double bpm = ClipFx::detectTempo (mix, 0, total, sr);
    const auto key = ClipFx::detectKey (mix, 0, total, sr);
    juce::String text;
    text << "Tempo: " << (bpm > 0.0 ? UiHelpers::formatBpm (bpm) + " BPM" : juce::String ("couldn't find a steady beat")) << "\n"
         << "Key: " << key.name() << (key.tonic >= 0 && key.confidence < 0.25f ? " (not very sure)" : "");

    if (bpm <= 0.0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Song tempo & key", text);
        return;
    }
    UiHelpers::offerTempo (engine, bpm, "Song tempo & key", "Key: " + key.name() + (key.tonic >= 0 && key.confidence < 0.25f ? " (not very sure)" : ""));
}

void MainComponent::addAutomation (Track& target, Automation::Kind kind, int slot, int param)
{
    if (engine.isRecording() || ! engine.isValidChannel (&target))
        return;
    auto* t = engine.addAutomationTrack (target, kind, slot, param);
    auto* tc = trackComps.add (new TrackComponent (ctx, *t));
    trackHolder.addAndMakeVisible (tc);
    layoutTracks();
    mixer.rebuild();
    viewport.setViewPosition (0, juce::jmax (0, trackHolder.getHeight() - viewport.getHeight()));
}

void MainComponent::newProject()
{
    if (engine.isRecording() || engine.isExporting())
        return;

    bool hasWork = false;
    for (auto* t : engine.getTracks())
        hasWork = hasWork || ! t->clips.empty() || t->countPlugins() > 0;
    hasWork = hasWork || engine.getMaster().countPlugins() > 0;

    if (! hasWork)
    {
        startNewProject();
        return;
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "New project",
                                        "Start a new empty project? Anything you haven't saved in this one will be lost.",
                                        "New project", "Cancel", nullptr,
                                        juce::ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe != nullptr && result != 0)
            safe->startNewProject();
    }));
}

void MainComponent::startNewProject (const juce::StringArray& trackNames)
{
    clipEditor = nullptr;
    pluginWindows.closeAll();
    mixer.select (&engine.getMaster());
    trackComps.clear();

    engine.stop();
    engine.clearTracks();
    engine.clearSessionNotes();
    if (studio != nullptr) studio->sessionChanged();
    engine.setTransportPosition (0);
    auto& master = engine.getMaster();
    engine.replaceInserts (master, {}).clear();
    master.volumeDb = 0.0f;
    master.pan = 0.0f;
    master.mute = false;
    engine.bpm = 120.0;
    engine.metronomeOn = false;
    engine.punchIn = -1;
    engine.punchOut = -1;
    engine.punchEnabled = false;
    metroBtn.setToggleState (false, juce::dontSendNotification);

    history.clear();
    projectFile = juce::File();
    view.startSeconds = 0.0;

    if (trackNames.isEmpty())
        for (int i = 1; i <= 8; ++i)
            engine.addTrack ("Track " + juce::String (i));
    else
        for (auto& n : trackNames)
            engine.addTrack (n);
    rebuildTrackComponents();

    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName (kAppTitle);
    repaint();
}

void MainComponent::openProject()
{
    if (engine.isRecording())
        return;

    AudioEngine::getProjectsFolder().createDirectory();
    ctx.chooser = std::make_unique<juce::FileChooser> ("Open project (AMETHYST or FL Studio .flp)", AudioEngine::getProjectsFolder(),
                                                       juce::String (ProjectIO::openWildcard) + ";*.flp");

    juce::Component::SafePointer<MainComponent> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (safe != nullptr && file.existsAsFile())
            safe->openProjectFile (file);
    });
}

void MainComponent::openProjectFile (const juce::File& file)
{
    if (engine.isRecording() || ! file.existsAsFile())
        return;

    // Only projects open as projects. An audio file (e.g. "Open with AMETHYST" on an MP3) becomes a new
    // track; anything else is refused - never adopted as the project file, so Save can't overwrite it.
    if (! file.hasFileExtension ("amethyst;vdaw;flp"))
    {
        if (engine.canImport (file))
            addAudioFileAsTrack (file);
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Not a project",
                                                    file.getFileName() + " isn't an AMETHYST or FL Studio project.");
        return;
    }
    if (! file.hasFileExtension ("flp"))
        if (auto xml = juce::XmlDocument::parse (file); xml == nullptr || ! xml->hasTagName ("VOCALDAW_PROJECT"))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't open project",
                                                    file.getFileName() + " isn't a readable AMETHYST project. Your current session is unchanged.");
            return;
        }

    clipEditor = nullptr;
    pluginWindows.closeAll();
    mixer.select (&engine.getMaster());
    trackComps.clear();

    const bool isFlp = file.hasFileExtension ("flp");
    FlpImport::Report flpReport;
    auto result = isFlp ? FlpImport::importProject (engine, file, flpReport)
                        : ProjectIO::load (engine, file);
    history.clear(); // undo steps from the previous project don't apply
    projectFile = isFlp ? juce::File() : file;   // an imported FL project is saved as a new .amethyst
    noteRecentProject (file);

    metroBtn.setToggleState (engine.metronomeOn.load(), juce::dontSendNotification);
    view.startSeconds = 0.0;
    rebuildTrackComponents();

    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName (kAppTitle + " - " + file.getFileNameWithoutExtension());

    if (studio != nullptr) studio->sessionChanged();
    if (isFlp && result.wasOk())
        showTextReport ("FL Studio import - " + file.getFileNameWithoutExtension(), flpReport.toText());
    else if (result.failed())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                isFlp ? "Couldn't import FL Studio project" : "Project opened with problems", result.getErrorMessage());
}

void MainComponent::openFileFromSystem (const juce::File& f)
{
    skipWelcome = true;
    welcome = nullptr;
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe, f] { if (safe != nullptr) safe->openProjectFile (f); });
}

void MainComponent::noteRecentProject (const juce::File& f)
{
    recentProjects.addFile (f);
    appProperties.getUserSettings()->setValue ("recentProjects", recentProjects.toString());
}

void MainComponent::showWelcome()
{
    if (isTestInstance())
        return;
    WelcomeScreen::Callbacks cb;
    juce::Component::SafePointer<MainComponent> safe (this);
    auto close = [safe] { if (safe != nullptr) { safe->welcome = nullptr; safe->grabKeyboardFocus(); } };
    cb.close = close;
    cb.newEmpty = [safe, close] { close(); if (safe != nullptr) safe->startTemplate (0); };
    cb.vocalSession = [safe, close] { close(); if (safe != nullptr) safe->startTemplate (1); };
    cb.beatSession = [safe, close] { close(); if (safe != nullptr) safe->startTemplate (2); };
    cb.open = [safe, close] { close(); if (safe != nullptr) safe->openProject(); };
    cb.importFl = [safe, close]
    {
        close();
        if (safe == nullptr) return;
        safe->ctx.chooser = std::make_unique<juce::FileChooser> ("Import FL Studio project",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Image-Line/FL Studio/Projects"), "*.flp");
        safe->ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult().existsAsFile())
                safe->openProjectFile (fc.getResult());
        });
    };
    cb.openRecent = [safe, close] (const juce::File& f) { close(); if (safe != nullptr) safe->openProjectFile (f); };
    cb.setShowAtStart = [safe] (bool on) { if (safe != nullptr) safe->appProperties.getUserSettings()->setValue ("showWelcome", on); };

    recentProjects.removeNonExistentFiles();
    juce::Array<juce::File> recent;
    for (int i = 0; i < recentProjects.getNumFiles(); ++i)
        recent.add (recentProjects.getFile (i));
    const int tier = ctx.tier ? ctx.tier() : 0;
    const juce::String plan = tier >= 2 ? "Ultra Pro" : tier == 1 ? "Pro" : (account->isSignedIn() ? "Free" : "Not signed in");

    welcome = std::make_unique<WelcomeScreen> (std::move (cb), recent, tier, plan, appProperties.getUserSettings()->getBoolValue ("showWelcome", true));
    addAndMakeVisible (*welcome);
    welcome->setBounds (getLocalBounds());
    welcome->setAlpha (0.0f);
    juce::Desktop::getInstance().getAnimator().fadeIn (welcome.get(), 180);
    welcome->grabKeyboardFocus();
}

void MainComponent::startTemplate (int kind)
{
    startTemplate (kind, false);
}

void MainComponent::startTemplate (int kind, bool confirmed)
{
    if (kind == 0) { newProject(); return; }
    if (! confirmed && sessionHasWork())
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "New project",
                                            "Start a new project from this template? Anything you haven't saved in this one will be lost.",
                                            "Start", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create ([safe, kind] (int ok)
        {
            if (safe != nullptr && ok == 1)
                safe->startTemplate (kind, true);
        }));
        return;
    }

    const int tier = ctx.tier ? ctx.tier() : 0;
    auto addFx = [this] (Track& t, const char* id, const char* preset)
    {
        juce::PluginDescription desc;
        for (auto& d : Builtins::getDescriptions())
            if (d.fileOrIdentifier == id) desc = d;
        juce::String err;
        auto plugin = engine.createPlugin (desc, err);
        if (plugin == nullptr)
            return;   // not unlocked: leave the slot empty
        if (auto* am = dynamic_cast<AmethystPlugin*> (plugin.get()))
            for (int i = 0; i < (int) am->presets.size(); ++i)
                if (am->presets[(size_t) i].name == preset) { am->setCurrentProgram (i); break; }
        const int slot = engine.firstFreeSlot (t);
        if (slot >= 0)
            engine.insertPlugin (t, slot, std::move (plugin)).reset();
    };

    if (kind == 1)
    {
        startNewProject ({ "Beat", "Vocals", "Adlibs", "Harmonies" });
        auto& tracks = engine.getTracks();
        auto* vocals = tracks[1];
        vocals->armed = true;
        vocals->monitor = true;
        if (tier >= 2) addFx (*vocals, "amethyst.tune", "Default");
        if (tier >= 1)
        {
            addFx (*vocals, "amethyst.eq", "Vocal Presence");
            addFx (*vocals, "amethyst.dynamics", "Vocal Leveler");
            addFx (*vocals, "amethyst.deesser", "Default");
            addFx (*vocals, "amethyst.echo", "Vocal Quarter");
            addFx (*vocals, "amethyst.reverb", "Vocal Plate");
        }
        engine.setPlayThrough (*tracks[2], vocals);   // adlibs use the vocal chain
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (kAppTitle + " - Vocal session");
    }
    else
    {
        startNewProject ({ "Beat", "Drums", "Melody", "Bass" });
        if (tier >= 1) addFx (engine.getMaster(), "amethyst.master", "Balanced");
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
            w->setName (kAppTitle + " - Mix a beat");
    }
    rebuildTrackComponents();
}

void MainComponent::showFileMenu()
{
   #if JUCE_MAC
    const juce::String ctrl ("Cmd");
   #else
    const juce::String ctrl ("Ctrl");
   #endif
    juce::PopupMenu m;
    auto item = [&m] (int id, const juce::String& text, const juce::String& keys, bool enabled = true)
    {
        juce::PopupMenu::Item it (text);
        it.itemID = id;
        it.shortcutKeyDescription = keys;
        it.isEnabled = enabled;
        m.addItem (it);
    };
    item (1, "New project", ctrl + "+N");
    item (2, "Open...", ctrl + "+O");
    juce::PopupMenu recent;
    recentProjects.removeNonExistentFiles();
    for (int i = 0; i < recentProjects.getNumFiles(); ++i)
        recent.addItem (100 + i, recentProjects.getFile (i).getFileNameWithoutExtension());
    m.addSubMenu ("Open recent", recent, recent.getNumItems() > 0);
    m.addSeparator();
    item (3, "Save", ctrl + "+S", ! engine.isRecording());
    item (4, "Save as...", "Shift+" + ctrl + "+S", ! engine.isRecording());
    m.addSeparator();
    item (5, "Import FL Studio project (.flp)...", {});
    m.addSeparator();
    if (studio != nullptr)
    {
        studio->addToFileMenu (m);
        m.addSeparator();
    }
    item (6, "Welcome screen", {});

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fileBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0 || r >= 700000) return;
        auto& self = *safe;
        if (r >= 100) { self.openProjectFile (self.recentProjects.getFile (r - 100)); return; }
        switch (r)
        {
            case 1: self.newProject(); break;
            case 2: self.openProject(); break;
            case 3: self.saveProject (false); break;
            case 4: self.saveProject (true); break;
            case 5:
            {
                self.ctx.chooser = std::make_unique<juce::FileChooser> ("Import FL Studio project",
                    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Image-Line/FL Studio/Projects"), "*.flp");
                self.ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
                {
                    if (safe != nullptr && fc.getResult().existsAsFile())
                        safe->openProjectFile (fc.getResult());
                });
                break;
            }
            case 6: self.showWelcome(); break;
            default: break;
        }
    });
}

void MainComponent::showSettingsMenu()
{
    const int tier = ctx.tier ? ctx.tier() : 0;
    juce::PopupMenu m;
    {
        juce::PopupMenu::Item all ("Settings...");
        juce::Component::SafePointer<MainComponent> self (this);
        all.setID (700900).setAction ([self] { if (self != nullptr) self->showSettingsWindow(); });
        all.shortcutKeyDescription = "Ctrl+,";
        m.addItem (all);
        m.addSeparator();
    }
    m.addItem (1, "Audio device and inputs...");
    m.addItem (2, "Plug-in manager (scan VST3)...");
    m.addSeparator();

    juce::PopupMenu themes;
    for (int i = 0; i < Theme::kNumThemes; ++i)
    {
        const auto& th = Theme::kThemes[i];
        const bool unlocked = tier >= th.tier;
        juce::String label (th.name);
        if (! unlocked) label << (th.tier >= 2 ? "   (Ultra Pro)" : "   (Pro)");
        themes.addItem (200 + i, label, true, Theme::currentTheme() == i);
    }
    m.addSubMenu ("Colour theme", themes);

    juce::PopupMenu bgMenu;
    bgMenu.addItem (400, "Choose picture...");
    bgMenu.addSeparator();
    bgMenu.addItem (401, "Haze: light", customBackground.isValid(), customBackground.isValid() && backgroundHaze < 0.4f);
    bgMenu.addItem (402, "Haze: medium", customBackground.isValid(), customBackground.isValid() && backgroundHaze >= 0.4f && backgroundHaze < 0.75f);
    bgMenu.addItem (403, "Haze: heavy", customBackground.isValid(), customBackground.isValid() && backgroundHaze >= 0.75f);
    bgMenu.addSeparator();
    bgMenu.addItem (404, "Default background", customBackground.isValid());
    m.addSubMenu ("Background picture", bgMenu);
    {
        auto* st = appProperties.getUserSettings();
        const auto id = st->getValue ("discordAppId", AMETHYST_DISCORD_APP_ID);
        juce::PopupMenu dc;
        dc.addItem (500, "Show my activity on Discord", true, st->getBoolValue ("discordEnabled", true));
        dc.addItem (501, "Show the project name", true, st->getBoolValue ("discordShowProject", false));
        {
            juce::PopupMenu::Item planItem ("Show my plan (Pro / Ultra Pro)");
            juce::Component::SafePointer<MainComponent> self (this);
            planItem.setID (700501).setTicked (st->getBoolValue ("discordShowPlan", true)).setEnabled ((ctx.tier ? ctx.tier() : 0) >= 1)
                    .setAction ([self]
            {
                if (self == nullptr) return;
                auto* s = self->appProperties.getUserSettings();
                s->setValue ("discordShowPlan", ! s->getBoolValue ("discordShowPlan", true));
                self->updateDiscord();
            });
            dc.addItem (planItem);
        }
        dc.addSeparator();
        dc.addItem (502, "Set application ID...");
        dc.addItem (503, id.isEmpty() ? "Needs an application ID"
                         : (discord != nullptr && discord->isConnected() ? "Connected to Discord" : "Waiting for Discord (is it open?)"), false);
        m.addSubMenu ("Discord status", dc);
    }
    m.addSeparator();
    m.addItem (3, "Show welcome screen at start", true, appProperties.getUserSettings()->getBoolValue ("showWelcome", true));
    m.addSeparator();
    m.addItem (600, "Check for updates now");
    m.addItem (601, "Check for updates automatically", true, appProperties.getUserSettings()->getBoolValue ("autoUpdate", true));
    m.addItem (602, "AMETHYST 2026  version " + juce::String (ProjectInfo::versionString), false);

    juce::Component::SafePointer<MainComponent> safe (this);
    if (studio != nullptr) { m.addSeparator(); studio->addToSettingsMenu (m); }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&settingsBtn), [safe, tier] (int r)
    {
        if (r >= 700000) return;
        if (safe == nullptr || r == 0) return;
        auto& self = *safe;
        if (r == 1) self.showAudioSettings();
        else if (r == 2) self.showPluginManager();
        else if (r == 3)
        {
            auto* st = self.appProperties.getUserSettings();
            st->setValue ("showWelcome", ! st->getBoolValue ("showWelcome", true));
        }
        else if (r >= 200 && r < 200 + Theme::kNumThemes)
        {
            const int idx = r - 200;
            if (tier < Theme::kThemes[idx].tier)
            {
                if (self.ctx.showProPrompt)
                    self.ctx.showProPrompt (juce::String (Theme::kThemes[idx].name) + " colour theme", Theme::kThemes[idx].tier);
                return;
            }
            self.applyColourTheme (idx, true);
        }
        else if (r == 400) self.chooseBackground();
        else if (r >= 401 && r <= 403) self.setBackgroundHaze (r == 401 ? 0.25f : r == 402 ? 0.6f : 0.9f);
        else if (r == 404) { self.appProperties.getUserSettings()->removeValue ("backgroundImage"); self.loadCustomBackground(); }
    });
}

void MainComponent::checkForUpdates (bool userAsked)
{
    if (updater == nullptr || ! updater->isAvailable())
    {
        if (userAsked)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Updates",
                                                    "Automatic updates are available in the Windows version installed with the AMETHYST 2026 installer.");
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    updater->check ([safe, userAsked] (bool newer, Updater::Release rel, juce::String error)
    {
        if (safe == nullptr) return;
        auto* st = safe->appProperties.getUserSettings();
        if (error.isNotEmpty())
        {
            if (userAsked)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Updates", error);
            return;
        }
        if (! newer)
        {
            if (userAsked)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Updates",
                                                        "You're up to date - AMETHYST 2026 version " + juce::String (ProjectInfo::versionString) + ".");
            return;
        }
        if (! userAsked && st->getValue ("skipUpdateVersion") == rel.version)
            return;   // the user chose to skip this one

        auto* box = new juce::AlertWindow ("Update available",
                                           "AMETHYST 2026 version " + rel.version + " is ready (you have " + juce::String (ProjectInfo::versionString) + ")."
                                               + (rel.notes.isNotEmpty() ? "\n\nWhat's new:\n" + rel.notes : juce::String())
                                               + "\n\nYour session is kept and offered back after the update.",
                                           juce::MessageBoxIconType::InfoIcon);
        box->addButton ("Update now", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Later", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        box->addButton ("Skip this version", 2);
        box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, rel] (int r)
        {
            if (safe == nullptr) return;
            if (r == 2) safe->appProperties.getUserSettings()->setValue ("skipUpdateVersion", rel.version);
            if (r == 1) safe->startUpdate (rel);
        }), true);
    });
}

void MainComponent::startUpdate (const Updater::Release& rel)
{
    if (engine.isRecording() || engine.isExporting())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Update", "Finish recording or exporting first, then update.");
        return;
    }
    engine.stop();
    // keep this session safe: saved project is saved, anything else stays as the autosave copy
    if (projectFile != juce::File())
        saveProject (false);
    autosaveNow();
    if (auto info = juce::XmlDocument::parse (autosaveInfo()))
    {
        info->setAttribute ("reason", "update");
        info->writeTo (autosaveInfo());
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    Updater::downloadAndInstall (rel, updater->getTrustedPrefix(), [safe] (bool ok, juce::String error)
    {
        if (safe == nullptr) return;
        if (! ok)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update", error);
            return;
        }
        safe->keepAutosaveOnExit = true;
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    });
}

void MainComponent::updateDiscord()
{
    if (discord == nullptr)
        return;
    DiscordPresence::Activity a;
    if (engine.isRecording())        a.details = engine.isPunchRecording() ? "Punching in a vocal" : "Recording vocals";
    else if (engine.isExporting())   a.details = "Exporting a song";
    else if (engine.isAuditioning()) a.details = "Making a nightcore edit";
    else if (mixer.isVisible())      a.details = "Mixing";
    else if (engine.isPlaying())     a.details = "Listening back";
    else                             a.details = "Writing a song";

    int trackCount = 0;
    for (auto* t : engine.getTracks())
        if (! t->isAutomation()) ++trackCount;
    const bool showName = appProperties.getUserSettings()->getBoolValue ("discordShowProject", false);
    if (showName && projectFile != juce::File())
        a.state = projectFile.getFileNameWithoutExtension();
    else
        a.state = juce::String (trackCount) + " tracks  -  " + juce::String (juce::roundToInt (engine.bpm.load())) + " BPM";

    const int tier = ctx.tier ? ctx.tier() : 0;
    if (tier >= 2)      { a.smallImage = "ultra"; a.smallText = "AMETHYST Ultra Pro"; }
    else if (tier == 1) { a.smallImage = "pro";   a.smallText = "AMETHYST Pro"; }
    // the plan in the status text itself: "Recording vocals  |  Ultra Pro" (can be turned off)
    if (tier >= 1 && appProperties.getUserSettings()->getBoolValue ("discordShowPlan", true))
    {
        a.plan = tier >= 2 ? "Ultra Pro" : "Pro";
        const juce::String gem (juce::CharPointer_UTF8 (tier >= 2 ? "\xf0\x9f\x91\x91" : "\xf0\x9f\x92\x8e"));   // crown / gem
        a.details << "  " << gem << " " << a.plan;
    }
    discord->setActivity (a);
}

void MainComponent::askDiscordAppId()
{
    discordIdBox = std::make_unique<juce::AlertWindow> ("Discord application ID",
        "Create an application at discord.com/developers/applications, then paste its Application ID here.\n"
        "Under Rich Presence > Art Assets, upload the AMETHYST icon named \"amethyst\" (and optionally \"pro\" / \"ultra\").",
        juce::MessageBoxIconType::NoIcon);
    discordIdBox->addTextEditor ("id", appProperties.getUserSettings()->getValue ("discordAppId", AMETHYST_DISCORD_APP_ID), "Application ID");
    discordIdBox->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    discordIdBox->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe (this);
    discordIdBox->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int r)
    {
        if (safe == nullptr || safe->discordIdBox == nullptr) return;
        if (r == 1)
        {
            const auto id = safe->discordIdBox->getTextEditorContents ("id").retainCharacters ("0123456789");
            safe->appProperties.getUserSettings()->setValue ("discordAppId", id);
            if (safe->discord != nullptr)
                safe->discord->setApplicationId (id.isNotEmpty() ? id : juce::String (AMETHYST_DISCORD_APP_ID));
            safe->updateDiscord();
        }
        safe->discordIdBox = nullptr;
    }));
}

void MainComponent::peekMascot (const juce::String& kind, bool always)
{
    if (! always && ! appProperties.getUserSettings()->getBoolValue ("mascotEggs", true))
        return;
    if (welcome != nullptr || splash != nullptr || mascotPeek.isShowing())
        return;
    mascotPeek.show (Mascot::randomLine (kind));
}

void MainComponent::applyIconStyle (int style, bool save)
{
    Mascot::iconOn() = style == 1;
    if (save)
        appProperties.getUserSettings()->setValue ("iconStyle", style);
    backgroundCache = {};
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* c = juce::Desktop::getInstance().getComponent (i))
        {
            c->sendLookAndFeelChange();   // windows with cached headers redraw them
            c->repaint();
        }
    applyWindowIcon();
    repaint();
}

void MainComponent::applyWindowIcon()
{
    if (getPeer() == nullptr) return;
    windowIconApplied = true;
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setIcon (renderAppIcon (256));
}

void MainComponent::applyColourTheme (int index, bool save)
{
    Theme::applyTheme (index);
    if (auto* lf = dynamic_cast<GlassLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
        lf->refreshColours();
    if (save)
        appProperties.getUserSettings()->setValue ("colourTheme", index);
    backgroundCache = {};
    // every window repaints with the new palette (editors drop their cached backgrounds)
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* c = juce::Desktop::getInstance().getComponent (i))
        {
            c->sendLookAndFeelChange();
            c->repaint();
        }
    mixer.rebuild();
    repaint();
}

void MainComponent::showTextReport (const juce::String& title, const juce::String& text)
{
    auto* editor = new juce::TextEditor();
    editor->setMultiLine (true, false);
    editor->setReadOnly (true);
    editor->setScrollbarsShown (true);
    editor->setFont (juce::FontOptions (14.0f));
    editor->setColour (juce::TextEditor::backgroundColourId, Theme::windowBg);
    editor->setText (text, false);
    editor->setSize (720, 480);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (editor);
    o.dialogTitle = title;
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::exportMix()
{
    using Exporter::Format;
   #if JUCE_MAC
    const juce::String ctrl ("Cmd");
   #else
    const juce::String ctrl ("Ctrl");
   #endif

    juce::PopupMenu m;
    m.addSectionHeader ("Export the whole song as");
    auto add = [&m] (Format f, const juce::String& shortcut)
    {
        juce::PopupMenu::Item item (Exporter::displayName (f) + "...");
        item.itemID = 1 + (int) f;
        item.isEnabled = Exporter::isAvailable (f);
        item.shortcutKeyDescription = shortcut;
        m.addItem (item);
    };
    add (Format::Wav, ctrl + "+R");
    add (Format::Mp3, "Shift+" + ctrl + "+R");
    add (Format::Ogg, {});
    add (Format::Flac, {});
    add (Format::M4a, {});
    m.addSeparator();
    const int last = appProperties.getUserSettings()->getIntValue ("lastExportFormat", -1);
    m.addItem (100, "Last exported format" + (last >= 0 ? "  (" + Exporter::displayName ((Format) last) + ")" : juce::String()), last >= 0);
    if (studio != nullptr)
        studio->addToExportMenu (m);

    juce::Component::SafePointer<MainComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportBtn), [safe, last] (int r)
    {
        if (safe == nullptr || r == 0 || r > 100)
            return;
        safe->exportAs (r == 100 ? last : r - 1);
    });
}

void MainComponent::exportAs (int formatIndex)
{
    using Exporter::Format;
    const auto format = (Format) juce::jlimit (0, 4, formatIndex);

    if (engine.isRecording() || engine.isExporting())
        return;
    if (engine.getSessionLengthSamples() <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Nothing to export", "Record or import something first.");
        return;
    }
    if (! Exporter::isAvailable (format))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Not available",
                                                Exporter::displayName (format) + " export isn't available on this computer.");
        return;
    }

    const auto ext = Exporter::extension (format);
    const auto defaultName = projectFile != juce::File() ? projectFile.getFileNameWithoutExtension() : juce::String ("Mix");
    ctx.chooser = std::make_unique<juce::FileChooser> ("Export " + Exporter::displayName (format),
                                                       AudioEngine::getAppFolder().getChildFile (defaultName + ext), "*" + ext);

    juce::Component::SafePointer<MainComponent> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, format, ext] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;
        safe->appProperties.getUserSettings()->setValue ("lastExportFormat", (int) format);
        Exporter::exportSong (safe->engine, format, fc.getResult().withFileExtension (ext), safe.getComponent());
    });
}

//==============================================================================
Track* MainComponent::selectedTrackForTools()
{
    if (auto* sel = mixer.getSelected(); sel != nullptr && ! sel->isMaster && engine.isValidChannel (sel))
        return sel;
    for (auto* t : engine.getTracks())
        if (t->armed.load() && ! t->isAutomation())
            return t;
    for (auto* t : engine.getTracks())
        if (! t->isAutomation() && t->playThrough == nullptr && ! t->clips.empty())
            return t;
    return nullptr;
}

void MainComponent::registerCommands()
{
    using C = Studio2026::Command;
    auto& s = *studio;
    s.addCommand (C { "Play / pause", "Transport", "Space", [this] { togglePlay(); } });
    s.addCommand (C { "Record", "Transport", "R", [this] { toggleRecord(); } });
    s.addCommand (C { "Stop (again: back to start)", "Transport", {}, [this] { stopOrReturn(); } });
    s.addCommand (C { "Go to start", "Transport", "Home", [this] { engine.setTransportPosition (0); view.startSeconds = 0.0; repaint(); } });
    s.addCommand (C { "Metronome on / off", "Transport", {}, [this] { engine.metronomeOn = ! engine.metronomeOn.load(); metroBtn.setToggleState (engine.metronomeOn.load(), juce::dontSendNotification); } });
    s.addCommand (C { "Count-in: cycle off / 1 bar / 2 bars", "Transport", {}, [this] { setCountIn ((countInBars + 1) % 3); } });
    s.addCommand (C { "Punch-in recording on / off", "Transport", {}, [this] { engine.punchEnabled = ! engine.punchEnabled.load() && engine.hasPunchRegion(); repaint(); } });
    s.addCommand (C { "Save", "File", "Ctrl+S", [this] { saveProject (false); } });
    s.addCommand (C { "Save as...", "File", "Ctrl+Shift+S", [this] { saveProject (true); } });
    s.addCommand (C { "Open project...", "File", "Ctrl+O", [this] { openProject(); } });
    s.addCommand (C { "New project", "File", "Ctrl+N", [this] { newProject(); } });
    s.addCommand (C { "Welcome screen", "File", {}, [this] { showWelcome(); } });
    s.addCommand (C { "Undo", "Edit", "Ctrl+Z", [this] { undoEdit (false); } });
    s.addCommand (C { "Redo", "Edit", "Ctrl+Y", [this] { undoEdit (true); } });
    s.addCommand (C { "Move tool", "Edit", "V", [this] { setTool (EditTool::Move); } });
    s.addCommand (C { "Trim tool", "Edit", "T", [this] { setTool (EditTool::Trim); } });
    s.addCommand (C { "Cut tool", "Edit", "C", [this] { setTool (EditTool::Cut); } });
    s.addCommand (C { "Comp tool (keep the best part of each take)", "Edit", "X", [this] { setTool (EditTool::Comp); } });
    s.addCommand (C { "Add track", "Edit", {}, [this] { addTrack(); } });
    s.addCommand (C { "Mixer / arrangement", "View", "Tab", [this] { setMixerVisible (! mixer.isVisible()); } });
    s.addCommand (C { "Export WAV", "Export", "Ctrl+R", [this] { exportAs ((int) Exporter::Format::Wav); } });
    s.addCommand (C { "Export MP3", "Export", "Ctrl+Shift+R", [this] { exportAs ((int) Exporter::Format::Mp3); } });
    s.addCommand (C { "Export FLAC", "Export", {}, [this] { exportAs ((int) Exporter::Format::Flac); } });
    s.addCommand (C { "Nightcore Maker", "Tools", {}, [this] { showToolsMenu(); }, 2 });
    s.addCommand (C { "Find song tempo & key", "Tools", {}, [this] { analyseSong(); } });
    s.addCommand (C { "Settings...", "Settings", "Ctrl+,", [this] { showSettingsWindow(); } });
    s.addCommand (C { "Switch app icon (gem / anime angel)", "Settings", {}, [this] { applyIconStyle (Mascot::iconOn() ? 0 : 1, true); } });
    s.addCommand (C { "Summon the angel", "Fun", {}, [this] { peekMascot ("summon", true); } });
    s.addCommand (C { "What's new", "Help", {}, [this] { appProperties.getUserSettings()->removeValue ("lastSeenVersion"); maybeShowWhatsNew(); } });
    s.addCommand (C { "Audio device and inputs", "Settings", {}, [this] { showAudioSettings(); } });
    s.addCommand (C { "Plug-in manager (scan VST3)", "Settings", {}, [this] { showPluginManager(); } });
    s.addCommand (C { "Check for updates", "Settings", {}, [this] { checkForUpdates (true); } });
}

//==============================================================================
// --ui-test: drives the 2026 features through the real (hidden) window and writes snapshots.
int MainComponent::runUiSelfTest (const juce::File& folder)
{
    int fails = 0;
    auto check = [&fails] (bool ok, const juce::String& what)
    {
        std::printf ("[%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
        if (! ok) ++fails;
    };
    auto snap = [this, &folder] (const juce::String& name)
    {
        if (folder == juce::File()) return;
        folder.createDirectory();
        auto img = createComponentSnapshot (getLocalBounds());
        juce::FileOutputStream fos (folder.getChildFile (name + ".png"));
        if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); juce::PNGImageFormat().writeImageToStream (img, fos); }
    };

    welcome = nullptr;
    startNewProject ({ "Vocals", "Adlibs", "Beat" });
    resized();
    auto* vox = engine.getTracks()[0];
    const double sr = engine.getSampleRate();
    // a little audio so the lanes aren't empty
    {
        auto clip = std::make_shared<AudioClip>();
        auto buf = std::make_shared<juce::AudioBuffer<float>> (2, (int) (sr * 8));
        for (int i = 0; i < buf->getNumSamples(); ++i)
        {
            const float env = 0.5f + 0.5f * std::sin ((float) i / (float) sr * 1.3f);
            const float v = 0.35f * env * std::sin ((float) i * 0.05f) * (std::sin ((float) i / (float) sr * 9.0f) > -0.3f ? 1.0f : 0.05f);
            buf->setSample (0, i, v); buf->setSample (1, i, v);
        }
        clip->source = buf; clip->buffer = buf; clip->startSample = (juce::int64) (sr * 2.0); clip->name = "Lead take";
        engine.addClip (*vox, clip);
        vox->colour = juce::Colour (0xffff6fc8);
        vox->lowCutHz = 100.0f;
        rebuildTrackComponents();
    }

    // markers with M, navigation with , and .
    engine.setTransportPosition ((juce::int64) (sr * 4.0));
    keyPressed (juce::KeyPress ('m'));
    engine.setTransportPosition ((juce::int64) (sr * 8.0));
    keyPressed (juce::KeyPress ('m'));
    check (engine.markers.size() == 2 && engine.markers[0].name == "Intro" && engine.markers[1].name == "Verse",
           "M adds markers: " + juce::String ((int) engine.markers.size()) + (engine.markers.size() == 2 ? " (" + engine.markers[0].name + ", " + engine.markers[1].name + ")" : juce::String()));
    engine.setTransportPosition ((juce::int64) (sr * 10.0));
    keyPressed (juce::KeyPress (','));
    check (std::abs ((double) engine.getTransportPosition() / sr - 8.0) < 0.01, "',' jumps back to the Verse marker");
    keyPressed (juce::KeyPress (','));
    check (std::abs ((double) engine.getTransportPosition() / sr - 4.0) < 0.01, "',' again jumps to Intro");
    keyPressed (juce::KeyPress ('.'));
    check (std::abs ((double) engine.getTransportPosition() / sr - 8.0) < 0.01, "'.' jumps forward");

    // loop with L
    engine.setTransportPosition ((juce::int64) (sr * 2.1));
    keyPressed (juce::KeyPress ('l'));
    check (engine.loopEnabled.load() && engine.hasLoopRegion() && engine.loopStart.load() == (juce::int64) (sr * 2.0) && engine.loopEnd.load() == (juce::int64) (sr * 10.0), "L makes a 4-bar loop from the playhead's bar (2 s .. 10 s)");
    keyPressed (juce::KeyPress ('l'));
    check (! engine.loopEnabled.load() && engine.hasLoopRegion(), "L again turns the loop off (region kept)");
    keyPressed (juce::KeyPress ('l'));

    // monitor modes with Ctrl+M
    keyPressed (juce::KeyPress ('m', juce::ModifierKeys::commandModifier, 0));
    check (engine.monitorMode.load() == 1, "Ctrl+M switches to mono check");
    snap ("ui_arrange");
    for (int i = 0; i < 4; ++i) keyPressed (juce::KeyPress ('m', juce::ModifierKeys::commandModifier, 0));
    check (engine.monitorMode.load() == 0, "Ctrl+M cycles back to stereo");

    // palette, shortcuts, lyrics
    keyPressed (juce::KeyPress ('k', juce::ModifierKeys::commandModifier, 0));
    check (studio->isPaletteOpen() && studio->getNumCommands() >= 40, "Ctrl+K opens the command palette (" + juce::String (studio->getNumCommands()) + " commands)");
    snap ("ui_palette");
    studio->closeOverlays();
    keyPressed (juce::KeyPress (juce::KeyPress::F1Key));
    check (studio->isShortcutsOpen(), "F1 opens the shortcut sheet");
    snap ("ui_shortcuts");
    studio->closeOverlays();
    engine.lyrics = "Verse 1\nI was up all night, writing every line\nHook\nAmethyst skies, we shine";
    studio->sessionChanged();
    keyPressed (juce::KeyPress ('l', juce::ModifierKeys::commandModifier, 0));
    check (studio->isLyricsOpen(), "Ctrl+L opens the lyrics panel");
    studio->toast ("Saved My Song");
    check (studio->getNumToasts() >= 1, "toasts show");
    snap ("ui_lyrics_toast");
    studio->closeOverlays();

    // mixer strip with colour, low cut badge and a clip light
    vox->clipped = true;
    setMixerVisible (true);
    juce::Desktop::getInstance().getAnimator().cancelAllAnimations (true);
    mixer.setAlpha (1.0f);
    mixer.select (vox);
    resized();
    snap ("ui_mixer");

    // the Settings window, every page (offscreen)
    if (folder != juce::File())
        for (int tab = 0; tab < 6; ++tab)
        {
            auto win = SettingsWindow::create (makeSettingsHost(), tab);
            win->setSize (880, 620);
            auto img = win->createComponentSnapshot (win->getLocalBounds());
            juce::FileOutputStream fos (folder.getChildFile ("ui_settings_" + juce::String (tab) + ".png"));
            if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); juce::PNGImageFormat().writeImageToStream (img, fos); }
        }

    // the anime mascot: icon style (logo everywhere), a peek with a speech bubble
    {
        applyIconStyle (1, false);
        check (Mascot::iconOn() && Mascot::face().isValid() && Mascot::icon().isValid() && Mascot::figure().isValid(), "mascot art loads (face, icon, full figure)");
        mascotPeek.show ("Hi! You found me!");
        mascotPeek.jumpToUp();
        check (mascotPeek.isVisible() && mascotPeek.getWidth() > 0, "the angel peeks in from the corner");
        snap ("ui_mascot");
        if (folder != juce::File())
        {
            auto icon = renderAppIcon (256);
            juce::FileOutputStream fos (folder.getChildFile ("ui_mascot_icon.png"));
            if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); juce::PNGImageFormat().writeImageToStream (icon, fos); }
        }
        mascotPeek.setVisible (false);
        applyIconStyle (0, false);
        check (! Mascot::iconOn(), "switching back to the gem icon");
    }
    setMixerVisible (false);
    juce::Desktop::getInstance().getAnimator().cancelAllAnimations (true);
    viewport.setAlpha (1.0f);

    // Smart Vocal Chain, reverse reverb swell and version history (quiet: no windows, everything unlocked)
    {
        studio->setQuiet (true);
        auto savedTier = engine.unlockedTier;
        engine.unlockedTier = nullptr;
        studio->smartVocalChain (*vox);
        int n = 0; juce::StringArray names;
        for (auto& s : vox->inserts) if (s->plugin != nullptr) { ++n; names.add (s->plugin->getName()); }
        check (n == 5, "Smart Vocal Chain builds " + juce::String (n) + " effects: " + names.joinIntoString (", "));

        auto first = vox->clips.front();
        const auto before = vox->clips.size();
        studio->reverseReverbSwell (*vox, first);
        std::shared_ptr<AudioClip> swell;
        for (auto& c : vox->clips) if (c != first) swell = c;
        const bool swellOk = vox->clips.size() == before + 1 && swell != nullptr && swell->getEnd() > first->startSample.load()
                             && swell->startSample.load() < first->startSample.load() && swell->buffer != nullptr && swell->buffer->getMagnitude (0, swell->buffer->getNumSamples()) > 0.1f;
        check (swellOk, "reverse reverb swell: " + (swell != nullptr ? juce::String ((first->startSample.load() - swell->startSample.load()) / sr, 2) + " s lead-in before the clip" : juce::String ("missing")));
        snap ("ui_swell");
        if (swell != nullptr) { swell->file.deleteFile(); engine.removeClip (*vox, swell.get()); }

        auto proj = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("amethyst_versions_test.amethyst");
        proj.replaceWithText ("v1");
        studio->archiveBeforeSave (proj);
        juce::Thread::sleep (1100);
        proj.replaceWithText ("v2");
        studio->archiveBeforeSave (proj);
        auto vdir = proj.getParentDirectory().getChildFile ("amethyst_versions_test versions");
        const int kept = vdir.findChildFiles (juce::File::findFiles, false, "*.amethyst").size();
        check (kept == 2, "version history keeps each earlier save (" + juce::String (kept) + " versions)");
        vdir.deleteRecursively();
        proj.deleteFile();
        engine.unlockedTier = savedTier;
        studio->setQuiet (false);
        rebuildTrackComponents();
    }

    // copy / paste / duplicate / delete
    {
        auto lead = vox->clips.front();
        ctx.selection.selectOnly (vox, lead);
        keyPressed (juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0));
        engine.setTransportPosition ((juce::int64) (sr * 20.0));
        const auto before = vox->clips.size();
        keyPressed (juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0));
        std::shared_ptr<AudioClip> pasted;
        for (auto& c : vox->clips) if (c != lead) pasted = c;
        check (vox->clips.size() == before + 1 && pasted != nullptr && pasted->startSample.load() == (juce::int64) (sr * 20.0)
                   && pasted->getLength() == lead->getLength() && ctx.selection.isSelected (pasted.get()),
               "Ctrl+C / Ctrl+V pastes the take at the playhead (20 s), selected for dragging");

        // highlight 1 s of the take (Ctrl-drag) and paste that part elsewhere
        ctx.selection.clear();
        ctx.selection.rangeTrack = vox;
        ctx.selection.rangeStart = (juce::int64) (sr * 3.0);
        ctx.selection.rangeEnd = (juce::int64) (sr * 4.0);
        ctx.selection.lastTrack = engine.getTracks()[1];
        keyPressed (juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0));
        engine.setTransportPosition ((juce::int64) (sr * 30.0));
        keyPressed (juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0));
        auto* adlibs = engine.getTracks()[1];
        const bool rangeOk = adlibs->clips.size() == 1 && adlibs->clips[0]->startSample.load() == (juce::int64) (sr * 30.0)
                             && std::abs ((double) adlibs->clips[0]->getLength() / sr - 1.0) < 0.001;
        check (rangeOk, "a highlighted 1 s range copies and pastes onto another track");
        snap ("ui_paste");

        keyPressed (juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0));
        check (adlibs->clips.size() == 2 && adlibs->clips[1]->startSample.load() == (juce::int64) (sr * 31.0), "Ctrl+D duplicates right after");
        keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        check (adlibs->clips.size() == 1, "Delete removes the selection");
        undoEdit (false);
        check (adlibs->clips.size() == 2, "undo brings it back");

        // Ctrl+X on a range cuts the hole out of the take
        ctx.selection.clear();
        ctx.selection.rangeTrack = vox;
        ctx.selection.rangeStart = (juce::int64) (sr * 4.0);
        ctx.selection.rangeEnd = (juce::int64) (sr * 5.0);
        keyPressed (juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0));
        bool hole = true;
        for (auto& c : vox->clips)
            if (c->startSample.load() < (juce::int64) (sr * 5.0) - 10 && c->getEnd() > (juce::int64) (sr * 4.0) + 10) hole = false;
        check (hole, "Ctrl+X cuts the highlighted second out of the take");
        rebuildTrackComponents();
    }

    // undo for mixer and plug-in changes
    {
        auto settle = [this] { history.pollMixer (false); juce::Thread::sleep (650); history.pollMixer (false); history.pollMixer (false); };
        history.pollMixer (false);                        // baseline
        settle();
        vox->volumeDb = -9.0f;                            // a fader move
        settle();
        undoEdit (false);
        check (std::abs (vox->volumeDb.load()) < 0.01f || std::abs (vox->volumeDb.load() + 9.0f) > 0.01f,
               "undo puts the fader back (" + juce::String (vox->volumeDb.load(), 1) + " dB)");
        const float afterUndo = vox->volumeDb.load();
        undoEdit (true);
        check (std::abs (vox->volumeDb.load() + 9.0f) < 0.01f && std::abs (afterUndo + 9.0f) > 0.01f, "redo moves it again (-9 dB)");

        // insert a plug-in, then change one of its knobs
        auto savedTier = engine.unlockedTier;
        engine.unlockedTier = nullptr;
        juce::PluginDescription eqDesc;
        for (auto& d : Builtins::getDescriptions()) if (d.fileOrIdentifier == "amethyst.eq") eqDesc = d;
        juce::String err;
        const int freeSlot = engine.firstFreeSlot (*vox);
        engine.insertPlugin (*vox, freeSlot, engine.createPlugin (eqDesc, err)).reset();
        settle();
        auto* eq = dynamic_cast<AmethystPlugin*> (vox->inserts[(size_t) freeSlot]->plugin.get());
        if (eq != nullptr) eq->setParam ("b4_gain", 6.0f);
        settle();
        undoEdit (false);
        eq = dynamic_cast<AmethystPlugin*> (vox->inserts[(size_t) freeSlot]->plugin.get());
        check (eq != nullptr && std::abs (eq->get ("b4_gain")) < 0.01f, "undo resets a plug-in knob (band 4 gain back to 0 dB)");
        undoEdit (false);
        check (vox->inserts[(size_t) freeSlot]->plugin == nullptr, "undo removes a plug-in that was added");
        undoEdit (true);
        eq = dynamic_cast<AmethystPlugin*> (vox->inserts[(size_t) freeSlot]->plugin.get());
        check (eq != nullptr, "redo brings the plug-in back");
        engine.unlockedTier = savedTier;
    }

    // hold / drag the right mouse button over clips to delete them (no menu)
    {
        TrackComponent* tc = nullptr;
        for (auto* t : trackComps) if (&t->getTrack() == engine.getTracks()[1]) tc = t;
        auto* adl = engine.getTracks()[1];
        if (tc != nullptr && ! adl->clips.empty())
        {
            view.startSeconds = 25.0;
            resized();
            tc->repaint();
            const auto target = adl->clips.front();
            const double sr2 = engine.getSampleRate();
            const auto lane = tc->getLocalBounds().withTrimmedLeft (kHeaderWidth);
            const float x = view.timeToX (((double) target->startSample.load() + target->getLength() * 0.5) / sr2, lane);
            const juce::Point<float> p0 (x, (float) tc->getHeight() * 0.5f), p1 (x + 12.0f, (float) tc->getHeight() * 0.5f);
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto mods = juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier);
            const auto now = juce::Time::getCurrentTime();
            auto ev = [&] (juce::Point<float> p, bool dragged)
            {
                return juce::MouseEvent (src, p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, tc, tc, now, p0, now, 1, dragged);
            };
            const auto before = adl->clips.size();
            tc->mouseDown (ev (p0, false));
            tc->mouseDrag (ev (p1, true));
            tc->mouseUp (ev (p1, true));
            check (adl->clips.size() == before - 1, "right-drag over a clip deletes it (" + juce::String ((int) before) + " -> " + juce::String ((int) adl->clips.size()) + " clips, no menu)");
            undoEdit (false);
            check (adl->clips.size() == before, "undo brings the right-deleted clip back");
            view.startSeconds = 0.0;
        }
        else
            check (false, "right-drag test: no clip to delete");
    }

    // return to start on stop
    {
        engine.setTransportPosition ((juce::int64) (sr * 6.0));
        keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        engine.setTransportPosition ((juce::int64) (sr * 9.0));   // as if it had played on
        keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        check (! engine.isPlaying() && engine.getTransportPosition() == (juce::int64) (sr * 6.0), "Space stops and returns to where playback started (6 s)");
        engine.returnOnStop = false;
        keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        engine.setTransportPosition ((juce::int64) (sr * 9.0));
        keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        check (engine.getTransportPosition() == (juce::int64) (sr * 9.0), "with the setting off, stop stays where it is");
        engine.returnOnStop = true;
    }

    // license-check frames (rendered offscreen): checking, Ultra success, failure
    if (folder != juce::File())
    {
        auto frame = [] (int steps, bool ok, bool bad, double t, double since)
        {
            LicenseCheck::Frame f;
            f.stepsDone = steps; f.success = ok; f.failed = bad; f.tier = 2; f.t = t; f.sinceEnd = since;
            f.error = "Invalid login credentials";
            return f;
        };
        const std::pair<const char*, LicenseCheck::Frame> frames[] = {
            { "license_checking", frame (2, false, false, 1.3, 0.0) },
            { "license_ultra", frame (4, true, false, 2.5, 0.9) },
            { "license_failed", frame (1, false, true, 1.0, 0.6) } };
        for (auto& [name, fr] : frames)
        {
            juce::Image img (juce::Image::ARGB, 480, 560, true);
            {
                juce::Graphics g (img);
                g.fillAll (Theme::windowBg);
                LicenseCheck::paintFrame (g, img.getBounds().toFloat(), fr);
            }
            juce::FileOutputStream fos (folder.getChildFile (juce::String (name) + ".png"));
            if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); juce::PNGImageFormat().writeImageToStream (img, fos); }
        }
    }

    // startup animation frames (rendered offscreen)
    if (folder != juce::File())
        for (double t : { 0.35, 0.9, 1.6 })
        {
            juce::Image img (juce::Image::ARGB, 1200, 760, true);
            {
                juce::Graphics g (img);   // drawing lands in the image when this closes
                g.fillAll (Theme::background);
                StartupSplash::paintFrame (g, img.getBounds().toFloat(), t);
            }
            juce::FileOutputStream fos (folder.getChildFile ("splash_" + juce::String (t, 2) + ".png"));
            if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); juce::PNGImageFormat().writeImageToStream (img, fos); }
        }

    // project round trip keeps markers, loop, lyrics, colour, low cut
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("amethyst_uitest.amethyst");
        ProjectIO::save (engine, f);
        const auto markers = engine.markers.size();
        const auto lyr = engine.lyrics;
        engine.clearSessionNotes();
        vox->colour = {};
        ProjectIO::load (engine, f);
        auto* v2 = engine.getTracks()[0];
        check (engine.markers.size() == markers && engine.lyrics == lyr && engine.hasLoopRegion() && v2 != nullptr
                   && v2->colour == juce::Colour (0xffff6fc8) && std::abs (v2->lowCutHz.load() - 100.0f) < 0.1f,
               "project keeps markers, loop, lyrics, track colour and low cut");
        f.deleteFile();
        rebuildTrackComponents();
    }
    return fails;
}

//==============================================================================
void MainComponent::showSettingsWindow (int tab)
{
    SettingsWindow::show (makeSettingsHost(), this, tab);
}

SettingsWindow::Host MainComponent::makeSettingsHost()
{
    SettingsWindow::Host h;
    h.engine = &engine;
    h.props = &appProperties;
    h.tier = [this] { return ctx.tier ? ctx.tier() : 0; };
    h.proPrompt = [this] (const juce::String& f, int t) { if (ctx.showProPrompt) ctx.showProPrompt (f, t); };
    h.setTheme = [this] (int i) { applyColourTheme (i, true); };
    h.getIconStyle = [] { return Mascot::iconOn() ? 1 : 0; };
    h.setIconStyle = [this] (int s) { applyIconStyle (s, true); };
    h.chooseBackground = [this] { chooseBackground(); };
    h.removeBackground = [this] { appProperties.getUserSettings()->removeValue ("backgroundImage"); loadCustomBackground(); };
    h.setHaze = [this] (float v) { setBackgroundHaze (v); };
    h.getCountIn = [this] { return countInBars; };
    h.setCountIn = [this] (int b) { setCountIn (b); };
    h.checkUpdates = [this] { checkForUpdates (true); };
    h.openPluginManager = [this] { showPluginManager(); };
    h.discordChanged = [this]
    {
        if (discord != nullptr) discord->setEnabled (appProperties.getUserSettings()->getBoolValue ("discordEnabled", true));
        updateDiscord();
    };
    h.shortcutsText = [this]
    {
        juce::String s;
        for (auto& line : studio != nullptr ? studio->getShortcutLines() : juce::StringArray()) s << line << "\n";
        return s;
    };
    return h;
}

void MainComponent::maybeShowWhatsNew()
{
    auto* st = appProperties.getUserSettings();
    const juce::String current (ProjectInfo::versionString);
    const auto last = st->getValue ("lastSeenVersion");
    st->setValue ("lastSeenVersion", current);
    if (last == current || ! st->getBoolValue ("showWhatsNew", true))
        return;
    if (last.isEmpty() && st->getValue ("recentProjects").isEmpty())
        return;   // brand new install: the welcome screen does the introductions
    SettingsWindow::showWhatsNew (current, {
        "Settings window with everything in one place (Ctrl+,)",
        "Take comping: swipe across takes to keep the best parts (X)",
        "Crossfades where clips overlap, and a volume handle on every clip",
        "Undo now covers the mixer and plug-in changes",
        "Send buses: shared reverb / delay channels with per-track sends",
        "Plug-in crash protection: a crashing plug-in is switched off, not the app",
        "Signed licenses that work offline for 14 days",
    }, this);
}
