#include "Mixer.h"
#include "PluginWindows.h"
#include "Widgets.h"
#include "Plugins/Builtins.h"

namespace
{
    const juce::String pluginDragPrefix = "plugin:";
    const juce::String slotDragPrefix = "slot:";

    void styleSmallToggle (juce::TextButton& b, juce::Colour on)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    }
}

//==============================================================================
PluginBrowser::PluginBrowser (AppContext& c) : ctx (c)
{
    search.setTextToShowWhenEmpty ("Search plug-ins...", Theme::text.withAlpha (0.4f));
    search.onTextChange = [this] { refresh(); };
    addAndMakeVisible (search);

    scanBtn.setTooltip ("Scan for VST3 plug-ins");
    scanBtn.onClick = [this] { if (ctx.showPluginManager) ctx.showPluginManager(); };
    addAndMakeVisible (scanBtn);

    list.setModel (this);
    list.setRowHeight (24);
    list.setColour (juce::ListBox::backgroundColourId, Theme::lane);
    addAndMakeVisible (list);

    refresh();
}

void PluginBrowser::refresh()
{
    filtered.clear();
    const auto needle = search.getText().trim();

    for (const auto& d : ctx.engine.knownPlugins.getTypes())
    {
        if (d.isInstrument)
            continue;
        if (needle.isNotEmpty() && ! d.name.containsIgnoreCase (needle) && ! d.manufacturerName.containsIgnoreCase (needle))
            continue;
        filtered.add (d);
    }

    std::sort (filtered.begin(), filtered.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
    {
        const bool ownA = a.pluginFormatName == "AMETHYST", ownB = b.pluginFormatName == "AMETHYST";
        if (ownA != ownB)
            return ownA; // built-ins first
        return a.name.compareNatural (b.name) < 0;
    });

    list.updateContent();
    list.repaint();
}

void PluginBrowser::paint (juce::Graphics& g)
{
    Glass::panel (g, getLocalBounds().toFloat().reduced (2.0f), 14.0f);
    Glass::title (g, getLocalBounds().removeFromTop (32).reduced (12, 0), "Plug-ins");
    Glass::well (g, list.getBounds().toFloat().expanded (2.0f), 8.0f);

    if (filtered.isEmpty())
    {
        g.setColour (Theme::text.withAlpha (0.5f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText ("No effect plug-ins yet.\nClick Scan... to find your VST3s.",
                          list.getBounds().reduced (12), juce::Justification::centredTop, 4);
    }
}

void PluginBrowser::resized()
{
    auto r = getLocalBounds().reduced (12, 0);
    auto top = r.removeFromTop (34);
    scanBtn.setBounds (top.removeFromRight (64).reduced (0, 5));
    search.setBounds (r.removeFromTop (28));
    r.removeFromTop (10);
    r.removeFromBottom (12);
    list.setBounds (r);
}

void PluginBrowser::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, filtered.size()))
        return;

    if (selected)
    {
        g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.4f), 0.0f, 0.0f, Theme::accent2.withAlpha (0.15f), (float) width, 0.0f, false));
        g.fillRoundedRectangle (juce::Rectangle<float> (2.0f, 1.0f, (float) width - 4.0f, (float) height - 2.0f), 5.0f);
    }

    const auto& d = filtered.getReference (row);
    const bool own = d.pluginFormatName == "AMETHYST";
    int x = 8;
    if (own)
    {
        drawLogoMark (g, juce::Rectangle<float> (6.0f, 4.0f, (float) height - 8.0f, (float) height - 8.0f));
        x = height + 2;
    }
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (13.0f, own ? juce::Font::bold : juce::Font::plain));
    g.drawText (d.name, x, 0, width - x - 8, height, juce::Justification::centredLeft, true);

    const int needed = own ? Builtins::requiredTier (d.fileOrIdentifier) : 0;
    const bool locked = own && ctx.tier && ctx.tier() < needed;
    if (locked)
    {
        // small "PRO" / "ULTRA" pill with a padlock
        const float pillW = needed >= Builtins::tierUltra ? 52.0f : 40.0f;
        auto pill = juce::Rectangle<float> ((float) width - pillW - 6.0f, (float) height * 0.5f - 8.0f, pillW, 16.0f);
        g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.55f), pill.getX(), pill.getY(),
                                                 Theme::accent2.withAlpha (0.45f), pill.getRight(), pill.getY(), false));
        g.fillRoundedRectangle (pill, 8.0f);
        auto lock = pill.removeFromLeft (14.0f).reduced (3.0f, 3.5f).translated (2.0f, 0.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (lock.withTrimmedTop (lock.getHeight() * 0.45f), 1.0f);
        g.drawEllipse (lock.withTrimmedBottom (lock.getHeight() * 0.35f).reduced (1.0f, 0.0f), 1.0f);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (needed >= Builtins::tierUltra ? "ULTRA" : "PRO", pill.toNearestInt().withTrimmedLeft (8), juce::Justification::centred);
    }
    else
    {
        g.setColour (own ? Theme::accent : Theme::text.withAlpha (0.4f));
        g.setFont (juce::FontOptions (10.0f, own ? juce::Font::bold : juce::Font::plain));
        g.drawText (own ? d.category.toUpperCase() : d.manufacturerName, 8, 0, width - 12, height, juce::Justification::centredRight, true);
    }
}

juce::var PluginBrowser::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    if (rows.isEmpty() || ! juce::isPositiveAndBelow (rows[0], filtered.size()))
        return {};
    return pluginDragPrefix + filtered.getReference (rows[0]).createIdentifierString();
}

void PluginBrowser::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, filtered.size()) && onPluginDoubleClicked)
        onPluginDoubleClicked (filtered.getReference (row));
}

//==============================================================================
SlotRow::SlotRow (SlotPanel& o, int i) : owner (o), index (i)
{
    mixKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    mixKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    mixKnob.setRange (0.0, 1.0, 0.01);
    mixKnob.setDoubleClickReturnValue (true, 1.0);
    mixKnob.setTooltip ("Wet/dry mix (double-click for 100%)");
    mixKnob.onValueChange = [this] { if (auto* s = getSlot()) s->mix = (float) mixKnob.getValue(); };
    addAndMakeVisible (mixKnob);

    led.setTooltip ("Slot on / off");
    led.onClick = [this] { if (auto* s = getSlot()) s->bypassed = ! led.getToggleState(); };
    addAndMakeVisible (led);

    disableFocusOnChildren (*this);
}

InsertSlot* SlotRow::getSlot() const
{
    auto* t = owner.getTrack();
    if (t == nullptr || ! juce::isPositiveAndBelow (index, (int) t->inserts.size()))
        return nullptr;
    return t->inserts[(size_t) index].get();
}

void SlotRow::refresh()
{
    auto* s = getSlot();
    const bool filled = s != nullptr && s->plugin != nullptr;

    mixKnob.setEnabled (filled);
    led.setEnabled (filled);

    if (! mixKnob.isMouseButtonDown())
        mixKnob.setValue (filled ? s->mix.load() : 1.0, juce::dontSendNotification);
    led.setToggleState (filled && ! s->bypassed.load(), juce::dontSendNotification);
    repaint();
}

void SlotRow::paint (juce::Graphics& g)
{
    auto* s = getSlot();
    const bool filled = s != nullptr && s->plugin != nullptr;
    auto r = getLocalBounds().reduced (0, 1);

    const auto rf = r.toFloat().reduced (0.0f, 1.0f);
    if (filled)
    {
        g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.22f), rf.getX(), rf.getY(),
                                                 Theme::accent.withAlpha (0.06f), rf.getRight(), rf.getY(), false));
        g.fillRoundedRectangle (rf, 7.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawRoundedRectangle (rf.reduced (0.5f), 7.0f, 1.0f);
    }
    else
    {
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRoundedRectangle (rf, 7.0f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawRoundedRectangle (rf.reduced (0.5f), 7.0f, 1.0f);
    }

    if (dragOver)
    {
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillRoundedRectangle (rf, 7.0f);
        g.setColour (Theme::accent2);
        g.drawRoundedRectangle (rf.reduced (0.5f), 7.0f, 1.5f);
    }

    auto textArea = r.withTrimmedRight (52).withTrimmedLeft (6);
    juce::Path arrow;
    const float cy = (float) r.getCentreY();
    arrow.addTriangle ((float) textArea.getX(), cy - 4.0f, (float) textArea.getX(), cy + 4.0f, (float) textArea.getX() + 5.0f, cy);
    g.setColour (Theme::text.withAlpha (0.5f));
    g.fillPath (arrow);
    textArea.removeFromLeft (12);

    if (filled)
    {
        g.setColour (s->bypassed.load() ? Theme::text.withAlpha (0.4f) : Theme::text);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (s->plugin->getName(), textArea, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (Theme::text.withAlpha (0.35f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::italic));
        g.drawText ("Slot " + juce::String (index + 1), textArea, juce::Justification::centredLeft, true);
    }
}

void SlotRow::resized()
{
    auto r = getLocalBounds().reduced (2, 2);
    led.setBounds (r.removeFromRight (20));
    r.removeFromRight (4);
    mixKnob.setBounds (r.removeFromRight (26));
}

void SlotRow::mouseDown (const juce::MouseEvent& e)
{
    dragStarted = false;
    if (e.mods.isPopupMenu())
        owner.showSlotMenu (index, *this);
}

void SlotRow::mouseDrag (const juce::MouseEvent& e)
{
    auto* s = getSlot();
    if (dragStarted || s == nullptr || s->plugin == nullptr || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 6)
        return;

    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragStarted = true;
        container->startDragging (slotDragPrefix + juce::String (index), this);
    }
}

void SlotRow::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && ! dragStarted && e.mouseWasClicked())
        owner.slotClicked (index, *this);
}

bool SlotRow::isInterestedInDragSource (const SourceDetails& details)
{
    const auto desc = details.description.toString();
    if (desc.startsWith (pluginDragPrefix))
        return owner.getTrack() != nullptr;
    if (desc.startsWith (slotDragPrefix))
        return desc.fromFirstOccurrenceOf (slotDragPrefix, false, false).getIntValue() != index;
    return false;
}

void SlotRow::itemDropped (const SourceDetails& details)
{
    dragOver = false;
    owner.dropPlugin (index, details.description.toString());
    repaint();
}

bool SlotRow::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".vst3") || f.endsWithIgnoreCase (".fst"))
            return owner.getTrack() != nullptr;
    return false;
}

void SlotRow::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (auto& f : files)
    {
        if (f.endsWithIgnoreCase (".vst3"))
        {
            owner.dropVst3File (index, juce::File (f));
            break;
        }
        if (f.endsWithIgnoreCase (".fst"))
        {
            owner.loadFlPreset (juce::File (f));
            break;
        }
    }
    repaint();
}

//==============================================================================
SlotPanel::SlotPanel (AppContext& c) : ctx (c)
{
    inputLabel.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.7f));
    addAndMakeVisible (inputLabel);

    inputBox.setTextWhenNoChoicesAvailable ("No inputs enabled");
    inputBox.onChange = [this] { if (track != nullptr && ! track->isMaster) UiHelpers::applyInputSelection (inputBox, *track); };
    addAndMakeVisible (inputBox);

    for (int i = 0; i < Track::maxInserts; ++i)
        addAndMakeVisible (rows.add (new SlotRow (*this, i)));

    savePresetBtn.setTooltip ("Save this channel's fader, pan and all slot plug-ins (with their settings)");
    savePresetBtn.onClick = [this] { if (track != nullptr) UiHelpers::savePresetAs (ctx, *track); };
    addAndMakeVisible (savePresetBtn);

    loadPresetBtn.setTooltip ("Load a channel preset");
    loadPresetBtn.onClick = [this]
    {
        if (track != nullptr)
            UiHelpers::showPresetMenu (ctx, *track, loadPresetBtn, [this] { refresh(); if (onChannelChanged) onChannelChanged(); });
    };
    addAndMakeVisible (loadPresetBtn);

    hint.setText ("Drag plug-ins from the list (or .vst3 files) onto a slot. "
                  "Drag a slot onto another to swap. Right-click a slot for options.",
                  juce::dontSendNotification);
    hint.setFont (juce::FontOptions (11.5f));
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.5f));
    addAndMakeVisible (hint);

    disableFocusOnChildren (*this);
}

void SlotPanel::setTrack (Track* t)
{
    track = t;
    const bool showInput = track != nullptr && ! track->isMaster;
    inputLabel.setVisible (showInput);
    inputBox.setVisible (showInput);
    if (showInput)
        UiHelpers::fillInputBox (inputBox, ctx.engine, *track);
    resized();
    refresh();
}

void SlotPanel::refresh()
{
    for (auto* r : rows)
        r->refresh();
    repaint (getLocalBounds().removeFromTop (34));
}

void SlotPanel::paint (juce::Graphics& g)
{
    Glass::panel (g, getLocalBounds().toFloat().reduced (2.0f), 14.0f);
    Glass::title (g, getLocalBounds().removeFromTop (36).reduced (12, 0),
                  track != nullptr ? track->name + "  -  inserts" : juce::String ("Inserts"));
}

void SlotPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 0);
    r.removeFromTop (38);

    if (inputBox.isVisible())
    {
        auto line = r.removeFromTop (26);
        inputLabel.setBounds (line.removeFromLeft (46));
        inputBox.setBounds (line);
        r.removeFromTop (8);
    }

    for (auto* row : rows)
        row->setBounds (r.removeFromTop (31));

    r.removeFromTop (12);
    auto presetLine = r.removeFromTop (28);
    savePresetBtn.setBounds (presetLine.removeFromLeft (presetLine.getWidth() / 2 - 3));
    presetLine.removeFromLeft (6);
    loadPresetBtn.setBounds (presetLine);

    r.removeFromTop (10);
    hint.setBounds (r.removeFromTop (60));
}

void SlotPanel::slotClicked (int index, juce::Component& target)
{
    if (track == nullptr)
        return;

    if (auto* p = track->inserts[(size_t) index]->plugin.get())
    {
        ctx.pluginWindows.show (*p);
        return;
    }

    auto* t = track;
    UiHelpers::choosePlugin (ctx, target, [this, t, index] (const juce::PluginDescription& d)
    {
        if (ctx.engine.isValidChannel (t))
            UiHelpers::loadPluginIntoSlot (ctx, *t, index, d, true);
        refresh();
    });
}

void SlotPanel::showSlotMenu (int index, juce::Component& target)
{
    if (track == nullptr)
        return;

    auto* t = track;
    auto& slot = *t->inserts[(size_t) index];
    const bool filled = slot.plugin != nullptr;

    juce::PopupMenu m;
    if (filled)
    {
        m.addSectionHeader (slot.plugin->getName());
        m.addItem (1, "Open editor");
        m.addItem (2, "Replace...");
        m.addItem (3, "Enabled", true, ! slot.bypassed.load());
        m.addSeparator();
        m.addItem (4, "Move up", index > 0);
        m.addItem (5, "Move down", index < Track::maxInserts - 1);
        m.addSeparator();
        m.addItem (6, "Remove");
    }
    else
    {
        m.addItem (2, "Add plug-in...");
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                     [this, t, index, &target] (int result)
    {
        if (result == 0 || ! ctx.engine.isValidChannel (t))
            return;

        auto& s = *t->inserts[(size_t) index];
        switch (result)
        {
            case 1: if (s.plugin != nullptr) ctx.pluginWindows.show (*s.plugin); break;
            case 2:
                UiHelpers::choosePlugin (ctx, target, [this, t, index] (const juce::PluginDescription& d)
                {
                    if (ctx.engine.isValidChannel (t))
                        UiHelpers::loadPluginIntoSlot (ctx, *t, index, d, true);
                    refresh();
                });
                break;
            case 3: s.bypassed = ! s.bypassed.load(); break;
            case 4: ctx.engine.moveInsert (*t, index, index - 1); break;
            case 5: ctx.engine.moveInsert (*t, index, index + 1); break;
            case 6:
            {
                if (s.plugin != nullptr)
                    ctx.pluginWindows.close (*s.plugin);
                ctx.engine.removeInsert (*t, index).reset();
                break;
            }
            default: break;
        }
        refresh();
    });
}

void SlotPanel::dropPlugin (int index, const juce::String& description)
{
    if (track == nullptr)
        return;

    if (description.startsWith (slotDragPrefix))
    {
        const int from = description.fromFirstOccurrenceOf (slotDragPrefix, false, false).getIntValue();
        ctx.engine.moveInsert (*track, from, index);
    }
    else if (description.startsWith (pluginDragPrefix))
    {
        const auto id = description.fromFirstOccurrenceOf (pluginDragPrefix, false, false);
        if (auto desc = ctx.engine.knownPlugins.getTypeForIdentifierString (id))
            UiHelpers::loadPluginIntoSlot (ctx, *track, index, *desc, true);
    }

    refresh();
}

void SlotPanel::loadFlPreset (const juce::File& file)
{
    if (track == nullptr || ! ctx.engine.isValidChannel (track))
        return;
    UiHelpers::loadPreset (ctx, *track, file, [this] { refresh(); if (onChannelChanged) onChannelChanged(); });
}

void SlotPanel::dropVst3File (int index, const juce::File& file)
{
    if (track == nullptr)
        return;

    juce::PluginDescription desc;
    if (UiHelpers::describeVst3File (ctx, file, desc))
        UiHelpers::loadPluginIntoSlot (ctx, *track, index, desc, true);
    else
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Not a VST3 plug-in",
                                                "Couldn't read a plug-in from:\n" + file.getFullPathName());
    refresh();
}

void SlotPanel::loadFirstFree (const juce::PluginDescription& desc)
{
    if (track == nullptr)
        return;

    const int slot = ctx.engine.firstFreeSlot (*track);
    if (slot < 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "All slots full",
                                                "Remove a plug-in or drop onto a slot to replace it.");
        return;
    }

    UiHelpers::loadPluginIntoSlot (ctx, *track, slot, desc, true);
    refresh();
}

//==============================================================================
MixerStrip::MixerStrip (MixerView& o, Track& t, int num) : owner (o), track (t), number (num)
{
    armLed.setTooltip ("Arm for recording");
    armLed.onClick = [this] { track.armed = armLed.getToggleState(); };
    armLed.setVisible (! track.isMaster);
    addChildComponent (armLed);

    gainKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    gainKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    gainKnob.setRange (-12.0, 24.0, 0.1);
    gainKnob.setDoubleClickReturnValue (true, 0.0);
    gainKnob.setTextValueSuffix (" dB");
    gainKnob.setTooltip ("Input gain: makes your mic louder when recording and monitoring (double-click for 0 dB)");
    gainKnob.onValueChange = [this] { track.inputGainDb = (float) gainKnob.getValue(); };
    if (! track.isMaster)
        addAndMakeVisible (gainKnob);

    panKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    panKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    panKnob.setRange (-1.0, 1.0, 0.01);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setTooltip ("Pan (double-click to centre)");
    panKnob.onValueChange = [this] { track.pan = (float) panKnob.getValue(); };
    addAndMakeVisible (panKnob);

    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader.setRange (-60.0, 24.0, 0.1);
    fader.setSkewFactorFromMidPoint (-6.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTooltip ("Volume (double-click for 0 dB)");
    fader.onValueChange = [this] { track.volumeDb = (float) fader.getValue(); };
    addAndMakeVisible (fader);

    styleSmallToggle (muteBtn, Theme::mute);
    styleSmallToggle (soloBtn, Theme::solo);
    muteBtn.onClick = [this] { track.mute = muteBtn.getToggleState(); };
    soloBtn.onClick = [this] { track.solo = soloBtn.getToggleState(); };
    addAndMakeVisible (muteBtn);
    if (! track.isMaster)
        addAndMakeVisible (soloBtn);

    disableFocusOnChildren (*this);
    tick();
}

void MixerStrip::setSelected (bool s)
{
    if (selected != s)
    {
        selected = s;
        repaint();
    }
}

void MixerStrip::tick()
{
    levelL = juce::jmax (track.meterL.exchange (0.0f), levelL * 0.85f);
    levelR = juce::jmax (track.meterR.exchange (0.0f), levelR * 0.85f);
    // peak hold: the highest level stays marked for 1.5 s, then falls
    const auto nowMs = juce::Time::getMillisecondCounter();
    for (auto [lvl, hold, at] : { std::tuple<float, float*, juce::uint32*> { levelL, &holdL, &holdAtL }, std::tuple<float, float*, juce::uint32*> { levelR, &holdR, &holdAtR } })
    {
        if (lvl >= *hold) { *hold = lvl; *at = nowMs; }
        else if (nowMs - *at > 1500) *hold = juce::jmax (lvl, *hold * 0.92f);
    }

    if (! fader.isMouseButtonDown())   fader.setValue (track.volumeDb.load(), juce::dontSendNotification);
    if (! panKnob.isMouseButtonDown()) panKnob.setValue (track.pan.load(), juce::dontSendNotification);
    if (! gainKnob.isMouseButtonDown()) gainKnob.setValue (track.inputGainDb.load(), juce::dontSendNotification);
    muteBtn.setToggleState (track.mute.load(), juce::dontSendNotification);
    soloBtn.setToggleState (track.solo.load(), juce::dontSendNotification);
    armLed.setToggleState (track.armed.load(), juce::dontSendNotification);
    repaint();
}

void MixerStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    const auto card = r.toFloat().reduced (3.0f, 2.0f);
    Glass::panel (g, card, 12.0f, selected ? 1.6f : 1.0f);
    if (selected)
    {
        g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.18f), card.getX(), card.getY(),
                                                 Theme::accent2.withAlpha (0.04f), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, 12.0f);
    }

    // track colour along the top of the card
    if (! track.colour.isTransparent())
    {
        g.setColour (track.colour.withAlpha (0.9f));
        g.fillRoundedRectangle (card.withHeight (4.0f).reduced (10.0f, 0.0f).translated (0.0f, 2.0f), 2.0f);
    }

    // number, with a red clip light (click it to clear)
    auto numberRow = r.removeFromTop (24);
    g.setColour (track.isMaster ? Theme::accent2 : Theme::text.withAlpha (0.75f));
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (track.isMaster ? juce::String ("M") : juce::String (number), numberRow, juce::Justification::centred);
    clipLightArea = numberRow.removeFromRight (18).withSizeKeepingCentre (10, 10);
    g.setColour (track.clipped.load() ? Theme::record : Theme::panelLight);
    g.fillEllipse (clipLightArea.toFloat());

    // small badges: low cut, polarity, plug-in latency
    {
        juce::StringArray badges;
        if (track.lowCutHz.load() > 1.0f) badges.add ("LC" + juce::String ((int) track.lowCutHz.load()));
        if (track.phaseInvert.load()) badges.add (juce::String (juce::CharPointer_UTF8 ("Ã")));
        if (track.latencySamples.load() > 0) badges.add (juce::String (track.latencySamples.load() / 48) + "ms");
        if (! badges.isEmpty())
        {
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.setColour (Theme::accent.brighter (0.3f));
            g.drawText (badges.joinIntoString (" "), nameArea.withHeight (14).translated (0, 3), juce::Justification::centred, true);
        }
    }

    // vertical name
    Glass::well (g, nameArea.toFloat(), 7.0f);
    {
        juce::Graphics::ScopedSaveState s (g);
        const auto centre = nameArea.getCentre().toFloat();
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, centre.x, centre.y));
        auto rotated = juce::Rectangle<int> (0, 0, nameArea.getHeight() - 8, nameArea.getWidth()).withCentre (nameArea.getCentre());
        g.setColour (selected ? juce::Colours::white : Theme::text);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (track.name, rotated, juce::Justification::centredLeft, true);
    }

    // meters
    auto m = meterArea.toFloat();
    auto mL = m.removeFromLeft (m.getWidth() / 2.0f - 1.0f);
    drawMeter (g, mL, levelL);
    m.removeFromLeft (2.0f);
    drawMeter (g, m, levelR);
    // peak-hold ticks (same dB scale as the meter: -60..+6)
    auto holdY = [] (juce::Rectangle<float> area, float lvl)
    {
        const float db = juce::jlimit (-60.0f, 6.0f, juce::Decibels::gainToDecibels (lvl, -60.0f));
        return area.getBottom() - (db + 60.0f) / 66.0f * area.getHeight();
    };
    for (auto [area, hold] : { std::pair<juce::Rectangle<float>, float> { mL, holdL }, std::pair<juce::Rectangle<float>, float> { m, holdR } })
        if (hold > 0.001f)
        {
            g.setColour (hold > 1.0f ? Theme::record : Theme::text.withAlpha (0.85f));
            g.fillRect (area.getX(), holdY (area, hold) - 1.0f, area.getWidth(), 2.0f);
        }

    // dB readout
    const float db = track.volumeDb.load();
    g.setColour (Theme::text.withAlpha (0.75f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (db <= -60.0f ? juce::String ("-inf") : juce::String (db, 1),
                getLocalBounds().removeFromBottom (20), juce::Justification::centred);

    if (selected)
    {
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.drawRoundedRectangle (card.expanded (1.0f), 13.0f, 3.0f);
        g.setColour (Theme::accent);
        g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, 1.2f);
    }
}

void MixerStrip::resized()
{
    auto r = getLocalBounds().reduced (6, 0);
    r.removeFromTop (22);
    nameArea = r.removeFromTop (juce::jlimit (60, 150, getHeight() / 4));
    r.removeFromTop (4);
    {
        auto row = r.removeFromTop (34);
        if (track.isMaster)
            armLed.setBounds (row.withSizeKeepingCentre (18, 18));
        else
        {
            armLed.setBounds (row.removeFromLeft (row.getWidth() / 2).withSizeKeepingCentre (18, 18));
            gainKnob.setBounds (row.withSizeKeepingCentre (32, 32));
        }
    }
    panKnob.setBounds (r.removeFromTop (40).withSizeKeepingCentre (38, 38));
    r.removeFromTop (4);

    auto buttons = r.removeFromTop (22);
    if (track.isMaster)
        muteBtn.setBounds (buttons.withSizeKeepingCentre (28, 22));
    else
    {
        muteBtn.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 1));
        buttons.removeFromLeft (2);
        soloBtn.setBounds (buttons);
    }

    r.removeFromTop (8);
    r.removeFromBottom (22);
    meterArea = r.removeFromRight (14);
    r.removeFromRight (4);
    fader.setBounds (r);
}

void MixerStrip::mouseDown (const juce::MouseEvent& e)
{
    if (clipLightArea.expanded (4).contains (e.getPosition()))
    {
        track.clipped = false;
        holdL = holdR = 0.0f;
        repaint();
    }
    owner.select (&track);
}

//==============================================================================
MixerView::MixerView (AppContext& c) : ctx (c), browser (c), slotPanel (c)
{
    browser.onPluginDoubleClicked = [this] (const juce::PluginDescription& d) { slotPanel.loadFirstFree (d); };
    addAndMakeVisible (browser);

    stripViewport.setViewedComponent (&stripHolder, false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);

    addBtn.setTooltip ("Add a track");
    addBtn.onClick = [this] { if (onAddTrack) onAddTrack(); };
    stripHolder.addAndMakeVisible (addBtn);

    addAndMakeVisible (slotPanel);
    disableFocusOnChildren (*this);
}

void MixerView::rebuild()
{
    auto* previous = slotPanel.getTrack();
    strips.clear();

    stripHolder.addAndMakeVisible (strips.add (new MixerStrip (*this, ctx.engine.getMaster(), 0)));
    int n = 1;
    for (auto* t : ctx.engine.getTracks())
    {
        ++n; // keep track numbers matching the track list
        if (t->playThrough == nullptr && ! t->isAutomation()) // take lanes and automation clips have no channel of their own
            stripHolder.addAndMakeVisible (strips.add (new MixerStrip (*this, *t, n - 1)));
    }

    resized();
    select (previous != nullptr && ctx.engine.isValidChannel (previous) ? previous : &ctx.engine.getMaster());
}

void MixerView::select (Track* t)
{
    if (t == nullptr || ! ctx.engine.isValidChannel (t))
        t = &ctx.engine.getMaster();

    for (auto* s : strips)
        s->setSelected (&s->getTrack() == t);

    if (slotPanel.getTrack() != t)
        slotPanel.setTrack (t);
}

void MixerView::tick()
{
    for (auto* s : strips)
        s->tick();
    slotPanel.refresh();
}

void MixerView::paint (juce::Graphics&) {}

void MixerView::resized()
{
    auto r = getLocalBounds();
    browser.setBounds (r.removeFromLeft (kBrowserWidth));
    r.removeFromLeft (8);
    slotPanel.setBounds (r.removeFromRight (kSlotPanelWidth));
    r.removeFromRight (8);
    stripViewport.setBounds (r);

    const int h = juce::jmax (300, stripViewport.getHeight() - (stripViewport.getHeight() < 300 ? 0 : 10));
    int x = 0;
    for (int i = 0; i < strips.size(); ++i)
    {
        strips[i]->setBounds (x, 0, kMixerStripWidth, h);
        x += kMixerStripWidth + (i == 0 ? 8 : 0); // gap after master
    }
    addBtn.setBounds (x + 8, 30, 32, 32);
    stripHolder.setSize (x + 48, h);
}
