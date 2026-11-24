#include "ClipEditor.h"
#include "PluginWindows.h"

namespace
{
    constexpr int kKnobW = 66, kKnobH = 66;
}

ClipEditor::ClipEditor (AppContext& c, Track& t, std::shared_ptr<AudioClip> cl)
    : ctx (c), track (t), clip (cl), thumbnail (256, c.engine.audioFormats, c.thumbCache)
{
    onLed.setTooltip ("Clip on / off");
    onLed.onClick = [this] { if (auto cp = clip.lock()) cp->enabled = onLed.getToggleState(); };
    addAndMakeVisible (onLed);

    for (auto* k : { &panK, &volK, &pitchK, &fineK, &speedK, &startK, &lengthK, &fadeInK, &fadeOutK, &trimK,
                     &boostK, &eqK, &ringMixK, &ringFreqK, &cutK, &resK, &reverbK, &delayK, &pogoK })
    {
        k->slider.onValueChange = [this] { changed(); };
        addAndMakeVisible (k);
    }

    trimK.slider.textFromValueFunction = [] (double v) { return v <= -89.5 ? juce::String ("Off") : juce::String (v, 1) + " dB"; };
    eqK.slider.textFromValueFunction = [] (double v) { return std::abs (v) < 0.01 ? juce::String ("Flat") : (v < 0 ? "Dark " : "Bright ") + juce::String (std::abs (v) * 100.0, 0) + "%"; };
    cutK.slider.textFromValueFunction = [] (double v) { return v >= 19500.0 ? juce::String ("Open") : juce::String ((int) v) + " Hz"; };
    pogoK.slider.textFromValueFunction = [] (double v) { return juce::String (v * 12.0, 1) + " st glide"; };

    replaceBtn.setTooltip ("Replace audio file...");
    replaceBtn.onClick = [this] { replaceFile(); };
    revealBtn.setTooltip ("Show file in Explorer");
    revealBtn.onClick = [this] { if (auto cp = clip.lock()) cp->file.revealToUser(); };
    deleteBtn.setTooltip ("Delete this clip from the track");
    deleteBtn.onClick = [this]
    {
        if (auto cp = clip.lock())
            if (ctx.engine.isValidChannel (&track))
                ctx.engine.removeClip (track, cp.get());
    };
    for (auto* b : { &replaceBtn, &revealBtn, &deleteBtn })
        addAndMakeVisible (b);

    declickBox.addItemList ({ "Off", "Short (3 ms)", "Smooth (20 ms)" }, 1);
    declickBox.setTooltip ("Tiny fades at the clip edges to stop clicks");
    declickBox.onChange = [this] { changed(); };
    addAndMakeVisible (declickBox);

    resetBtn.setTooltip ("Put every clip setting back to default");
    resetBtn.onClick = [this]
    {
        if (auto cp = clip.lock())
        {
            cp->settings = ClipSettings();
            loadFromSettings();
            ctx.engine.requestRender (cp);
        }
    };
    addAndMakeVisible (resetBtn);

    modeBox.addItemList ({ "Resample", "Stretch (keep pitch)" }, 1);
    modeBox.setTooltip ("Resample: speed changes pitch, like tape.  Stretch: change speed and pitch independently.");
    modeBox.onChange = [this] { changed(); };
    addAndMakeVisible (modeBox);

    for (auto* b : { &shapeTab, &colorTab })
        addAndMakeVisible (b);
    shapeTab.onClick = [this] { showPage (0); };
    colorTab.onClick = [this] { showPage (1); };

    for (auto* tb : { &removeDC, &normalize, &reverse, &invert, &stereoFade, &swapStereo, &softClip })
    {
        tb->onClick = [this] { changed(); };
        addAndMakeVisible (tb);
    }

    revA.onClick = [this] { revA.setToggleState (true, juce::dontSendNotification); revB.setToggleState (false, juce::dontSendNotification); changed(); };
    revB.onClick = [this] { revB.setToggleState (true, juce::dontSendNotification); revA.setToggleState (false, juce::dontSendNotification); changed(); };
    for (auto* b : { &revA, &revB })
        addAndMakeVisible (b);
    revA.setTooltip ("Reverb A: tight room");
    revB.setTooltip ("Reverb B: big hall");

    shapePage = { &removeDC, &normalize, &reverse, &invert, &stereoFade, &swapStereo, &startK, &lengthK, &fadeInK, &fadeOutK, &trimK };
    colorPage = { &boostK, &eqK, &ringMixK, &ringFreqK, &cutK, &resK, &softClip, &revA, &revB, &reverbK, &delayK, &pogoK };

    disableFocusOnChildren (*this);
    loadFromSettings();
    showPage (0);
    updateThumbnail();

    setSize (700, 640);
    startTimerHz (30);
}

ClipEditor::~ClipEditor()
{
    stopTimer();
    // flush a pending edit so it isn't lost when the window closes quickly
    if (renderCountdown > 0)
        if (auto cp = clip.lock())
            ctx.engine.requestRender (cp);
}

bool ClipEditor::isValid() const
{
    auto cp = clip.lock();
    if (cp == nullptr || ! ctx.engine.isValidChannel (&track))
        return false;
    return std::any_of (track.clips.begin(), track.clips.end(), [&] (const auto& c) { return c == cp; });
}

//==============================================================================
void ClipEditor::loadFromSettings()
{
    auto cp = clip.lock();
    if (cp == nullptr)
        return;

    const juce::ScopedValueSetter<bool> svs (loading, true);
    const auto& s = cp->settings;
    const auto n = juce::dontSendNotification;

    onLed.setToggleState (cp->enabled.load(), n);
    panK.slider.setValue (s.pan, n);
    volK.slider.setValue (s.gainDb, n);
    pitchK.slider.setValue (s.pitchSemis, n);
    declickBox.setSelectedId (s.declick + 1, n);
    modeBox.setSelectedId (s.timeMode + 1, n);
    fineK.slider.setValue (s.fineCents, n);
    speedK.slider.setValue (s.speed, n);

    removeDC.setToggleState (s.removeDC, n);
    normalize.setToggleState (s.normalize, n);
    reverse.setToggleState (s.reverse, n);
    invert.setToggleState (s.invert, n);
    stereoFade.setToggleState (s.stereoFade, n);
    swapStereo.setToggleState (s.swapStereo, n);
    startK.slider.setValue (s.startPct, n);
    lengthK.slider.setValue (s.lengthPct, n);
    fadeInK.slider.setValue (s.fadeInMs, n);
    fadeOutK.slider.setValue (s.fadeOutMs, n);
    trimK.slider.setValue (s.trimDb, n);

    boostK.slider.setValue (s.boostDb, n);
    eqK.slider.setValue (s.eqTilt, n);
    ringMixK.slider.setValue (s.ringMix, n);
    ringFreqK.slider.setValue (s.ringFreq, n);
    cutK.slider.setValue (s.cutHz, n);
    resK.slider.setValue (s.resonance, n);
    softClip.setToggleState (s.softClip, n);
    revA.setToggleState (s.reverbType != 1, n);
    revB.setToggleState (s.reverbType == 1, n);
    reverbK.slider.setValue (s.reverb, n);
    delayK.slider.setValue (s.delay, n);
    pogoK.slider.setValue (s.pogo, n);
}

void ClipEditor::storeToSettings()
{
    auto cp = clip.lock();
    if (cp == nullptr)
        return;

    auto& s = cp->settings;
    s.pan = (float) panK.slider.getValue();
    s.gainDb = (float) volK.slider.getValue();
    s.pitchSemis = (float) pitchK.slider.getValue();
    s.declick = juce::jmax (0, declickBox.getSelectedId() - 1);
    s.timeMode = juce::jmax (0, modeBox.getSelectedId() - 1);
    s.fineCents = (float) fineK.slider.getValue();
    s.speed = (float) speedK.slider.getValue();

    s.removeDC = removeDC.getToggleState();
    s.normalize = normalize.getToggleState();
    s.reverse = reverse.getToggleState();
    s.invert = invert.getToggleState();
    s.stereoFade = stereoFade.getToggleState();
    s.swapStereo = swapStereo.getToggleState();
    s.startPct = (float) startK.slider.getValue();
    s.lengthPct = (float) lengthK.slider.getValue();
    s.fadeInMs = (float) fadeInK.slider.getValue();
    s.fadeOutMs = (float) fadeOutK.slider.getValue();
    s.trimDb = (float) trimK.slider.getValue();

    s.boostDb = (float) boostK.slider.getValue();
    s.eqTilt = (float) eqK.slider.getValue();
    s.ringMix = (float) ringMixK.slider.getValue();
    s.ringFreq = (float) ringFreqK.slider.getValue();
    s.cutHz = (float) cutK.slider.getValue();
    s.resonance = (float) resK.slider.getValue();
    s.softClip = softClip.getToggleState();
    s.reverbType = revB.getToggleState() ? 1 : 0;
    s.reverb = (float) reverbK.slider.getValue();
    s.delay = (float) delayK.slider.getValue();
    s.pogo = (float) pogoK.slider.getValue();
}

void ClipEditor::changed()
{
    if (loading)
        return;
    storeToSettings();
    renderCountdown = 5; // ~150 ms debounce while knobs move
}

void ClipEditor::showPage (int p)
{
    page = p;
    shapeTab.setToggleState (p == 0, juce::dontSendNotification);
    colorTab.setToggleState (p == 1, juce::dontSendNotification);
    for (auto* c : shapePage) c->setVisible (p == 0);
    for (auto* c : colorPage) c->setVisible (p == 1);
    repaint();
}

void ClipEditor::updateThumbnail()
{
    auto cp = clip.lock();
    const juce::AudioBuffer<float>* buf = cp != nullptr ? cp->buffer.get() : nullptr;
    if (buf == thumbBuffer)
        return;

    thumbBuffer = buf;
    if (buf != nullptr && buf->getNumSamples() > 0)
    {
        thumbnail.reset (buf->getNumChannels(), ctx.engine.getSampleRate(), buf->getNumSamples());
        thumbnail.addBlock (0, *buf, 0, buf->getNumSamples());
    }
    else
    {
        thumbnail.clear();
    }
    repaint (waveCard);
}

void ClipEditor::replaceFile()
{
    ctx.chooser = std::make_unique<juce::FileChooser> ("Replace audio", juce::File(), ctx.engine.audioFormats.getWildcardForAllFormats());
    juce::Component::SafePointer<ClipEditor> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || ! fc.getResult().existsAsFile())
            return;
        if (auto cp = safe->clip.lock())
            if (! safe->ctx.engine.replaceClipSource (*cp, fc.getResult()))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't read file", fc.getResult().getFullPathName());
        safe->repaint();
    });
}

void ClipEditor::timerCallback()
{
    if (renderCountdown > 0 && --renderCountdown == 0)
        if (auto cp = clip.lock())
            ctx.engine.requestRender (cp);

    updateThumbnail();

    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        if (auto cp = clip.lock())
        {
            const auto title = cp->file.getFileNameWithoutExtension() + "  -  " + track.name;
            if (w->getName() != title)
                w->setName (title);
        }

    repaint (waveCard.withHeight (30));
}

//==============================================================================
void ClipEditor::paint (juce::Graphics& g)
{
    if (backgroundCache.isNull() || backgroundCache.getWidth() != getWidth() || backgroundCache.getHeight() != getHeight())
    {
        backgroundCache = juce::Image (juce::Image::RGB, juce::jmax (1, getWidth()), juce::jmax (1, getHeight()), false);
        juce::Graphics bg (backgroundCache);
        Glass::background (bg, getLocalBounds());
    }
    g.drawImageAt (backgroundCache, 0, 0);

    auto cp = clip.lock();

    // Header
    Glass::panel (g, headerArea.toFloat(), 14.0f, 1.2f);
    drawLogoMark (g, headerArea.withWidth (54).reduced (10).toFloat());
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    auto nameArea = headerArea.withTrimmedLeft (58).withWidth (260);
    g.drawText (cp != nullptr ? cp->file.getFileNameWithoutExtension() : juce::String ("(clip removed)"),
                nameArea.removeFromTop (nameArea.getHeight() / 2 + 4).withTrimmedTop (8), juce::Justification::bottomLeft, true);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText ("on " + track.name, nameArea, juce::Justification::topLeft, true);

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.drawText ("ON", onLed.getBounds().translated (0, 20).withHeight (14).expanded (8, 0), juce::Justification::centred);

    // Cards
    Glass::panel (g, fileCard.toFloat());
    Glass::title (g, fileCard.withHeight (30).reduced (12, 0), "File");
    if (cp != nullptr)
    {
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (cp->file.getFileName(), fileCard.reduced (14, 0).withTrimmedTop (34).withHeight (22), juce::Justification::centredLeft, true);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (cp->file.getParentDirectory().getFullPathName(), fileCard.reduced (14, 0).withTrimmedTop (56).withHeight (16),
                    juce::Justification::centredLeft, true);
    }

    Glass::panel (g, renderCard.toFloat());
    Glass::title (g, renderCard.withHeight (30).reduced (12, 0), "Render");
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText ("Declicking", renderCard.reduced (14, 0).withTrimmedTop (36).withHeight (24), juce::Justification::centredLeft);

    if (cp != nullptr && cp->source != nullptr)
    {
        const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
        auto info = renderCard.reduced (14, 0).withTrimmedTop (100).withHeight (70);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        juce::String text;
        text << "Source   " << juce::String (cp->source->getNumSamples() / sr, 2) << " s,  "
             << (cp->source->getNumChannels() > 1 ? "stereo" : "mono") << "\n"
             << "Result   " << juce::String ((double) cp->getLength() / sr, 2) << " s\n"
             << "Rate     " << juce::String ((int) sr) << " Hz";
        g.drawFittedText (text, info, juce::Justification::topLeft, 4);
    }

    Glass::panel (g, timeCard.toFloat());
    Glass::title (g, timeCard.withHeight (30).reduced (12, 0), "Time stretching");
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Mode", modeBox.getBounds().translated (0, -18).withHeight (16), juce::Justification::centredLeft);

    Glass::panel (g, fxCard.toFloat());
    Glass::title (g, fxCard.withHeight (34).reduced (12, 0), "Baked effects");

    if (page == 1)
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText ("ROOM", revA.getBounds().translated (0, revA.getHeight() + 2).withHeight (14).expanded (6, 0), juce::Justification::centred);
        g.drawText ("HALL", revB.getBounds().translated (0, revB.getHeight() + 2).withHeight (14).expanded (6, 0), juce::Justification::centred);
    }

    // Waveform
    Glass::panel (g, waveCard.toFloat());
    auto waveArea = waveCard.reduced (10).withTrimmedTop (22);
    Glass::well (g, waveArea.toFloat(), 8.0f);
    Glass::title (g, waveCard.withHeight (30).reduced (12, 0), "Result");

    if (ctx.engine.isRendering())
    {
        g.setColour (Theme::accent2);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("rendering...", waveCard.withHeight (30).reduced (14, 0), juce::Justification::centredRight);
    }

    if (thumbnail.getTotalLength() > 0.0)
    {
        auto inner = waveArea.reduced (6, 8);
        const bool on = cp != nullptr && cp->enabled.load();
        g.setGradientFill (juce::ColourGradient (Theme::wave.withAlpha (on ? 0.95f : 0.3f), (float) inner.getX(), (float) inner.getY(),
                                                 Theme::accent.withAlpha (on ? 0.9f : 0.3f), (float) inner.getX(), (float) inner.getBottom(), false));
        thumbnail.drawChannels (g, inner, 0.0, thumbnail.getTotalLength(), 1.0f);
    }
}

void ClipEditor::resized()
{
    backgroundCache = {};
    auto r = getLocalBounds().reduced (12);

    headerArea = r.removeFromTop (78);
    {
        auto h = headerArea.reduced (12, 6);
        pitchK.setBounds (h.removeFromRight (kKnobW));
        volK.setBounds (h.removeFromRight (kKnobW));
        panK.setBounds (h.removeFromRight (kKnobW));
        h.removeFromRight (8);
        onLed.setBounds (h.removeFromRight (30).withSizeKeepingCentre (22, 22).translated (0, -6));
    }

    r.removeFromTop (12);
    waveCard = r.removeFromBottom (150);
    r.removeFromBottom (12);

    auto left = r.removeFromLeft (240);
    r.removeFromLeft (12);

    fileCard = left.removeFromTop (118);
    {
        auto row = fileCard.reduced (14, 0).withTrimmedTop (78).withHeight (30);
        replaceBtn.setBounds (row.removeFromLeft (40));
        row.removeFromLeft (6);
        revealBtn.setBounds (row.removeFromLeft (40));
        row.removeFromLeft (6);
        deleteBtn.setBounds (row.removeFromLeft (40));
    }

    left.removeFromTop (12);
    renderCard = left;
    {
        auto rr = renderCard.reduced (14, 0);
        declickBox.setBounds (rr.withTrimmedTop (62).withHeight (26));
        resetBtn.setBounds (rr.withTrimmedTop (renderCard.getHeight() - 42).withHeight (28));
    }

    timeCard = r.removeFromTop (118);
    {
        auto tr = timeCard.reduced (14, 0).withTrimmedTop (34);
        auto knobs = tr.removeFromRight (kKnobW * 2);
        fineK.setBounds (knobs.removeFromLeft (kKnobW).withHeight (kKnobH + 8));
        speedK.setBounds (knobs.removeFromLeft (kKnobW).withHeight (kKnobH + 8));
        modeBox.setBounds (tr.withTrimmedTop (26).withHeight (28).withWidth (juce::jmin (tr.getWidth() - 10, 190)));
    }

    r.removeFromTop (12);
    fxCard = r;
    {
        auto tabs = fxCard.withHeight (34).reduced (12, 5);
        colorTab.setBounds (tabs.removeFromRight (64));
        tabs.removeFromRight (4);
        shapeTab.setBounds (tabs.removeFromRight (64));

        auto content = fxCard.reduced (14, 0).withTrimmedTop (40).withTrimmedBottom (10);

        // Shape page
        {
            auto c = content;
            auto toggles = c.removeFromTop (84);
            auto col1 = toggles.removeFromLeft (toggles.getWidth() / 2);
            for (auto* tb : { &removeDC, &normalize, &reverse })
                tb->setBounds (col1.removeFromTop (28));
            for (auto* tb : { &invert, &stereoFade, &swapStereo })
                tb->setBounds (toggles.removeFromTop (28));

            c.removeFromTop (8);
            auto knobs = c.removeFromTop (kKnobH + 8);
            const int w = knobs.getWidth() / 5;
            for (auto* k : { &startK, &lengthK, &fadeInK, &fadeOutK, &trimK })
                k->setBounds (knobs.removeFromLeft (w));
        }

        // Color page
        {
            auto c = content;
            auto row1 = c.removeFromTop (kKnobH + 8);
            const int w = row1.getWidth() / 6;
            for (auto* k : { &boostK, &eqK, &ringMixK, &ringFreqK, &cutK, &resK })
                k->setBounds (row1.removeFromLeft (w));

            c.removeFromTop (10);
            auto row2 = c.removeFromTop (kKnobH + 8);
            softClip.setBounds (row2.removeFromLeft (96).withSizeKeepingCentre (96, 28));
            auto ab = row2.removeFromLeft (70).withSizeKeepingCentre (64, 26).translated (0, -8);
            revA.setBounds (ab.removeFromLeft (30));
            ab.removeFromLeft (4);
            revB.setBounds (ab.removeFromLeft (30));
            const int w2 = row2.getWidth() / 3;
            for (auto* k : { &reverbK, &delayK, &pogoK })
                k->setBounds (row2.removeFromLeft (w2));
        }
    }
}

//==============================================================================
ClipEditorWindow::ClipEditorWindow (AppContext& ctx, Track& t, std::shared_ptr<AudioClip> c)
    : juce::DocumentWindow ("Clip", Theme::windowBg, juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    editor = new ClipEditor (ctx, t, std::move (c));
    setContentOwned (editor, true);
    setResizable (false, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    FloatAboveMain::attach (*this);
}
