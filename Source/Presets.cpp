#include "Presets.h"

namespace ChannelPresets
{
    juce::File getFolder()
    {
        auto f = AudioEngine::getAppFolder().getChildFile ("Channel Presets");
        f.createDirectory();
        return f;
    }

    juce::Array<juce::File> list()
    {
        auto files = getFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });
        return files;
    }

    std::unique_ptr<juce::XmlElement> toXml (const Track& track, const juce::String& presetName)
    {
        auto xml = std::make_unique<juce::XmlElement> ("CHANNEL_PRESET");
        xml->setAttribute ("version", 2);
        xml->setAttribute ("name", presetName);

        auto* strip = xml->createNewChildElement ("STRIP");
        strip->setAttribute ("volumeDb", (double) track.volumeDb.load());
        strip->setAttribute ("pan", (double) track.pan.load());

        for (size_t i = 0; i < track.inserts.size(); ++i)
        {
            const auto& slot = track.inserts[i];
            if (slot->plugin == nullptr)
                continue;

            auto* ins = xml->createNewChildElement ("INSERT");
            ins->setAttribute ("index", (int) i);
            ins->setAttribute ("bypassed", slot->bypassed.load());
            ins->setAttribute ("mix", (double) slot->mix.load());
            ins->addChildElement (slot->plugin->getPluginDescription().createXml().release());

            juce::MemoryBlock state;
            slot->plugin->getStateInformation (state);
            ins->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        }

        return xml;
    }

    juce::Result applyXml (AudioEngine& engine, Track& track, const juce::XmlElement& xml)
    {
        if (! xml.hasTagName ("CHANNEL_PRESET"))
            return juce::Result::fail ("Not a channel preset.");

        juce::StringArray errors;
        std::vector<std::unique_ptr<InsertSlot>> slots ((size_t) Track::maxInserts);
        int fallbackIndex = 0;

        for (auto* ins : xml.getChildWithTagNameIterator ("INSERT"))
        {
            // Respect the saved slot position; older presets without one fill from the top.
            int index = ins->getIntAttribute ("index", -1);
            if (index < 0 || index >= Track::maxInserts || slots[(size_t) index] != nullptr)
            {
                while (fallbackIndex < Track::maxInserts && slots[(size_t) fallbackIndex] != nullptr)
                    ++fallbackIndex;
                index = fallbackIndex;
            }
            if (index >= Track::maxInserts)
                break;

            juce::PluginDescription desc;
            auto* pd = ins->getChildByName ("PLUGIN");
            if (pd == nullptr || ! desc.loadFromXml (*pd))
            {
                errors.add ("Unreadable plug-in entry");
                continue;
            }

            juce::String err;
            auto plugin = engine.createPlugin (desc, err);
            if (plugin == nullptr)
            {
                errors.add (desc.name + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
                continue;
            }

            if (auto* st = ins->getChildByName ("STATE"))
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (st->getAllSubText().trim()) && mb.getSize() > 0)
                    plugin->setStateInformation (mb.getData(), (int) mb.getSize());
            }

            auto slot = std::make_unique<InsertSlot>();
            slot->plugin = std::move (plugin);
            slot->bypassed = ins->getBoolAttribute ("bypassed", false);
            slot->mix = (float) juce::jlimit (0.0, 1.0, ins->getDoubleAttribute ("mix", 1.0));
            slots[(size_t) index] = std::move (slot);
        }

        if (auto* strip = xml.getChildByName ("STRIP"))
        {
            track.volumeDb = (float) strip->getDoubleAttribute ("volumeDb", 0.0);
            track.pan = (float) juce::jlimit (-1.0, 1.0, strip->getDoubleAttribute ("pan", 0.0));
        }

        auto old = engine.replaceInserts (track, std::move (slots));
        old.clear(); // old plug-ins destroyed outside the audio lock

        if (errors.isEmpty())
            return juce::Result::ok();

        return juce::Result::fail ("These plug-ins could not be loaded:\n" + errors.joinIntoString ("\n"));
    }

    juce::Result save (const Track& track, const juce::File& file)
    {
        auto xml = toXml (track, file.getFileNameWithoutExtension());
        if (! xml->writeTo (file))
            return juce::Result::fail ("Couldn't write " + file.getFullPathName());
        return juce::Result::ok();
    }

    juce::Result load (AudioEngine& engine, Track& track, const juce::File& file)
    {
        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr)
            return juce::Result::fail ("Couldn't read " + file.getFullPathName());
        return applyXml (engine, track, *xml);
    }
}

//==============================================================================
namespace ProjectIO
{
    juce::Result save (AudioEngine& engine, const juce::File& file)
    {
        juce::XmlElement root ("VOCALDAW_PROJECT");
        root.setAttribute ("version", 2);
        root.setAttribute ("bpm", engine.bpm.load());
        root.setAttribute ("metronome", engine.metronomeOn.load());

        auto& master = engine.getMaster();
        auto* mx = root.createNewChildElement ("MASTER");
        mx->setAttribute ("mute", master.mute.load());
        mx->addChildElement (ChannelPresets::toXml (master, "Master").release());

        const double sr = engine.getSampleRate();

        for (auto* t : engine.getTracks())
        {
            auto* tx = root.createNewChildElement ("TRACK");
            tx->setAttribute ("name", t->name);
            tx->setAttribute ("inputChannel", t->inputChannel.load());
            tx->setAttribute ("stereoInput", t->stereoInput.load());
            tx->setAttribute ("mute", t->mute.load());
            tx->setAttribute ("solo", t->solo.load());
            tx->setAttribute ("monitor", t->monitor.load());
            tx->addChildElement (ChannelPresets::toXml (*t, t->name).release());

            for (auto& clip : t->clips)
            {
                auto* cx = tx->createNewChildElement ("CLIP");
                cx->setAttribute ("file", clip->file.getFullPathName());
                cx->setAttribute ("startSeconds", (double) clip->startSample.load() / sr);
                if (! clip->enabled.load())
                    cx->setAttribute ("enabled", false);
                clip->settings.writeTo (*cx);
            }
        }

        if (! root.writeTo (file))
            return juce::Result::fail ("Couldn't write " + file.getFullPathName());
        return juce::Result::ok();
    }

    juce::Result load (AudioEngine& engine, const juce::File& file)
    {
        auto root = juce::XmlDocument::parse (file);
        if (root == nullptr || ! root->hasTagName ("VOCALDAW_PROJECT"))
            return juce::Result::fail ("Not an AMETHYST project.");

        engine.stop();
        engine.clearTracks();
        engine.setTransportPosition (0);

        engine.bpm = root->getDoubleAttribute ("bpm", 120.0);
        engine.metronomeOn = root->getBoolAttribute ("metronome", false);

        juce::StringArray problems;
        auto& master = engine.getMaster();

        if (auto* mx = root->getChildByName ("MASTER"))
        {
            master.mute = mx->getBoolAttribute ("mute", false);
            if (auto* preset = mx->getChildByName ("CHANNEL_PRESET"))
            {
                auto r = ChannelPresets::applyXml (engine, master, *preset);
                if (r.failed())
                    problems.add ("Master: " + r.getErrorMessage());
            }
        }
        else
        {
            engine.replaceInserts (master, {}).clear();
            master.volumeDb = (float) root->getDoubleAttribute ("masterVolumeDb", 0.0);
            master.pan = 0.0f;
        }

        const double sr = engine.getSampleRate();

        for (auto* tx : root->getChildWithTagNameIterator ("TRACK"))
        {
            auto* t = engine.addTrack (tx->getStringAttribute ("name", "Track"));
            t->inputChannel = tx->getIntAttribute ("inputChannel", 0);
            t->stereoInput = tx->getBoolAttribute ("stereoInput", false);
            t->mute = tx->getBoolAttribute ("mute", false);
            t->solo = tx->getBoolAttribute ("solo", false);
            t->monitor = tx->getBoolAttribute ("monitor", false);

            if (auto* preset = tx->getChildByName ("CHANNEL_PRESET"))
            {
                auto r = ChannelPresets::applyXml (engine, *t, *preset);
                if (r.failed())
                    problems.add (t->name + ": " + r.getErrorMessage());
            }

            for (auto* cx : tx->getChildWithTagNameIterator ("CLIP"))
            {
                juce::File clipFile (cx->getStringAttribute ("file"));
                const auto start = (juce::int64) std::llround (cx->getDoubleAttribute ("startSeconds") * sr);

                if (auto clip = engine.loadClip (clipFile, start))
                {
                    clip->enabled = cx->getBoolAttribute ("enabled", true);
                    clip->settings.readFrom (*cx);
                    if (! clip->settings.isNeutral())
                        engine.renderNow (*clip);
                    engine.addClip (*t, std::move (clip));
                }
                else
                    problems.add (t->name + ": missing audio " + clipFile.getFullPathName());
            }
        }

        if (problems.isEmpty())
            return juce::Result::ok();
        return juce::Result::fail (problems.joinIntoString ("\n"));
    }
}
