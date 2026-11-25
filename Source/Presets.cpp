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
        {
            const double srNotes = juce::jmax (1.0, engine.getSampleRate());
            if (engine.hasLoopRegion())
            {
                root.setAttribute ("loopStartSeconds", (double) engine.loopStart.load() / srNotes);
                root.setAttribute ("loopEndSeconds", (double) engine.loopEnd.load() / srNotes);
                root.setAttribute ("loopOn", engine.loopEnabled.load());
            }
            if (! engine.markers.empty())
            {
                auto* mk = root.createNewChildElement ("MARKERS");
                for (auto& m : engine.markers)
                {
                    auto* x = mk->createNewChildElement ("MARKER");
                    x->setAttribute ("t", m.seconds);
                    x->setAttribute ("name", m.name);
                }
            }
            if (engine.lyrics.isNotEmpty())
                root.createNewChildElement ("LYRICS")->addTextElement (engine.lyrics);
        }

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
            tx->setAttribute ("inputGainDb", (double) t->inputGainDb.load());
            tx->setAttribute ("mute", t->mute.load());
            tx->setAttribute ("solo", t->solo.load());
            tx->setAttribute ("monitor", t->monitor.load());
            tx->setAttribute ("layerTakes", t->layerTakes.load());
            tx->setAttribute ("takesToFreeTrack", t->takesToFreeTrack);
            if (! t->colour.isTransparent())
                tx->setAttribute ("colour", t->colour.toString());
            if (t->lowCutHz.load() > 1.0f)
                tx->setAttribute ("lowCutHz", (double) t->lowCutHz.load());
            if (t->phaseInvert.load())
                tx->setAttribute ("phaseInvert", true);
            if (t->isBus)
            {
                tx->setAttribute ("isBus", true);
                tx->setAttribute ("busIndex", t->busIndex);
            }
            {
                juce::StringArray sends;
                for (int b = 0; b < Track::maxBuses; ++b)
                    if (t->sends[(size_t) b].load() > 0.0001f)
                        sends.add (juce::String (b) + ":" + juce::String (t->sends[(size_t) b].load(), 4));
                if (! sends.isEmpty())
                    tx->setAttribute ("sends", sends.joinIntoString (","));
            }
            if (t->playThrough != nullptr)
                tx->setAttribute ("playThroughIndex", engine.getTracks().indexOf (t->playThrough));
            tx->addChildElement (ChannelPresets::toXml (*t, t->name).release());

            if (auto* a = t->automation.get())
            {
                auto* ax = tx->createNewChildElement ("AUTOMATION");
                ax->setAttribute ("kind", a->kind == Automation::Kind::Volume ? "volume" : a->kind == Automation::Kind::Pan ? "pan" : "param");
                // target: -1 = master, -2 = deleted, else track index
                ax->setAttribute ("target", a->target == nullptr ? -2 : a->target == &master ? -1 : engine.getTracks().indexOf (a->target));
                ax->setAttribute ("slot", a->slot);
                ax->setAttribute ("param", a->param);
                for (auto& p : a->points)
                {
                    auto* px = ax->createNewChildElement ("P");
                    px->setAttribute ("t", p.seconds);
                    px->setAttribute ("v", (double) p.value);
                }
            }

            for (auto& clip : t->clips)
            {
                auto* cx = tx->createNewChildElement ("CLIP");
                cx->setAttribute ("file", clip->file.getFullPathName());
                cx->setAttribute ("startSeconds", (double) clip->startSample.load() / sr);
                if (clip->name.isNotEmpty())
                    cx->setAttribute ("name", clip->name);
                if (! clip->colour.isTransparent())
                    cx->setAttribute ("colour", clip->colour.toString());
                if (clip->getOffset() > 0)
                    cx->setAttribute ("trimOffsetSeconds", (double) clip->getOffset() / sr);
                if (clip->trimLength.load() >= 0)
                    cx->setAttribute ("trimLengthSeconds", (double) clip->getLength() / sr);
                if (! clip->enabled.load())
                    cx->setAttribute ("enabled", false);
                if (std::abs (clip->handleGainDb.load()) > 0.01f)
                    cx->setAttribute ("clipGainDb", (double) clip->handleGainDb.load());
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
        engine.clearSessionNotes();
        {
            const double srNotes = juce::jmax (1.0, engine.getSampleRate());
            if (root->hasAttribute ("loopStartSeconds"))
            {
                engine.loopStart = (juce::int64) std::llround (root->getDoubleAttribute ("loopStartSeconds") * srNotes);
                engine.loopEnd = (juce::int64) std::llround (root->getDoubleAttribute ("loopEndSeconds") * srNotes);
                engine.loopEnabled = root->getBoolAttribute ("loopOn", false);
            }
            if (auto* mk = root->getChildByName ("MARKERS"))
                for (auto* x : mk->getChildWithTagNameIterator ("MARKER"))
                    engine.markers.push_back ({ x->getDoubleAttribute ("t"), x->getStringAttribute ("name") });
            std::sort (engine.markers.begin(), engine.markers.end(), [] (auto& a, auto& b) { return a.seconds < b.seconds; });
            if (auto* ly = root->getChildByName ("LYRICS"))
                engine.lyrics = ly->getAllSubText();
        }

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

        std::vector<std::pair<Track*, int>> routes; // applied once every track exists
        std::vector<std::pair<Track*, int>> automationTargets;
        for (auto* tx : root->getChildWithTagNameIterator ("TRACK"))
        {
            auto* t = engine.addTrack (tx->getStringAttribute ("name", "Track"));
            t->inputChannel = tx->getIntAttribute ("inputChannel", 0);
            t->stereoInput = tx->getBoolAttribute ("stereoInput", false);
            t->inputGainDb = (float) juce::jlimit (-12.0, 24.0, tx->getDoubleAttribute ("inputGainDb", 0.0));
            t->mute = tx->getBoolAttribute ("mute", false);
            t->solo = tx->getBoolAttribute ("solo", false);
            t->monitor = tx->getBoolAttribute ("monitor", false);
            t->layerTakes = tx->getBoolAttribute ("layerTakes", false); // older projects used "newest on top"
            t->takesToFreeTrack = tx->getBoolAttribute ("takesToFreeTrack", true);
            if (tx->hasAttribute ("colour"))
                t->colour = juce::Colour::fromString (tx->getStringAttribute ("colour"));
            t->lowCutHz = (float) juce::jlimit (0.0, 1000.0, tx->getDoubleAttribute ("lowCutHz", 0.0));
            t->phaseInvert = tx->getBoolAttribute ("phaseInvert", false);
            if (tx->getBoolAttribute ("isBus", false))
            {
                t->isBus = true;
                t->busIndex = juce::jlimit (0, Track::maxBuses - 1, tx->getIntAttribute ("busIndex", 0));
            }
            for (auto& item : juce::StringArray::fromTokens (tx->getStringAttribute ("sends"), ",", {}))
            {
                const int b = item.upToFirstOccurrenceOf (":", false, false).getIntValue();
                if (juce::isPositiveAndBelow (b, Track::maxBuses))
                    t->sends[(size_t) b] = (float) juce::jlimit (0.0, 1.0, item.fromFirstOccurrenceOf (":", false, false).getDoubleValue());
            }
            if (tx->hasAttribute ("playThroughIndex"))
                routes.emplace_back (t, tx->getIntAttribute ("playThroughIndex"));

            if (auto* preset = tx->getChildByName ("CHANNEL_PRESET"))
            {
                auto r = ChannelPresets::applyXml (engine, *t, *preset);
                if (r.failed())
                    problems.add (t->name + ": " + r.getErrorMessage());
            }

            if (auto* ax = tx->getChildByName ("AUTOMATION"))
            {
                auto a = std::make_unique<Automation>();
                const auto kind = ax->getStringAttribute ("kind");
                a->kind = kind == "volume" ? Automation::Kind::Volume : kind == "pan" ? Automation::Kind::Pan : Automation::Kind::Param;
                a->slot = ax->getIntAttribute ("slot", -1);
                a->param = ax->getIntAttribute ("param", -1);
                for (auto* px : ax->getChildWithTagNameIterator ("P"))
                    a->points.push_back ({ px->getDoubleAttribute ("t"), (float) juce::jlimit (0.0, 1.0, px->getDoubleAttribute ("v")) });
                std::sort (a->points.begin(), a->points.end(), [] (auto& x, auto& y) { return x.seconds < y.seconds; });
                automationTargets.emplace_back (t, ax->getIntAttribute ("target", -2));
                const juce::ScopedLock sl (engine.getLock());
                t->automation = std::move (a);
            }

            for (auto* cx : tx->getChildWithTagNameIterator ("CLIP"))
            {
                juce::File clipFile (cx->getStringAttribute ("file"));
                const auto start = (juce::int64) std::llround (cx->getDoubleAttribute ("startSeconds") * sr);

                if (auto clip = engine.loadClip (clipFile, start))
                {
                    clip->enabled = cx->getBoolAttribute ("enabled", true);
                    clip->handleGainDb = (float) juce::jlimit (-36.0, 12.0, cx->getDoubleAttribute ("clipGainDb", 0.0));
                    clip->name = cx->getStringAttribute ("name");
                    if (cx->hasAttribute ("colour"))
                        clip->colour = juce::Colour::fromString (cx->getStringAttribute ("colour"));
                    clip->settings.readFrom (*cx);
                    if (! clip->settings.isNeutral())
                        engine.renderNow (*clip);
                    clip->trimOffset = (juce::int64) std::llround (cx->getDoubleAttribute ("trimOffsetSeconds", 0.0) * sr);
                    if (cx->hasAttribute ("trimLengthSeconds"))
                        clip->trimLength = (juce::int64) std::llround (cx->getDoubleAttribute ("trimLengthSeconds") * sr);
                    engine.addClip (*t, std::move (clip));
                }
                else
                    problems.add (t->name + ": missing audio " + clipFile.getFullPathName());
            }
        }

        for (auto& [lane, index] : routes)
            if (auto* channel = engine.getTracks()[index])
                engine.setPlayThrough (*lane, channel);

        for (auto& [lane, index] : automationTargets)
        {
            Track* target = index == -1 ? &master : engine.getTracks()[index];
            const juce::ScopedLock sl (engine.getLock());
            lane->automation->target = target != nullptr && ! target->isAutomation() ? target : nullptr;
        }

        if (problems.isEmpty())
            return juce::Result::ok();
        return juce::Result::fail (problems.joinIntoString ("\n"));
    }
}
