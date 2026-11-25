#include "UiHelpers.h"
#include "PluginWindows.h"
#include "Presets.h"
#include "Plugins/Builtins.h"
#include "FlpImport.h"

namespace UiHelpers
{
    void fillInputBox (juce::ComboBox& box, AudioEngine& engine, Track& track)
    {
        box.clear (juce::dontSendNotification);

        juce::StringArray names;
        if (auto* dev = engine.deviceManager.getCurrentAudioDevice())
        {
            const auto all = dev->getInputChannelNames();
            const auto active = dev->getActiveInputChannels();
            for (int i = 0; i < all.size(); ++i)
                if (active[i])
                    names.add (all[i]);
        }

        for (int i = 0; i < names.size(); ++i)
            box.addItem ("In " + juce::String (i + 1) + "  " + names[i], 1 + i);

        if (names.size() >= 2)
        {
            box.addSeparator();
            for (int i = 0; i + 1 < names.size(); i += 2)
                box.addItem ("Stereo " + juce::String (i + 1) + "/" + juce::String (i + 2), 1001 + i);
        }

        const int wanted = track.stereoInput ? 1001 + track.inputChannel.load() : 1 + track.inputChannel.load();
        if (box.indexOfItemId (wanted) >= 0)
            box.setSelectedId (wanted, juce::dontSendNotification);
        else if (box.getNumItems() > 0)
        {
            box.setSelectedItemIndex (0, juce::dontSendNotification);
            applyInputSelection (box, track);
        }
    }

    void applyInputSelection (const juce::ComboBox& box, Track& track)
    {
        const int id = box.getSelectedId();
        if (id >= 1001)   { track.stereoInput = true;  track.inputChannel = id - 1001; }
        else if (id >= 1) { track.stereoInput = false; track.inputChannel = id - 1; }
    }

    //==========================================================================
    void showPresetMenu (AppContext& ctx, Track& track, juce::Component& target, std::function<void()> onChanged)
    {
        const auto presets = ChannelPresets::list();

        juce::PopupMenu menu;
        menu.addSectionHeader ("Channel presets - " + track.name);
        menu.addItem (1, "Save channel preset...");
        menu.addItem (2, "Load preset from file...");
        menu.addItem (4, "Load FL Studio preset (.fst)...");
        menu.addItem (3, "Open presets folder");

        if (! presets.isEmpty())
        {
            menu.addSeparator();
            for (int i = 0; i < presets.size(); ++i)
                menu.addItem (100 + i, presets[i].getFileNameWithoutExtension());
        }

        auto* ctxPtr = &ctx;
        auto* trackPtr = &track;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                            [ctxPtr, trackPtr, presets, onChanged] (int result)
        {
            auto& c = *ctxPtr;
            if (result == 0 || ! c.engine.isValidChannel (trackPtr))
                return;

            if (result == 1)
            {
                savePresetAs (c, *trackPtr);
            }
            else if (result == 2)
            {
                c.chooser = std::make_unique<juce::FileChooser> ("Load channel preset", ChannelPresets::getFolder(),
                                                                 juce::String ("*") + ChannelPresets::fileExtension + ";*.fst");
                c.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                        [ctxPtr, trackPtr, onChanged] (const juce::FileChooser& fc)
                {
                    if (fc.getResult().existsAsFile() && ctxPtr->engine.isValidChannel (trackPtr))
                        loadPreset (*ctxPtr, *trackPtr, fc.getResult(), onChanged);
                });
            }
            else if (result == 4)
            {
                c.chooser = std::make_unique<juce::FileChooser> ("Load FL Studio preset (mixer track state or effect preset)", flPresetFolder(), "*.fst");
                c.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                        [ctxPtr, trackPtr, onChanged] (const juce::FileChooser& fc)
                {
                    if (fc.getResult().existsAsFile() && ctxPtr->engine.isValidChannel (trackPtr))
                        loadPreset (*ctxPtr, *trackPtr, fc.getResult(), onChanged);
                });
            }
            else if (result == 3)
            {
                ChannelPresets::getFolder().startAsProcess();
            }
            else if (result >= 100 && result - 100 < presets.size())
            {
                loadPreset (c, *trackPtr, presets[result - 100], onChanged);
            }
        });
    }

    void savePresetAs (AppContext& ctx, Track& track)
    {
        auto defaultFile = ChannelPresets::getFolder().getChildFile (juce::File::createLegalFileName (track.name) + ChannelPresets::fileExtension);
        ctx.chooser = std::make_unique<juce::FileChooser> ("Save channel preset", defaultFile,
                                                           juce::String ("*") + ChannelPresets::fileExtension);

        auto* ctxPtr = &ctx;
        auto* trackPtr = &track;
        ctx.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [ctxPtr, trackPtr] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File() || ! ctxPtr->engine.isValidChannel (trackPtr))
                return;

            auto result = ChannelPresets::save (*trackPtr, file.withFileExtension (ChannelPresets::fileExtension));
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save failed", result.getErrorMessage());
        });
    }

    juce::File flPresetFolder()
    {
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        for (auto f : { docs.getChildFile ("Image-Line/FL Studio/Presets/Mixer presets"),
                        docs.getChildFile ("Image-Line/FL Studio/Presets"),
                        juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads") })
            if (f.isDirectory())
                return f;
        return docs;
    }

    void loadPreset (AppContext& ctx, Track& track, const juce::File& file, std::function<void()> onChanged)
    {
        ctx.pluginWindows.closeAllFor (track);
        if (FlpImport::isFlPreset (file))
        {
            FlpImport::Report report;
            auto r = FlpImport::importPreset (ctx.engine, track, file, report);
            if (onChanged)
                onChanged();
            if (r.failed())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't load FL Studio preset", r.getErrorMessage());
            else if (ctx.showReport)
                ctx.showReport ("FL Studio preset - " + file.getFileNameWithoutExtension(), report.toText());
            return;
        }
        auto result = ChannelPresets::load (ctx.engine, track, file);
        if (onChanged)
            onChanged();

        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Preset loaded with problems", result.getErrorMessage());
    }

    //==========================================================================
    juce::String formatBpm (double bpm)
    {
        return std::abs (bpm - std::round (bpm)) < 0.005 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 2).trimCharactersAtEnd ("0");
    }

    void offerTempo (AudioEngine& engine, double bpm, const juce::String& title, const juce::String& extraText)
    {
        auto* box = new juce::AlertWindow (title, (extraText.isNotEmpty() ? extraText + "\n\n" : juce::String())
                                                      + "Tempo: " + formatBpm (bpm) + " BPM.\nSome beats feel half or double time - pick the one that matches how you count it.",
                                           juce::MessageBoxIconType::QuestionIcon);
        box->addButton ("Set " + formatBpm (bpm), 1, juce::KeyPress (juce::KeyPress::returnKey));
        if (bpm / 2.0 >= 40.0)  box->addButton ("Half: " + formatBpm (bpm / 2.0), 2);
        if (bpm * 2.0 <= 240.0) box->addButton ("Double: " + formatBpm (bpm * 2.0), 3);
        box->addButton ("Close", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        auto* eng = &engine;
        box->enterModalState (true, juce::ModalCallbackFunction::create ([eng, bpm] (int r)
        {
            if (r == 1) eng->bpm = bpm;
            if (r == 2) eng->bpm = bpm / 2.0;
            if (r == 3) eng->bpm = bpm * 2.0;
        }), true);
    }

    //==========================================================================
    void choosePlugin (AppContext& ctx, juce::Component& target, std::function<void (const juce::PluginDescription&)> onChosen)
    {
        juce::Array<juce::PluginDescription> types;
        for (const auto& d : ctx.engine.knownPlugins.getTypes())
            if (! d.isInstrument)
                types.add (d);

        juce::PopupMenu menu;
        if (types.isEmpty())
        {
            menu.addItem (-1, "No effect plug-ins scanned yet", false, false);
            menu.addItem (-2, "Scan for VST3 plug-ins...");
        }
        else
        {
            juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);
        }

        auto* ctxPtr = &ctx;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                            [ctxPtr, types, onChosen] (int result)
        {
            if (result == -2)
            {
                if (ctxPtr->showPluginManager)
                    ctxPtr->showPluginManager();
                return;
            }

            const int chosen = juce::KnownPluginList::getIndexChosenByMenu (types, result);
            if (chosen >= 0)
                onChosen (types.getReference (chosen));
        });
    }

    bool loadPluginIntoSlot (AppContext& ctx, Track& track, int slot, const juce::PluginDescription& desc, bool openEditor)
    {
        if (slot < 0 || slot >= (int) track.inserts.size())
            return false;

        if (desc.pluginFormatName == "AMETHYST" && ctx.tier)
        {
            const int needed = Builtins::requiredTier (desc.fileOrIdentifier);
            if (ctx.tier() < needed)
            {
                if (ctx.showProPrompt)
                    ctx.showProPrompt (desc.name, needed);
                return false;
            }
        }

        juce::String error;
        auto plugin = ctx.engine.createPlugin (desc, error);

        if (plugin == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Couldn't load plug-in", desc.name + "\n\n" + error);
            return false;
        }

        if (auto* oldPlugin = track.inserts[(size_t) slot]->plugin.get())
            ctx.pluginWindows.close (*oldPlugin);

        auto old = ctx.engine.insertPlugin (track, slot, std::move (plugin));
        old.reset();

        if (openEditor)
            if (auto* p = track.inserts[(size_t) slot]->plugin.get())
                ctx.pluginWindows.show (*p);

        return true;
    }

    bool describeVst3File (AppContext& ctx, const juce::File& file, juce::PluginDescription& out)
    {
        juce::OwnedArray<juce::PluginDescription> found;

        for (auto* format : ctx.engine.pluginFormats.getFormats())
            if (format->getName() == "VST3")
                format->findAllTypesForFile (found, file.getFullPathName());

        for (auto* d : found)
        {
            if (! d->isInstrument)
            {
                out = *d;
                ctx.engine.knownPlugins.addType (*d);
                return true;
            }
        }

        if (! found.isEmpty())
        {
            out = *found[0];
            ctx.engine.knownPlugins.addType (*found[0]);
            return true;
        }

        return false;
    }
}
