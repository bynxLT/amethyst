#include "AmethystPlugin.h"
#include "../UiHelpers.h"

//==============================================================================
namespace AmParams
{
    std::unique_ptr<juce::AudioParameterFloat> num (const juce::String& id, const juce::String& name, float min, float max, float def,
                                                    const juce::String& unit, int decimals, float skewCentre, float interval)
    {
        juce::NormalisableRange<float> range (min, max, interval);
        if (skewCentre != 0.0f && skewCentre > min && skewCentre < max)
            range.setSkewForCentre (skewCentre);

        auto attrs = juce::AudioParameterFloatAttributes()
                         .withLabel (unit)
                         .withStringFromValueFunction ([decimals] (float v, int) { return juce::String (v, decimals); });

        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, def, attrs);
    }

    std::unique_ptr<juce::AudioParameterChoice> choice (const juce::String& id, const juce::String& name, const juce::StringArray& options, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, options, def);
    }

    std::unique_ptr<juce::AudioParameterBool> toggle (const juce::String& id, const juce::String& name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }
}

//==============================================================================
AmethystPlugin::AmethystPlugin (const juce::String& id, const juce::String& displayName, const juce::String& categoryName,
                                const juce::String& tag, Layout layout)
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      pluginId (id), name (displayName), category (categoryName), tagline (tag),
      apvts (*this, &undoManager, "AMETHYST_STATE", std::move (layout))
{
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            params[rp->getParameterID().toStdString()] = apvts.getRawParameterValue (rp->getParameterID());
}

void AmethystPlugin::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = name;
    d.descriptiveName = name + " - " + tagline;
    d.pluginFormatName = "AMETHYST";
    d.category = category;
    d.manufacturerName = "AMETHYST";
    d.version = "1.0";
    d.fileOrIdentifier = pluginId;
    d.uniqueId = pluginId.hashCode();
    d.deprecatedUid = d.uniqueId;
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
    d.hasSharedContainer = false;
}

bool AmethystPlugin::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono());
}

double AmethystPlugin::getHostBpm() const
{
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                return juce::jmax (20.0, *bpm);
    return 120.0;
}

void AmethystPlugin::setCurrentProgram (int index)
{
    if (juce::isPositiveAndBelow (index, (int) presets.size()))
    {
        currentPreset = index;
        applyPreset (presets[(size_t) index]);
    }
}

const juce::String AmethystPlugin::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : juce::String ("Default");
}

void AmethystPlugin::setParam (const juce::String& id, float realValue)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (realValue));
        p->endChangeGesture();
    }
}

void AmethystPlugin::applyPreset (const Preset& preset)
{
    for (auto* p : getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr)
            continue;

        float normalised = rp->getDefaultValue();
        for (auto& [id, value] : preset.values)
            if (id == rp->getParameterID())
                normalised = rp->convertTo0to1 (value);

        rp->beginChangeGesture();
        rp->setValueNotifyingHost (normalised);
        rp->endChangeGesture();
    }
    currentPresetName = preset.name;
}

void AmethystPlugin::randomize()
{
    juce::Random rnd;
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            // keep things musical: stay within the middle 70% of each range, leave on/off switches alone
            if (dynamic_cast<juce::AudioParameterBool*> (rp) != nullptr)
                continue;
            const float v = dynamic_cast<juce::AudioParameterChoice*> (rp) != nullptr ? rnd.nextFloat() : 0.15f + 0.7f * rnd.nextFloat();
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (v);
            rp->endChangeGesture();
        }
    currentPresetName = "Randomized";
}

void AmethystPlugin::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("presetName", currentPresetName, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void AmethystPlugin::selectAB (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == abSlot)
        return;
    getStateInformation (abStates[(size_t) abSlot]);
    if (abStates[(size_t) slot].isEmpty())
        abStates[(size_t) slot] = abStates[(size_t) abSlot];   // first visit: start from the same settings
    abSlot = slot;
    setStateInformation (abStates[(size_t) slot].getData(), (int) abStates[(size_t) slot].getSize());
}

void AmethystPlugin::copyCurrentToOtherAB()
{
    getStateInformation (abStates[(size_t) (1 - abSlot)]);
}

void AmethystPlugin::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentPresetName = tree.getProperty ("presetName", "Default").toString();
            apvts.replaceState (tree);
        }
}

juce::File AmethystPlugin::getUserPresetFolder() const
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("AMETHYST").getChildFile ("Plugin Presets").getChildFile (name);
}

bool AmethystPlugin::saveUserPreset (const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
    {
        if (xml->writeTo (file))
        {
            currentPresetName = file.getFileNameWithoutExtension();
            return true;
        }
    }
    return false;
}

bool AmethystPlugin::loadUserPreset (const juce::File& file)
{
    if (auto xml = juce::XmlDocument::parse (file))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            currentPresetName = file.getFileNameWithoutExtension();
            return true;
        }
    return false;
}

//==============================================================================
namespace
{
    class SegmentedChoice : public juce::Component
    {
    public:
        explicit SegmentedChoice (juce::AudioParameterChoice& p) : param (p)
        {
            for (int i = 0; i < param.choices.size(); ++i)
            {
                auto* b = buttons.add (new juce::TextButton (param.choices[i]));
                b->onClick = [this, i]
                {
                    param.beginChangeGesture();
                    param.setValueNotifyingHost (param.convertTo0to1 ((float) i));
                    param.endChangeGesture();
                    refresh();
                };
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }
            refresh();
        }

        void refresh()
        {
            const int idx = param.getIndex();
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setToggleState (i == idx, juce::dontSendNotification);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            const int w = (r.getWidth() - 3 * (buttons.size() - 1)) / juce::jmax (1, buttons.size());
            for (auto* b : buttons)
            {
                b->setBounds (r.removeFromLeft (w));
                r.removeFromLeft (3);
            }
        }

        static int preferredWidth (const juce::AudioParameterChoice& p)
        {
            int w = 0;
            for (auto& c : p.choices)
                w += juce::jmax (34, 18 + 8 * c.length()) + 3;
            return w;
        }

    private:
        juce::AudioParameterChoice& param;
        juce::OwnedArray<juce::TextButton> buttons;
    };

    //==========================================================================
    class AmethystEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit AmethystEditor (AmethystPlugin& p) : juce::AudioProcessorEditor (p), plugin (p)
        {
            aBtn.onClick = [this] { plugin.selectAB (0); refreshAB(); };
            bBtn.onClick = [this] { plugin.selectAB (1); refreshAB(); };
            copyBtn.onClick = [this] { plugin.copyCurrentToOtherAB(); };
            aBtn.setTooltip ("Settings A");
            bBtn.setTooltip ("Settings B - switch to compare two settings");
            copyBtn.setTooltip ("Copy these settings to the other slot");
            for (auto* b : { &aBtn, &bBtn })
            {
                b->setClickingTogglesState (false);
                b->setColour (juce::TextButton::buttonOnColourId, Theme::accent.withAlpha (0.55f));
            }
            for (auto* b : { &aBtn, &bBtn, &copyBtn })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }
            refreshAB();
            prevBtn.onClick = [this] { step (-1); };
            nextBtn.onClick = [this] { step (1); };
            presetBtn.onClick = [this] { showPresetMenu(); };
            prevBtn.setTooltip ("Previous preset");
            nextBtn.setTooltip ("Next preset");
            presetBtn.setTooltip ("Presets");
            for (auto* b : { &prevBtn, &nextBtn, &presetBtn })
            {
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }

            visual = plugin.createVisual();
            if (visual != nullptr)
                addAndMakeVisible (*visual);

            buildSections();
            computeSize();
            startTimerHz (30);
            timerCallback();
        }

        void paint (juce::Graphics& g) override
        {
            if (bgCache.isNull() || bgCache.getWidth() != getWidth() || bgCache.getHeight() != getHeight())
            {
                bgCache = juce::Image (juce::Image::RGB, juce::jmax (1, getWidth()), juce::jmax (1, getHeight()), false);
                juce::Graphics bg (bgCache);
                Glass::background (bg, getLocalBounds());

                auto header = getLocalBounds().removeFromTop (kHeader).reduced (8, 6).toFloat();
                Glass::panel (bg, header, 12.0f, 1.1f);
                drawLogoMark (bg, header.withWidth (36.0f).reduced (4.0f).translated (6.0f, 0.0f));
                bg.setColour (Theme::text);
                bg.setFont (juce::FontOptions (17.0f, juce::Font::bold));
                bg.drawText (plugin.name, header.withTrimmedLeft (50.0f).withHeight (header.getHeight() * 0.6f).toNearestInt(),
                             juce::Justification::bottomLeft, true);
                bg.setColour (Theme::accent);
                bg.setFont (juce::FontOptions (10.5f, juce::Font::bold));
                bg.drawText (plugin.tagline.toUpperCase(), header.withTrimmedLeft (50.0f).withTrimmedTop (header.getHeight() * 0.6f).toNearestInt(),
                             juce::Justification::topLeft, true);

                for (auto& s : sectionUIs)
                {
                    Glass::panel (bg, s.area.toFloat(), 12.0f);
                    Glass::title (bg, s.area.withHeight (28).reduced (10, 0), s.title);
                }
            }
            g.drawImageAt (bgCache, 0, 0);

            // IN / OUT level meters
            auto m = meterArea.toFloat();
            const float barW = (m.getWidth() - 4.0f) / 2.0f;
            int idx = 0;
            for (float level : { inShown, outShown })
            {
                auto bar = m.removeFromLeft (barW);
                m.removeFromLeft (4.0f);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.fillRoundedRectangle (bar, 2.0f);
                const float db = juce::Decibels::gainToDecibels (level, -60.0f);
                const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
                auto lit = bar.withTrimmedTop (bar.getHeight() * (1.0f - frac));
                g.setGradientFill (juce::ColourGradient (level > 0.98f ? Theme::record : Theme::accent2, lit.getX(), bar.getY(),
                                                         Theme::accent, lit.getX(), bar.getBottom(), false));
                g.fillRoundedRectangle (lit, 2.0f);
                juce::ignoreUnused (idx++);
            }
        }

        void lookAndFeelChanged() override { bgCache = {}; repaint(); }

        void resized() override
        {
            bgCache = {};
            auto header = getLocalBounds().removeFromTop (kHeader).reduced (16, 13);
            nextBtn.setBounds (header.removeFromRight (28));
            header.removeFromRight (4);
            presetBtn.setBounds (header.removeFromRight (190));
            header.removeFromRight (4);
            prevBtn.setBounds (header.removeFromRight (28));
            header.removeFromRight (10);
            copyBtn.setBounds (header.removeFromRight (40));
            header.removeFromRight (2);
            bBtn.setBounds (header.removeFromRight (26));
            aBtn.setBounds (header.removeFromRight (26));
            header.removeFromRight (10);
            meterArea = header.removeFromRight (26).reduced (0, 2);

            auto r = getLocalBounds().withTrimmedTop (kHeader).reduced (kMargin, 0);
            if (visual != nullptr)
            {
                visual->setBounds (r.removeFromTop (plugin.getVisualHeight()));
                r.removeFromTop (kGap);
            }

            for (auto& row : rows)
            {
                auto rowArea = r.removeFromTop (row.height);
                r.removeFromTop (kGap);

                const int extra = juce::jmax (0, rowArea.getWidth() - row.width) / juce::jmax (1, (int) row.sections.size());
                for (auto idx : row.sections)
                {
                    auto& s = sectionUIs[(size_t) idx];
                    s.area = rowArea.removeFromLeft (s.width + extra).withHeight (row.height);
                    rowArea.removeFromLeft (kGap);
                    layoutSection (s);
                }
            }
        }

    private:
        static constexpr int kHeader = 58, kMargin = 12, kGap = 10, kTopLine = 30;

        struct SectionUI
        {
            juce::String title;
            std::vector<juce::Component*> top, knobs;
            int knobSize = 58, width = 0, height = 0, row = 0;
            juce::Rectangle<int> area;
        };

        struct Row { std::vector<int> sections; int width = 0, height = 0; };

        void buildSections()
        {
            for (auto& s : plugin.sections)
            {
                SectionUI ui;
                ui.title = s.title;
                ui.knobSize = s.knobSize;
                ui.row = s.row;
                int topWidth = 0;

                for (auto& id : s.params)
                {
                    auto* p = plugin.apvts.getParameter (id);
                    if (p == nullptr)
                        continue;

                    if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
                    {
                        if (c->choices.size() <= 4)
                        {
                            auto* seg = segments.add (new SegmentedChoice (*c));
                            seg->setSize (SegmentedChoice::preferredWidth (*c), 26);
                            addAndMakeVisible (seg);
                            ui.top.push_back (seg);
                            topWidth += seg->getWidth() + 6;
                        }
                        else
                        {
                            auto* box = combos.add (new juce::ComboBox());
                            box->addItemList (c->choices, 1);
                            box->setSize (92, 26);
                            box->setWantsKeyboardFocus (false);
                            addAndMakeVisible (box);
                            comboAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (plugin.apvts, id, *box));
                            ui.top.push_back (box);
                            topWidth += box->getWidth() + 6;
                        }
                    }
                    else if (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr)
                    {
                        auto* tb = toggles.add (new juce::ToggleButton (p->getName (32)));
                        tb->setSize (juce::jmax (44, 30 + 7 * p->getName (32).length()), 26);
                        tb->setWantsKeyboardFocus (false);
                        addAndMakeVisible (tb);
                        buttonAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (plugin.apvts, id, *tb));
                        ui.top.push_back (tb);
                        topWidth += tb->getWidth() + 6;
                    }
                    else if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                    {
                        auto* k = knobs.add (new LabeledKnob (p->getName (32), 0.0, 1.0, 0.0));
                        k->slider.setWantsKeyboardFocus (false);
                        addAndMakeVisible (k);
                        sliderAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (plugin.apvts, id, k->slider));
                        k->slider.setDoubleClickReturnValue (true, rp->convertFrom0to1 (rp->getDefaultValue()));
                        k->slider.automationProcessor = &plugin;
                        k->slider.setName (p->getName (32));
                        k->slider.automationParamIndex = rp->getParameterIndex();
                        k->slider.textFromValueFunction = [rp] (double v)
                        {
                            const auto label = rp->getLabel();
                            return rp->getText (rp->convertTo0to1 ((float) v), 0) + (label.isNotEmpty() ? " " + label : juce::String());
                        };
                        k->slider.updateText();
                        ui.knobs.push_back (k);
                    }
                }

                const int knobW = ui.knobSize + 8;
                ui.width = juce::jmax ((int) ui.knobs.size() * knobW, topWidth) + 20;
                ui.width = juce::jmax (ui.width, 12 + 8 * ui.title.length() + 20);
                ui.height = 30 + (ui.top.empty() ? 0 : kTopLine + 4) + (ui.knobs.empty() ? 0 : ui.knobSize + 18) + 8;
                sectionUIs.push_back (ui);
            }

            int maxRow = 0;
            for (auto& s : sectionUIs)
                maxRow = juce::jmax (maxRow, s.row);
            rows.resize ((size_t) maxRow + 1);
            for (int i = 0; i < (int) sectionUIs.size(); ++i)
            {
                auto& row = rows[(size_t) sectionUIs[(size_t) i].row];
                row.sections.push_back (i);
                row.width += sectionUIs[(size_t) i].width + (row.sections.size() > 1 ? kGap : 0);
                row.height = juce::jmax (row.height, sectionUIs[(size_t) i].height);
            }
        }

        void computeSize()
        {
            int w = juce::jmax (560, plugin.getMinimumEditorWidth());
            int h = kHeader + (visual != nullptr ? plugin.getVisualHeight() + kGap : 0);
            for (auto& row : rows)
            {
                w = juce::jmax (w, row.width);
                h += row.height + kGap;
            }
            setSize (w + kMargin * 2, h + 4);
        }

        void layoutSection (SectionUI& s)
        {
            auto r = s.area.reduced (10, 0).withTrimmedTop (30);
            if (! s.top.empty())
            {
                auto line = r.removeFromTop (kTopLine);
                int total = 0;
                for (auto* c : s.top) total += c->getWidth() + 6;
                line.removeFromLeft (juce::jmax (0, (line.getWidth() - total + 6) / 2));
                for (auto* c : s.top)
                {
                    c->setBounds (line.removeFromLeft (c->getWidth()).withSizeKeepingCentre (c->getWidth(), 26));
                    line.removeFromLeft (6);
                }
                r.removeFromTop (4);
            }

            if (! s.knobs.empty())
            {
                auto line = r.removeFromTop (s.knobSize + 16);
                const int w = line.getWidth() / (int) s.knobs.size();
                for (auto* k : s.knobs)
                    k->setBounds (line.removeFromLeft (w).withSizeKeepingCentre (juce::jmin (w, s.knobSize + 8), s.knobSize + 16));
            }
        }

        void timerCallback() override
        {
            for (auto* seg : segments)
                seg->refresh();
            inShown = juce::jmax (plugin.inLevel.exchange (0.0f), inShown * 0.8f);
            outShown = juce::jmax (plugin.outLevel.exchange (0.0f), outShown * 0.8f);
            repaint (meterArea.expanded (2));
            const auto text = plugin.currentPresetName;
            if (presetBtn.getButtonText() != text)
                presetBtn.setButtonText (text);
        }

        void step (int delta)
        {
            const int n = (int) plugin.presets.size();
            if (n == 0)
                return;
            plugin.setCurrentProgram ((plugin.getCurrentProgram() + delta + n) % n);
            timerCallback();
        }

        void showPresetMenu()
        {
            juce::PopupMenu m;
            m.addSectionHeader ("Factory");
            for (int i = 0; i < (int) plugin.presets.size(); ++i)
                m.addItem (1 + i, plugin.presets[(size_t) i].name, true, plugin.presets[(size_t) i].name == plugin.currentPresetName);

            const auto userFiles = plugin.getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.ampreset");
            if (! userFiles.isEmpty())
            {
                m.addSectionHeader ("Your presets");
                for (int i = 0; i < userFiles.size(); ++i)
                    m.addItem (1000 + i, userFiles[i].getFileNameWithoutExtension(), true,
                               userFiles[i].getFileNameWithoutExtension() == plugin.currentPresetName);
            }

            m.addSeparator();
            m.addItem (900, "Save preset...");
            m.addItem (901, "Randomize");
            m.addItem (902, "Reset to default");
            m.addItem (903, "Open presets folder");

            juce::Component::SafePointer<AmethystEditor> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetBtn), [safe, userFiles] (int r)
            {
                if (safe == nullptr || r == 0)
                    return;
                auto& p = safe->plugin;

                if (r >= 1 && r <= (int) p.presets.size())       p.setCurrentProgram (r - 1);
                else if (r >= 1000 && r - 1000 < userFiles.size()) p.loadUserPreset (userFiles[r - 1000]);
                else if (r == 901)                                p.randomize();
                else if (r == 902)                                p.applyPreset ({ "Default", {} });
                else if (r == 903)                                { p.getUserPresetFolder().createDirectory(); p.getUserPresetFolder().startAsProcess(); }
                else if (r == 900)
                {
                    p.getUserPresetFolder().createDirectory();
                    safe->chooser = std::make_unique<juce::FileChooser> ("Save " + p.name + " preset",
                                                                         p.getUserPresetFolder().getChildFile ("My preset.ampreset"), "*.ampreset");
                    safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                                [safe] (const juce::FileChooser& fc)
                    {
                        if (safe != nullptr && fc.getResult() != juce::File())
                            safe->plugin.saveUserPreset (fc.getResult().withFileExtension (".ampreset"));
                    });
                }
                safe->timerCallback();
            });
        }

        AmethystPlugin& plugin;
        juce::TextButton prevBtn { "<" }, nextBtn { ">" }, presetBtn { "Default" };
        juce::TextButton aBtn { "A" }, bBtn { "B" }, copyBtn { "Copy" };
        juce::Rectangle<int> meterArea;
        float inShown = 0.0f, outShown = 0.0f;
        void refreshAB()
        {
            aBtn.setToggleState (plugin.getABSlot() == 0, juce::dontSendNotification);
            bBtn.setToggleState (plugin.getABSlot() == 1, juce::dontSendNotification);
            copyBtn.setButtonText (plugin.getABSlot() == 0 ? "A>B" : "B>A");
        }
        std::unique_ptr<juce::Component> visual;

        juce::OwnedArray<LabeledKnob> knobs;
        juce::OwnedArray<juce::ToggleButton> toggles;
        juce::OwnedArray<juce::ComboBox> combos;
        juce::OwnedArray<SegmentedChoice> segments;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAttachments;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments;
        std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAttachments;

        std::vector<SectionUI> sectionUIs;
        std::vector<Row> rows;
        juce::Image bgCache;
        std::unique_ptr<juce::FileChooser> chooser;
    };
}

juce::AudioProcessorEditor* AmethystPlugin::createEditor()
{
    return new AmethystEditor (*this);
}

//==============================================================================
MeterHistory::MeterHistory (std::atomic<float>& in, std::atomic<float>& g, std::function<float()> thr)
    : input (in), gr (g), threshold (std::move (thr))
{
    inHist.assign (240, -100.0f);
    grHist.assign (240, 0.0f);
    startTimerHz (30);
}

void MeterHistory::timerCallback()
{
    inHist.erase (inHist.begin());
    inHist.push_back (input.exchange (-100.0f));
    grHist.erase (grHist.begin());
    lastGr = gr.exchange (0.0f);
    grHist.push_back (lastGr);
    repaint();
}

void MeterHistory::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    Glass::well (g, r, 10.0f);
    auto inner = r.reduced (10.0f, 8.0f);

    auto dbToY = [&] (float db) { return inner.getY() + inner.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 60.0f); };

    g.setFont (juce::FontOptions (10.0f));
    for (float db : { -6.0f, -12.0f, -24.0f, -36.0f, -48.0f })
    {
        const float y = dbToY (db);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
        g.setColour (Theme::textDim.withAlpha (0.5f));
        g.drawText (juce::String ((int) db), (int) inner.getRight() - 28, (int) y - 7, 26, 14, juce::Justification::centredRight);
    }

    const float step = inner.getWidth() / (float) (inHist.size() - 1);

    juce::Path level;
    level.startNewSubPath (inner.getX(), inner.getBottom());
    for (size_t i = 0; i < inHist.size(); ++i)
        level.lineTo (inner.getX() + step * (float) i, dbToY (inHist[i]));
    level.lineTo (inner.getRight(), inner.getBottom());
    level.closeSubPath();
    g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.45f), inner.getX(), inner.getY(),
                                             Theme::accent.withAlpha (0.05f), inner.getX(), inner.getBottom(), false));
    g.fillPath (level);

    juce::Path grPath;
    for (size_t i = 0; i < grHist.size(); ++i)
    {
        const float x = inner.getX() + step * (float) i;
        const float y = inner.getY() + inner.getHeight() * juce::jlimit (0.0f, 1.0f, grHist[i] / 30.0f);
        if (i == 0) grPath.startNewSubPath (x, y); else grPath.lineTo (x, y);
    }
    g.setColour (Theme::accent2.withAlpha (0.3f));
    g.strokePath (grPath, juce::PathStrokeType (4.0f));
    g.setColour (Theme::accent2);
    g.strokePath (grPath, juce::PathStrokeType (1.6f));

    if (threshold)
    {
        const float y = dbToY (threshold());
        juce::Path dash;
        dash.startNewSubPath (inner.getX(), y);
        dash.lineTo (inner.getRight(), y);
        const float pattern[] = { 6.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dash, dash, pattern, 2);
        g.setColour (Theme::solo.withAlpha (0.7f));
        g.fillPath (dash);
    }

    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("GR  " + juce::String (-lastGr, 1) + " dB", inner.toNearestInt().removeFromTop (16).withTrimmedLeft (4),
                juce::Justification::centredLeft);
}
