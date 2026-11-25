#include "FlpImport.h"
#include "Plugins/Builtins.h"
#include "Plugins/AmethystPlugin.h"

namespace FlpImport
{
namespace
{
    struct Mapping { const char* fl; const char* id; };

    // FL Studio native effect (internal name) -> AMETHYST built-in.
    const Mapping kMappings[] = {
        { "Fruity Reeverb 2", "amethyst.reverb" },       { "Fruity Reeverb", "amethyst.reverb" },
        { "Fruity Convolver", "amethyst.reverb" },       { "Fruity Reverb", "amethyst.reverb" },
        { "Fruity Parametric EQ 2", "amethyst.eq" },     { "Fruity Parametric EQ", "amethyst.eq" },
        { "Fruity 7 Band EQ", "amethyst.eq" },           { "Fruity EQ", "amethyst.eq" },
        { "Fruity Filter", "amethyst.eq" },              { "Fruity Love Philter", "amethyst.eq" },
        { "Fruity Limiter", "amethyst.dynamics" },       { "Fruity Compressor", "amethyst.dynamics" },
        { "Fruity Multiband Compressor", "amethyst.dynamics" }, { "Maximus", "amethyst.dynamics" },
        { "Emphasizer", "amethyst.dynamics" },           { "Transient Processor", "amethyst.dynamics" },
        { "Fruity Delay 2", "amethyst.echo" },           { "Fruity Delay 3", "amethyst.echo" },
        { "Fruity Delay", "amethyst.echo" },             { "Fruity Delay Bank", "amethyst.echo" },
        { "Fruity Flanger", "amethyst.prism" },          { "Fruity Chorus", "amethyst.prism" },
        { "Fruity Flangus", "amethyst.prism" },          { "Fruity Phaser", "amethyst.prism" },
        { "Vintage Chorus", "amethyst.prism" },          { "Vintage Phaser", "amethyst.prism" },
        { "Soundgoodizer", "amethyst.shine" },           { "Fruity Stereo Enhancer", "amethyst.doubler" },
        { "Fruity Stereo Shaper", "amethyst.doubler" },
        { "Pitcher", "amethyst.tune" },                  { "Newtone", "amethyst.tune" },
        { "Pitch Shifter", "amethyst.shift" },           { "Fruity Pitch Shifter", "amethyst.shift" },
        { "Fruity Blood Overdrive", "amethyst.warmth" }, { "Fruity Fast Dist", "amethyst.warmth" },
        { "Fruity WaveShaper", "amethyst.warmth" },      { "Fruity Squeeze", "amethyst.warmth" },
        { "Fruity Soft Clipper", "amethyst.warmth" },    { "Distructor", "amethyst.warmth" },
        { "Hardcore", "amethyst.warmth" },
    };

    juce::String builtinDisplayName (const juce::String& id)
    {
        for (auto& d : Builtins::getDescriptions())
            if (d.fileOrIdentifier == id)
                return d.name;
        return id;
    }

    bool findBuiltin (const juce::String& id, juce::PluginDescription& out)
    {
        for (auto& d : Builtins::getDescriptions())
            if (d.fileOrIdentifier == id) { out = d; return true; }
        return false;
    }

    // Find an installed plug-in for a Fruity Wrapper entry: scanned list by name (VST3 first),
    // then the .vst3 file FL used, if it's still there.
    bool findVst (AudioEngine& engine, const Flp::Plugin& p, juce::PluginDescription& out)
    {
        const auto name = p.vstName.isNotEmpty() ? p.vstName : p.displayName;
        const juce::PluginDescription* best = nullptr;
        juce::Array<juce::PluginDescription> types = engine.knownPlugins.getTypes();
        for (auto& t : types)
        {
            if (! t.name.equalsIgnoreCase (name) || t.isInstrument)
                continue;
            if (best == nullptr || (t.pluginFormatName == "VST3" && best->pluginFormatName != "VST3"))
                best = &t;
        }
        if (best != nullptr) { out = *best; return true; }

        if (p.vstPath.endsWithIgnoreCase (".vst3") && juce::File::isAbsolutePath (p.vstPath) && juce::File (p.vstPath).exists())
        {
            for (auto* format : engine.pluginFormats.getFormats())
            {
                if (format->getName() != "VST3")
                    continue;
                juce::OwnedArray<juce::PluginDescription> found;
                format->findAllTypesForFile (found, p.vstPath);
                for (auto* d : found)
                    if (! d->isInstrument) { out = *d; return true; }
            }
        }
        return false;
    }

    float gainToDb (float g) { return g <= 0.00001f ? -60.0f : juce::jlimit (-60.0f, 12.0f, 20.0f * std::log10 (g)); }

    //==== FL native effect settings -> AMETHYST parameters ======================
    // Layouts were worked out from FL Studio's own factory presets (.fst) and real projects.
    // Each returns a short note for the report, or "" if the data didn't look right.
    struct Blob
    {
        const juce::MemoryBlock& mb;
        bool has (size_t bytes) const { return mb.getSize() >= bytes; }
        int i32 (size_t offset) const
        {
            if (offset + 4 > mb.getSize()) return 0;
            auto* p = (const juce::uint8*) mb.getData() + offset;
            return (int) ((juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24));
        }
        float f32 (size_t offset) const
        {
            const auto v = (juce::uint32) i32 (offset);
            float f;
            std::memcpy (&f, &v, 4);
            return std::isfinite (f) ? f : 0.0f;
        }
        int u8 (size_t offset) const { return offset < mb.getSize() ? ((const juce::uint8*) mb.getData())[offset] : 0; }
    };

    float linToDb (double lin, float floorDb) { return lin <= 0.0 ? floorDb : (float) (20.0 * std::log10 (lin)); }
    float pct (double v) { return (float) juce::jlimit (0.0, 100.0, v); }

    // Fruity Parametric EQ 2: version, 7 gains (0.01 dB), 7 freqs (0..65536, log), 7 bandwidths, 7 types.
    juce::String translateEq (const Blob& b, AmethystPlugin& eq)
    {
        if (! b.has (116)) return {};
        // FL band type -> Facet EQ type (Low cut, Low shelf, Peak, High shelf, High cut, Notch)
        constexpr int typeMap[8] = { -1, 4, 2, 0, 5, 1, 2, 3 };
        for (int band = 0; band < 7; ++band)
        {
            const auto id = [band] (const char* what) { return "b" + juce::String (band + 1) + "_" + what; };
            const int flType = b.i32 (88 + 4 * (size_t) band);
            if (flType <= 0 || flType > 7)
            {
                eq.setParam (id ("on"), 0.0f);
                continue;
            }
            const double x = juce::jlimit (0, 65536, b.i32 (32 + 4 * (size_t) band)) / 65536.0;
            const double bw = juce::jmax (500, b.i32 (60 + 4 * (size_t) band));
            eq.setParam (id ("on"), 1.0f);
            eq.setParam (id ("type"), (float) typeMap[flType]);
            eq.setParam (id ("freq"), (float) juce::jlimit (20.0, 20000.0, 25.3 * std::pow (758.0, x)));
            eq.setParam (id ("gain"), juce::jlimit (-18.0f, 18.0f, b.i32 (4 + 4 * (size_t) band) / 100.0f));
            // Peaks: FL bandwidth -> Q (default 25412 = Q 1). Shelves and cuts use it as a slope, so keep those gentle.
            const double rawBw = juce::jmax (0, b.i32 (60 + 4 * (size_t) band));
            double q = std::pow (25412.0 / bw, 1.3);
            if (flType == 5 || flType == 7)      q = 0.3 + 0.6 * rawBw / 40124.0;
            else if (flType == 1 || flType == 3) q = 0.5 + 0.5 * rawBw / 36068.0;
            eq.setParam (id ("q"), (float) juce::jlimit (0.1, 18.0, q));
        }
        return "bands, gains and frequencies translated; Q approximate";
    }

    // Fruity Reeverb 2: [version 10001] low cut Hz, high cut x100 Hz, predelay ms, size, diffusion,
    // decay x0.1 s, high damping x100 Hz, bass x%, crossover Hz, stereo sep, dry/128, early/128, wet/128.
    juce::String translateReverb (const Blob& b, AmethystPlugin& rv)
    {
        const size_t o = b.i32 (0) > 1000 ? 4 : 0;
        if (! b.has (o + 52)) return {};
        auto f = [&] (int k) { return b.i32 (o + 4 * (size_t) k); };
        const double decaySeconds = f (5) / 10.0;
        const double dampHz = juce::jmax (1000, f (6) * 100);
        rv.setParam ("lowcut", (float) juce::jlimit (20, 1000, juce::jmax (20, f (0))));
        rv.setParam ("highcut", (float) juce::jlimit (1000, 20000, f (1) * 100));
        rv.setParam ("predelay", (float) juce::jlimit (0, 250, f (2)));
        rv.setParam ("size", pct (f (3)));
        rv.setParam ("diff", pct (f (4)));
        rv.setParam ("time", (float) juce::jlimit (0.1, 20.0, decaySeconds));
        rv.setParam ("hidamp", (float) juce::jlimit (1000.0, 20000.0, dampHz));
        rv.setParam ("bass", juce::jlimit (0.25f, 2.5f, f (7) / 100.0f));
        rv.setParam ("xover", (float) juce::jlimit (100, 2000, f (8)));
        rv.setParam ("dry", pct (f (10) * 100.0 / 128.0));
        rv.setParam ("er", pct (f (11) * 100.0 / 128.0));
        rv.setParam ("wet", pct (f (12) * 100.0 / 128.0));
        return "settings translated";
    }

    // Fruity Compressor: version, threshold x0.1 dB, ratio x0.1, gain x0.1 dB, attack x0.1 ms, release ms, type.
    juce::String translateCompressor (const Blob& b, AmethystPlugin& dyn)
    {
        if (! b.has (28)) return {};
        dyn.setParam ("mode", 1.0f);
        dyn.setParam ("thresh", juce::jlimit (-48.0f, 0.0f, b.i32 (4) / 10.0f));
        dyn.setParam ("ratio", juce::jlimit (1.0f, 20.0f, b.i32 (8) / 10.0f));
        dyn.setParam ("gain", juce::jlimit (-12.0f, 24.0f, b.i32 (12) / 10.0f));
        dyn.setParam ("att", juce::jlimit (0.05f, 100.0f, b.i32 (16) / 10.0f));
        dyn.setParam ("rel", juce::jlimit (5.0f, 1500.0f, (float) b.i32 (20)));
        dyn.setParam ("knee", (float) ((juce::jmax (0, b.i32 (24)) % 3) * 3));
        return "settings translated (knee from compressor type)";
    }

    // Fruity Limiter: version, gain (1000 = 0 dB, linear), saturation, ceiling (linear), ... noise gate gain/threshold/release at 64/68/72.
    juce::String translateLimiter (const Blob& b, AmethystPlugin& dyn)
    {
        if (! b.has (76)) return {};
        dyn.setParam ("mode", 0.0f);
        dyn.setParam ("gain", juce::jlimit (-12.0f, 24.0f, linToDb (b.i32 (4) / 1000.0, -12.0f)));
        dyn.setParam ("ceil", juce::jlimit (-24.0f, 0.0f, linToDb (b.i32 (12) / 1000.0, -24.0f)));
        const int gateGain = b.i32 (64), gateThr = b.i32 (68), gateRel = b.i32 (72);
        if (gateThr > 0)
        {
            dyn.setParam ("g_thr", juce::jlimit (-90.0f, -10.0f, linToDb (gateThr / 1000.0, -90.0f)));
            // FL's "gain 0" is a hard mute; 40 dB with a hold sounds the same on a quiet room without the chatter
            dyn.setParam ("g_range", gateGain <= 0 ? 40.0f : juce::jlimit (0.0f, 40.0f, -linToDb (gateGain / 1000.0, -40.0f)));
            dyn.setParam ("g_hold", 60.0f);
            dyn.setParam ("g_rel", (float) juce::jlimit (10.0, 1000.0, 10.0 * std::pow (100.0, gateRel / 10000.0)));
            return "limiter gain, ceiling and noise gate translated (gate release approximate)";
        }
        return "limiter gain and ceiling translated";
    }

    // Soundgoodizer: version, mode (A-D), amount (0-1000). Lustre has the same four flavours.
    juce::String translateSoundgoodizer (const Blob& b, AmethystPlugin& shine)
    {
        if (! b.has (12)) return {};
        shine.setParam ("mode", (float) juce::jlimit (0, 3, b.i32 (4)));
        shine.setParam ("amount", pct (b.i32 (8) / 10.0));
        return "mode and amount translated";
    }

    // Fruity Delay 3: feedback mode at 8, time at 20 (1/256 beat), feedback 60, feedback cutoff 68, wet 96, dry 104 (6000 = full).
    juce::String translateDelay3 (const Blob& b, AmethystPlugin& echo, double bpm)
    {
        if (! b.has (108)) return {};
        const double beats = b.i32 (20) / 256.0;
        echo.setParam ("sync", 0.0f);
        echo.setParam ("time", (float) juce::jlimit (1.0, 2000.0, beats * 60000.0 / juce::jmax (20.0, bpm)));
        echo.setParam ("fb_mode", b.i32 (8) == 2 ? 2.0f : 0.0f);
        echo.setParam ("fb", juce::jlimit (0.0f, 95.0f, b.i32 (60) / 60.0f));
        echo.setParam ("fb_cut", (float) juce::jlimit (500.0, 20000.0, 20.0 * std::pow (1000.0, b.i32 (68) / 6000.0)));
        echo.setParam ("wet", pct (b.i32 (96) / 60.0));
        echo.setParam ("dry", pct (b.i32 (104) / 60.0));
        return "time, feedback and levels approximated";
    }

    // Fruity Flangus: order, depth, speed, delay, spread, cross, dry, wet.
    juce::String translateFlangus (const Blob& b, AmethystPlugin& mod)
    {
        if (! b.has (32)) return {};
        const int delay = b.i32 (12);
        mod.setParam ("mode", delay >= 64 ? 1.0f : 0.0f);
        mod.setParam ("delay", juce::jlimit (0.1f, 30.0f, delay * 10.0f / 128.0f));
        mod.setParam ("depth", pct (b.i32 (4) * 100.0 / 1024.0));
        mod.setParam ("speed", (float) juce::jlimit (0.02, 10.0, 0.05 * std::pow (200.0, b.i32 (8) / 1024.0)));
        mod.setParam ("spread", pct (b.i32 (16) * 100.0 / 128.0));
        mod.setParam ("fb", juce::jlimit (-40.0f, 40.0f, b.i32 (20) * 40.0f / 128.0f));   // "cross" isn't true feedback; keep it mild
        mod.setParam ("dry", pct (std::abs (b.i32 (24)) * 100.0 / 128.0));
        mod.setParam ("wet", pct (std::abs (b.i32 (28)) * 100.0 / 128.0));
        return "settings approximated";
    }

    //---- pitch correction -----------------------------------------------------
    const char* const kNotes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // Same scales (and order) as AMETHYST TUNE.
    int scaleBits (int scale)
    {
        auto bits = [] (std::initializer_list<int> notes) { int m = 0; for (int n : notes) m |= 1 << n; return m; };
        switch (scale)
        {
            case 1:  return bits ({ 0, 2, 4, 5, 7, 9, 11 });
            case 2:  return bits ({ 0, 2, 3, 5, 7, 8, 10 });
            case 3:  return bits ({ 0, 2, 3, 5, 7, 8, 11 });
            case 4:  return bits ({ 0, 2, 4, 7, 9 });
            case 5:  return bits ({ 0, 3, 5, 7, 10 });
            case 6:  return bits ({ 0, 3, 5, 6, 7, 10 });
            default: return 0xFFF;
        }
    }

    // Sets Tune's key + scale for a 12-bit note mask (bit 0 = C); falls back to its Custom scale.
    // Returns a description like "A minor".
    juce::String setTuneNotes (AmethystPlugin& tune, int mask)
    {
        static const char* const names[] = { "chromatic", "major", "minor", "harmonic minor", "major pentatonic", "minor pentatonic", "blues" };
        if (mask == 0xFFF)
        {
            tune.setParam ("scale", 0.0f);
            return "chromatic (all notes)";
        }
        for (int scale = 1; scale <= 6; ++scale)
            for (int key = 0; key < 12; ++key)
                if ((((scaleBits (scale) << key) | (scaleBits (scale) >> (12 - key))) & 0xFFF) == mask)
                {
                    tune.setParam ("key", (float) key);
                    tune.setParam ("scale", (float) scale);
                    juce::String s = juce::String (kNotes[key]) + " " + names[scale];
                    if (scale == 1) s << " (same notes as " << kNotes[(key + 9) % 12] << " minor)";
                    return s;
                }
        tune.setParam ("scale", 7.0f);   // Custom
        tune.setParam ("mask", (float) mask);
        juce::StringArray on;
        for (int k = 0; k < 12; ++k)
            if ((mask >> k) & 1) on.add (kNotes[k]);
        return "custom notes " + on.joinIntoString (" ");
    }

    // Pitcher: "SSLF", u32 size, "PTR1", then its parameters as 0..1 floats (Correction Speed first; 1 = instant),
    // and at byte 144 twelve note switches C..B (1 = the note is allowed) - that's the key / scale.
    juce::String translatePitcher (const Blob& b, AmethystPlugin& tune)
    {
        if (! b.has (156) || b.i32 (0) != 0x464C5353)   // "SSLF"
            return {};
        int mask = 0;
        for (int k = 0; k < 12; ++k)
        {
            const int v = b.u8 (144 + (size_t) k);
            if (v > 1) return {};
            if (v != 0) mask |= 1 << k;
        }
        if (mask == 0) mask = 0xFFF;
        const auto notes = setTuneNotes (tune, mask);

        const double speed = juce::jlimit (0.0, 1.0, (double) b.f32 (12));
        const double slow = 1.0 - speed;
        tune.setParam ("retune", (float) juce::jlimit (0.0, 400.0, 400.0 * slow * slow));
        tune.setParam ("humanize", (float) juce::jlimit (0.0, 100.0, slow * 80.0));
        tune.setParam ("lock", 1.0f);
        return "key " + notes + ", correction speed " + juce::String (juce::roundToInt (speed * 100)) + "%";
    }

    // Newtone edits notes by hand on one recording, so there's nothing live to copy: give Tune a gentle setting.
    juce::String translateNewtone (AmethystPlugin& tune)
    {
        tune.setParam ("retune", 70.0f);
        tune.setParam ("humanize", 55.0f);
        return "Newtone's note-by-note edits belong to the old recording; Tune set to natural correction - pick your key";
    }

    //---- delays / filters / stereo / drive --------------------------------------
    // Fruity Delay 2: input pan (-128..128), input vol (128 = 100%), dry (128), feedback (128 = 100%),
    // time (ticks, 192 per beat), stereo offset (ticks), feedback mode (0 normal, 1 invert, 2 ping-pong), cutoff (128 = open).
    juce::String translateDelay2 (const Blob& b, AmethystPlugin& echo, double bpm)
    {
        if (! b.has (32)) return {};
        const int timeTicks = juce::jmax (1, b.i32 (16));
        const double beatMs = 60000.0 / juce::jmax (20.0, bpm);
        echo.setParam ("sync", 0.0f);
        echo.setParam ("time", (float) juce::jlimit (1.0, 2000.0, timeTicks / 192.0 * beatMs));
        echo.setParam ("in_pan", juce::jlimit (-100.0f, 100.0f, b.i32 (0) * 100.0f / 128.0f));
        echo.setParam ("in_vol", pct (b.i32 (4) * 100.0 / 128.0));
        echo.setParam ("dry", pct (b.i32 (8) * 100.0 / 128.0));
        echo.setParam ("fb", juce::jlimit (0.0f, 95.0f, b.i32 (12) * 100.0f / 128.0f));
        echo.setParam ("offset", juce::jlimit (-100.0f, 100.0f, b.i32 (20) * 50.0f / (float) timeTicks));
        echo.setParam ("fb_mode", (float) juce::jlimit (0, 2, b.i32 (24)));
        echo.setParam ("fb_cut", (float) juce::jlimit (500.0, 20000.0, 500.0 * std::pow (40.0, juce::jlimit (0, 128, b.i32 (28)) / 128.0)));
        echo.setParam ("wet", 100.0f);   // Delay 2's first echo comes back at the input level
        echo.setParam ("style", 0.0f);
        return "time, feedback, mode, cutoff and levels translated (stereo offset approximate)";
    }

    // Fruity Filter: version, cutoff (0..1024), resonance, low pass, band pass, high pass amounts (0..1024), x2.
    juce::String translateFilter (const Blob& b, AmethystPlugin& eq)
    {
        if (! b.has (28)) return {};
        const int lp = b.i32 (12), bp = b.i32 (16), hp = b.i32 (20);
        if (lp <= 0 && bp <= 0 && hp <= 0) return {};
        const double f = juce::jlimit (20.0, 20000.0, 20.0 * std::pow (1000.0, juce::jlimit (0, 1024, b.i32 (4)) / 1024.0));
        const float q = (float) juce::jlimit (0.5, 8.0, 0.7 + juce::jlimit (0, 1024, b.i32 (8)) / 1024.0 * 6.0);
        const float slope = b.i32 (24) != 0 ? 1.0f : 0.0f;

        for (int band = 1; band <= 7; ++band)
            eq.setParam ("b" + juce::String (band) + "_on", 0.0f);
        auto cut = [&] (int band, int type, double hz)
        {
            const auto id = [band] (const char* w) { return "b" + juce::String (band) + "_" + w; };
            eq.setParam (id ("on"), 1.0f);
            eq.setParam (id ("type"), (float) type);
            eq.setParam (id ("freq"), (float) juce::jlimit (20.0, 20000.0, hz));
            eq.setParam (id ("q"), q);
            eq.setParam (id ("slope"), slope);
        };
        if (lp >= bp && lp >= hp)      { if (f < 19000.0) cut (7, 4, f); return "low pass at " + juce::String (juce::roundToInt (f)) + " Hz"; }
        if (hp >= bp)                  { cut (1, 0, f); return "high pass at " + juce::String (juce::roundToInt (f)) + " Hz"; }
        cut (1, 0, f / 1.6);
        cut (7, 4, f * 1.6);
        return "band pass around " + juce::String (juce::roundToInt (f)) + " Hz";
    }

    // Fruity Stereo Enhancer: pan, volume (256 = 100%), stereo separation (negative = wider), phase offset, offset side, invert.
    juce::String translateStereoEnhancer (const Blob& b, AmethystPlugin& twin)
    {
        if (! b.has (24)) return {};
        const int sep = b.i32 (8), offset = std::abs (b.i32 (12));
        const double wide = juce::jlimit (0.0, 1.0, -sep / 128.0);
        if (wide <= 0.0 && offset == 0)
        {
            twin.setParam ("mix", 0.0f);   // FL's enhancer at rest changes nothing; keep Twin Voice silent too
            twin.setParam ("dry", 100.0f);
            return "neutral settings (no widening)";
        }
        twin.setParam ("dry", 100.0f);
        twin.setParam ("detune", 3.0f);
        twin.setParam ("human", 10.0f);
        twin.setParam ("delay", (float) juce::jlimit (5.0, 60.0, offset > 0 ? offset / 10.0 : 12.0));
        twin.setParam ("width", pct (40.0 + wide * 60.0));
        twin.setParam ("mix", pct (15.0 + wide * 45.0));
        return "widening approximated with a tight double";
    }

    // Fruity Fast Dist: pre amp (128 = 1x), threshold (1..10, 10 = clean), type (A/B), mix (128 = 100%), post (128 = 1x).
    juce::String translateFastDist (const Blob& b, AmethystPlugin& ember)
    {
        if (! b.has (20)) return {};
        const double preDb = 20.0 * std::log10 (juce::jmax (1, b.i32 (0)) / 128.0);
        const double thr = juce::jlimit (1, 10, b.i32 (4));
        ember.setParam ("mode", b.i32 (8) != 0 ? 2.0f : 1.0f);
        ember.setParam ("drive", (float) juce::jlimit (0.0, 36.0, preDb + (10.0 - thr) * 3.6));
        ember.setParam ("mix", pct (b.i32 (12) * 100.0 / 128.0));
        ember.setParam ("out", juce::jlimit (-24.0f, 12.0f, linToDb (b.i32 (16) / 128.0, -24.0f) - (float) (10.0 - thr) * 1.2f));
        return "drive, type, mix and output translated";
    }

    // Fruity Blood Overdrive: version, pre band, colour (5000 = neutral), pre amp, x100, post filter, post gain (10000 = 1x). 0..10000.
    juce::String translateBloodOverdrive (const Blob& b, AmethystPlugin& ember)
    {
        if (! b.has (28)) return {};
        ember.setParam ("mode", 1.0f);
        ember.setParam ("drive", (float) juce::jlimit (0.0, 36.0, b.i32 (12) / 10000.0 * 24.0 + (b.i32 (16) != 0 ? 12.0 : 0.0)));
        ember.setParam ("tone", juce::jlimit (-100.0f, 100.0f, (b.i32 (8) - 5000) / 50.0f));
        ember.setParam ("out", juce::jlimit (-24.0f, 12.0f, linToDb (b.i32 (24) / 10000.0, -24.0f)));
        ember.setParam ("mix", 100.0f);
        return "drive, colour and output approximated";
    }

    // Fruity Soft Clipper: threshold (128 = no clipping), post gain (128 = 1x).
    juce::String translateSoftClipper (const Blob& b, AmethystPlugin& ember)
    {
        if (! b.has (8)) return {};
        const double thr = juce::jlimit (1, 128, b.i32 (0)) / 128.0;
        ember.setParam ("mode", 0.0f);
        ember.setParam ("drive", (float) juce::jlimit (0.0, 36.0, -20.0 * std::log10 (thr)));
        ember.setParam ("out", juce::jlimit (-24.0f, 12.0f, linToDb (b.i32 (4) / 128.0, -24.0f)));
        ember.setParam ("tone", 0.0f);
        ember.setParam ("mix", 100.0f);
        return "threshold and post gain translated";
    }

    //---- multiband dynamics ------------------------------------------------------
    // The scales below were measured by rendering FL Studio's own plug-ins with chosen settings
    // (a saw at -16 dBFS RMS through each band) and reading back levels and crossover points.

    struct Point { double x, y; };

    // Piecewise log-frequency interpolation through measured points, extended at both ends.
    double interpHz (double x, std::initializer_list<Point> pts)
    {
        const Point* p = pts.begin();
        const size_t n = pts.size();
        size_t i = 1;
        while (i < n - 1 && x > p[i].x) ++i;
        const auto& a = p[i - 1];
        const auto& b = p[i];
        const double t = (x - a.x) / (b.x - a.x);
        return std::exp (std::log (a.y) + t * (std::log (b.y) - std::log (a.y)));
    }

    // Prism-style soft-knee gain reduction for a steady level.
    double staticGr (double levelDb, double thrDb, double ratio, double kneeDb)
    {
        const double over = levelDb - thrDb, slope = 1.0 - 1.0 / ratio;
        if (2.0 * over < -kneeDb) return 0.0;
        if (2.0 * over > kneeDb)  return over * slope;
        const double x = over + kneeDb * 0.5;
        return slope * x * x / (2.0 * kneeDb);
    }

    // Fruity Multiband Compressor (124 bytes): version, master volume, 2 flags, then three bands -
    //   high: state, freq, output, threshold, ratio, attack, release, knee      (4..11)
    //   mid:  state, high freq, low freq, output, threshold, ratio, attack, release, knee   (12..20)
    //   low:  state, freq, output, threshold, ratio, attack, release, knee      (21..28)
    // Knobs are 0..65536. State 0 = compress, 1 = mute, 2 = bypass.
    juce::String translateMultibandComp (const Blob& b, AmethystPlugin& dyn)
    {
        if (! b.has (116)) return {};
        auto knob = [&] (int index) { return juce::jlimit (0.0, 1.0, b.i32 (4 * (size_t) index) / 65536.0); };

        // measured: master gain = 4^x - 1 (x = 0.5 is unity); band output = 3.645x^2 + 1.785x (x = 1/3 is unity)
        const double masterDb = juce::jlimit (-24.0, 12.0, 20.0 * std::log10 (juce::jmax (1.0e-3, std::pow (4.0, knob (1)) - 1.0)));
        auto outputDb = [] (double x) { return juce::jlimit (-24.0, 15.0, 20.0 * std::log10 (juce::jmax (1.0e-3, 3.645 * x * x + 1.785 * x))); };
        // measured: threshold = -69 + 60x dB (RMS), ratio = 1 / (1 - x^0.35), knee knob has no audible effect
        auto thresholdDb = [] (double x) { return -69.0 + 60.0 * x; };
        auto ratioOf = [] (double x) { return juce::jlimit (1.0, 20.0, 1.0 / juce::jmax (0.05, 1.0 - std::pow (x, 0.35))); };

        // crossovers, measured -6 dB points
        // the high and low bands keep their own copy of each crossover (2-band presets collapse the mid band)
        const double xHigh = interpHz (b.i32 (5 * 4), { { 7000, 427 }, { 8500, 610 }, { 10000, 1049 }, { 11500, 1625 }, { 13000, 2418 },
                                                         { 14500, 3633 }, { 16000, 5846 }, { 17500, 9735 }, { 19000, 14678 } });
        const double xLow = interpHz (b.i32 (22 * 4), { { 3000, 93 }, { 5000, 196 }, { 6500, 340 }, { 8000, 633 }, { 9500, 1150 },
                                                        { 11000, 1871 }, { 12500, 2977 } });

        struct Band { const char* name; int state, out, thr, ratio, att, rel; };
        const Band bands[3] = { { "low", 21, 23, 24, 25, 26, 27 }, { "mid", 12, 15, 16, 17, 18, 19 }, { "high", 4, 6, 7, 8, 9, 10 } };
        constexpr double level = -16.0;   // a typical vocal / mix level the amounts are matched at
        double gr[3] = {}, maxGr = 0.0, outSum = 0.0;
        int strongest = -1, audible = 0;
        juce::StringArray muted;
        for (int i = 0; i < 3; ++i)
        {
            const int state = b.i32 (4 * (size_t) bands[i].state);
            if (state == 1) { muted.add (bands[i].name); continue; }
            outSum += outputDb (knob (bands[i].out));
            ++audible;
            if (state != 0) continue;
            gr[i] = staticGr (level, thresholdDb (knob (bands[i].thr)), ratioOf (knob (bands[i].ratio)), 10.0);
            if (gr[i] > maxGr) { maxGr = gr[i]; strongest = i; }
        }

        // capped: when FL mutes bands the remaining ones are often boosted a lot, and Prism applies it to everything
        const double makeup = juce::jlimit (-12.0, 12.0, masterDb + (audible > 0 ? outSum / audible : 0.0));
        dyn.setParam ("mode", 1.0f);
        dyn.setParam ("bands", 1.0f);
        dyn.setParam ("detect", 1.0f);
        dyn.setParam ("knee", 10.0f);
        dyn.setParam ("auto_mk", 0.0f);
        dyn.setParam ("x_low", (float) juce::jlimit (60.0, 1000.0, xLow));
        dyn.setParam ("x_high", (float) juce::jlimit (1000.0, 12000.0, xHigh));

        double thr = 0.0, ratio = 1.0;
        if (strongest >= 0)
        {
            const auto& s = bands[strongest];
            thr = thresholdDb (knob (s.thr));
            ratio = ratioOf (knob (s.ratio));
            dyn.setParam ("att", (float) juce::jlimit (0.05, 100.0, b.i32 (4 * (size_t) s.att) / 200.0));   // measured: 1000 = ~5 ms
            dyn.setParam ("rel", (float) juce::jlimit (5.0, 1500.0, b.i32 (4 * (size_t) s.rel) / 10.0));
        }
        dyn.setParam ("ratio", (float) ratio);
        // makeup after the compressor = the same gain before it with the threshold moved up by as much
        dyn.setParam ("gain", (float) makeup);
        dyn.setParam ("thresh", (float) juce::jlimit (-48.0, 0.0, thr + makeup));
        for (int i = 0; i < 3; ++i)
            dyn.setParam (i == 0 ? "amt_low" : i == 1 ? "amt_mid" : "amt_high", (float) (maxGr > 0.01 ? juce::jlimit (0.0, 100.0, 100.0 * gr[i] / maxGr) : 0.0));

        juce::String note;
        note << "crossovers " << juce::roundToInt (xLow) << " Hz / " << juce::String (xHigh / 1000.0, 1) << " kHz";
        if (strongest >= 0)
            note << ", threshold " << juce::roundToInt (thr) << " dB, ratio " << juce::String (ratio, 1) << ":1, band amounts matched";
        else
            note << ", no band compressing";
        note << ", gain " << juce::String (makeup, 1) << " dB";
        if (muted.size() > 0)
            note << "; muted " << muted.joinIntoString ("/") << " band not reproduced";
        return note;
    }

    // Maximus: version, then a header (low/mid crossover, flag, mid/high crossover, flag, ...) and four
    // 13-value blocks - low (8), mid (21), high (34), master (47): flag (1 = bypass, 2 = mute), pre, post,
    // pan, stereo, threshold, then attack / release / curve settings. Gains and thresholds use 1000 = 0 dB.
    juce::String translateMaximus (const Blob& b, AmethystPlugin& dyn)
    {
        if (! b.has (60 * 4)) return {};
        auto v = [&] (int index) { return b.i32 (4 * (size_t) index); };
        // measured on FL's renders: 750 = -4.4 dB, 500 = -10.5, 250 = -18.9, 1500 = +5
        auto db = [] (int raw) { return juce::jlimit (-60.0, 12.0, 12.16 * (1.0 - std::pow (1.365, (1000.0 - raw) / 250.0))); };

        const double xLow = interpHz (v (1), { { 10000, 100 }, { 15000, 167 }, { 19800, 243 }, { 25000, 452 }, { 30000, 917 }, { 36000, 1741 } });
        const double xHigh = interpHz (v (3), { { 30000, 420 }, { 36000, 754 }, { 42000, 1377 }, { 48065, 2578 }, { 55000, 5670 }, { 62000, 9886 } });

        // format 2 has one header value less, so its blocks start a place earlier
        const int base = (v (0) & 0xffff) <= 2 ? 7 : 8;
        const int blocks[3] = { base, base + 13, base + 26 };
        const int master = base + 39;
        double gainSum = 0.0;
        int active = 0;
        float amt[3] = {};
        juce::StringArray muted;
        for (int i = 0; i < 3; ++i)
        {
            const int flag = v (blocks[i]);
            if (flag == 2) { muted.add (i == 0 ? "low" : i == 1 ? "mid" : "high"); continue; }
            gainSum += db (v (blocks[i] + 1)) + db (v (blocks[i] + 2));
            ++active;
            amt[i] = flag == 0 ? 100.0f : 0.0f;
        }

        const double gain = juce::jlimit (-12.0, 24.0, db (v (master + 1)) + (active > 0 ? gainSum / active : 0.0));
        const double ceil = juce::jlimit (-24.0, -0.1, db (v (master + 5)) + db (v (master + 2)));
        dyn.setParam ("mode", 0.0f);
        dyn.setParam ("bands", 1.0f);
        dyn.setParam ("gain", (float) gain);
        dyn.setParam ("ceil", (float) ceil);
        dyn.setParam ("x_low", (float) juce::jlimit (60.0, 1000.0, xLow));
        dyn.setParam ("x_high", (float) juce::jlimit (1000.0, 12000.0, xHigh));
        dyn.setParam ("amt_low", amt[0]);
        dyn.setParam ("amt_mid", amt[1]);
        dyn.setParam ("amt_high", amt[2]);

        juce::String note;
        note << "crossovers " << juce::roundToInt (xLow) << " Hz / " << juce::String (xHigh / 1000.0, 1) << " kHz, drive "
             << juce::String (gain, 1) << " dB, ceiling " << juce::String (ceil, 1) << " dB; Maximus's drawn curves are approximated by limiting";
        if (muted.size() > 0)
            note << "; muted " << muted.joinIntoString ("/") << " band not reproduced";
        return note;
    }

    // FL's VST3 wrapper state: u32 1, u32 1, u32 headerSize (64), u32 0, then a header block, then
    // {u32 id, u64 size, payload} records: 3 = the plug-in's component state, 4 = its controller state.
    // Those are exactly what the VST3 SDK hands out, so we wrap them the way JUCE expects and load them.
    bool restoreVstState (const Flp::Plugin& p, juce::AudioPluginInstance& instance)
    {
        if (instance.getPluginDescription().pluginFormatName != "VST3" || p.vstState.getSize() < 32)
            return false;

        const auto* d = (const juce::uint8*) p.vstState.getData();
        const size_t n = p.vstState.getSize();
        auto rd32 = [d] (size_t o) { return (juce::uint32) d[o] | ((juce::uint32) d[o + 1] << 8) | ((juce::uint32) d[o + 2] << 16) | ((juce::uint32) d[o + 3] << 24); };
        auto rd64 = [&rd32] (size_t o) { return (juce::uint64) rd32 (o) | ((juce::uint64) rd32 (o + 4) << 32); };

        const auto headerSize = (size_t) rd32 (8);
        if (rd32 (0) != 1 || headerSize > n)
            return false;

        juce::MemoryBlock component, controller;
        size_t o = 16 + headerSize;
        while (o + 12 <= n)
        {
            const auto id = rd32 (o);
            const auto size = rd64 (o + 4);
            o += 12;
            if (size > n - o)
                return false;
            if (id == 3) component.replaceAll (d + o, (size_t) size);
            if (id == 4) controller.replaceAll (d + o, (size_t) size);
            o += (size_t) size;
        }
        if (component.isEmpty())
            return false;

        juce::XmlElement state ("VST3PluginState");
        state.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());
        if (! controller.isEmpty())
            state.createNewChildElement ("IEditController")->addTextElement (controller.toBase64Encoding());

        juce::MemoryBlock blob;
        juce::AudioProcessor::copyXmlToBinary (state, blob);
        instance.setStateInformation (blob.getData(), (int) blob.getSize());
        return true;
    }

    juce::String translateSettings (const Flp::Plugin& p, juce::AudioPluginInstance& instance, double bpm)
    {
        auto* am = dynamic_cast<AmethystPlugin*> (&instance);
        if (am == nullptr || p.state.isEmpty())
            return {};
        const Blob b { p.state };
        const auto& n = p.internalName;
        if (n.equalsIgnoreCase ("Fruity Parametric EQ 2")) return translateEq (b, *am);
        if (n.equalsIgnoreCase ("Fruity Reeverb 2"))       return translateReverb (b, *am);
        if (n.equalsIgnoreCase ("Fruity Compressor"))      return translateCompressor (b, *am);
        if (n.equalsIgnoreCase ("Fruity Limiter"))         return translateLimiter (b, *am);
        if (n.equalsIgnoreCase ("Soundgoodizer"))          return translateSoundgoodizer (b, *am);
        if (n.equalsIgnoreCase ("Fruity Delay 3"))         return translateDelay3 (b, *am, bpm);
        if (n.equalsIgnoreCase ("Fruity Flangus"))         return translateFlangus (b, *am);
        if (n.equalsIgnoreCase ("Pitcher"))                return translatePitcher (b, *am);
        if (n.equalsIgnoreCase ("Newtone"))                return translateNewtone (*am);
        if (n.equalsIgnoreCase ("Fruity Delay 2"))         return translateDelay2 (b, *am, bpm);
        if (n.equalsIgnoreCase ("Fruity Filter"))          return translateFilter (b, *am);
        if (n.equalsIgnoreCase ("Fruity Stereo Enhancer")) return translateStereoEnhancer (b, *am);
        if (n.equalsIgnoreCase ("Fruity Fast Dist"))       return translateFastDist (b, *am);
        if (n.equalsIgnoreCase ("Fruity Blood Overdrive")) return translateBloodOverdrive (b, *am);
        if (n.equalsIgnoreCase ("Fruity Soft Clipper"))    return translateSoftClipper (b, *am);
        if (n.equalsIgnoreCase ("Maximus"))                return translateMaximus (b, *am);
        if (n.equalsIgnoreCase ("Fruity Multiband Compressor")) return translateMultibandComp (b, *am);
        return {};
    }

    // Creates the AMETHYST version of one FL effect, with its settings. Adds a report line either way.
    std::unique_ptr<InsertSlot> buildSlot (AudioEngine& engine, const Flp::Plugin& p, const juce::String& place, double bpm, Report& report)
    {
        const auto label = (p.displayName.isNotEmpty() && p.displayName != p.getName() ? "\"" + p.displayName + "\" (" + p.getName() + ")" : p.getName());

        juce::PluginDescription desc;
        juce::String targetName, note;
        if (p.isWrapper())
        {
            if (! findVst (engine, p, desc))
            {
                report.notTranslated.add (label + " - plug-in not found (install it, then scan it in Plug-in Manager) [" + place + "]");
                return nullptr;
            }
            targetName = desc.name + " (" + desc.pluginFormatName + ")";
            note = "its FL settings aren't carried over yet";
        }
        else
        {
            const auto id = mapNativeEffect (p.internalName);
            if (id.isEmpty() || ! findBuiltin (id, desc))
            {
                report.notTranslated.add (label + " - no AMETHYST equivalent yet [" + place + "]");
                return nullptr;
            }
            targetName = desc.name;
            note = "default settings";
        }

        juce::String err;
        auto plugin = engine.createPlugin (desc, err);
        if (plugin == nullptr)
        {
            report.notTranslated.add (label + " -> " + targetName + " failed: " + (err.isNotEmpty() ? err : juce::String ("couldn't load")) + " [" + place + "]");
            return nullptr;
        }

        if (p.isWrapper())
        {
            if (restoreVstState (p, *plugin))
                note = "FL settings restored";
        }
        else if (auto translated = translateSettings (p, *plugin, bpm); translated.isNotEmpty())
            note = translated;

        auto slot = std::make_unique<InsertSlot>();
        slot->plugin = std::move (plugin);
        slot->bypassed = ! p.enabled;
        slot->mix = p.mix;
        report.translated.add (label + "  ->  " + targetName + "  (" + note + (p.enabled ? juce::String() : juce::String (", bypassed")) + ") [" + place + "]");
        return slot;
    }

    void applyInsert (AudioEngine& engine, Track& track, const Flp::Insert& ins, const juce::String& where, double bpm, Report& report,
                      bool setLevels = true)
    {
        std::vector<std::unique_ptr<InsertSlot>> slots ((size_t) Track::maxInserts);
        int nextFree = 0;

        for (auto& p : ins.plugins)
        {
            int index = p.slot;
            if (index < 0 || index >= Track::maxInserts || slots[(size_t) index] != nullptr)
            {
                while (nextFree < Track::maxInserts && slots[(size_t) nextFree] != nullptr) ++nextFree;
                index = nextFree;
            }
            if (index >= Track::maxInserts)
            {
                report.notTranslated.add (p.getName() + " - no free slot on " + where);
                continue;
            }
            if (auto slot = buildSlot (engine, p, where + ", slot " + juce::String (index + 1), bpm, report))
                slots[(size_t) index] = std::move (slot);
        }

        if (setLevels)
        {
            track.volumeDb = gainToDb (ins.volume);
            track.pan = ins.pan;
        }
        engine.replaceInserts (track, std::move (slots)).clear();
    }

    std::shared_ptr<AudioClip> cloneClip (const AudioClip& src, juce::int64 start)
    {
        auto c = std::make_shared<AudioClip>();
        c->file = src.file;
        c->lossless = src.lossless;
        c->source = src.source;
        c->buffer = src.source;
        c->startSample = start;
        return c;
    }
}

juce::String mapNativeEffect (const juce::String& flName)
{
    for (auto& m : kMappings)
        if (flName.equalsIgnoreCase (m.fl))
            return m.id;
    return {};
}

juce::String Report::toText() const
{
    juce::String s;
    if (presetTarget.isNotEmpty())
        s << "Loaded FL Studio preset \"" << projectName << "\" (saved in FL Studio " << flVersion << ") onto " << presetTarget << "\n";
    else
        s << "Imported \"" << projectName << "\" (FL Studio " << flVersion << ", " << juce::String (tempo, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") << " BPM)\n"
          << tracksCreated << " tracks, " << clipsImported << " audio clips\n";
    auto section = [&s] (const juce::String& title, const juce::StringArray& lines)
    {
        if (lines.isEmpty()) return;
        s << "\n" << title << " (" << lines.size() << ")\n";
        for (auto& l : lines) s << "  - " << l << "\n";
    };
    section ("Effects brought over", translated);
    section ("Effects not brought over", notTranslated);
    section ("Missing audio files", missingAudio);
    section ("Not imported", skipped);
    return s;
}

bool isFlPreset (const juce::File& f)
{
    return f.hasFileExtension ("fst");
}

juce::Result importPreset (AudioEngine& engine, Track& track, const juce::File& file, Report& report)
{
    report = Report();
    Flp::Preset preset;
    auto r = Flp::readPreset (file, preset);
    if (r.failed())
        return r;

    report.projectName = file.getFileNameWithoutExtension();
    report.flVersion = preset.version.isNotEmpty() ? preset.version : juce::String ("(unknown)");
    report.tempo = engine.bpm.load();
    report.presetTarget = track.isMaster ? juce::String ("Master") : track.name;
    const double bpm = engine.bpm.load();

    if (preset.isMixerState)
    {
        // a whole mixer track state replaces the chain, like "Load mixer track state" in FL
        applyInsert (engine, track, preset.insert, report.presetTarget, bpm, report, preset.hasLevels);
        if (preset.insert.name.isNotEmpty() && ! track.isMaster)
            report.skipped.add ("FL track name \"" + preset.insert.name + "\" (the channel keeps its own name)");
        return juce::Result::ok();
    }

    // one effect's preset: add it to the first free slot, keep the rest of the chain
    int index = -1;
    for (int i = 0; i < (int) track.inserts.size(); ++i)
        if (track.inserts[(size_t) i] == nullptr || track.inserts[(size_t) i]->plugin == nullptr) { index = i; break; }
    if (index < 0)
        return juce::Result::fail ("All " + juce::String (Track::maxInserts) + " effect slots on " + report.presetTarget + " are full.");

    auto slot = buildSlot (engine, preset.insert.plugins.front(), report.presetTarget + ", slot " + juce::String (index + 1), bpm, report);
    if (slot == nullptr)
        return juce::Result::ok();   // the report says why
    const bool bypassed = slot->bypassed.load();
    const float mix = slot->mix.load();
    engine.insertPlugin (track, index, std::move (slot->plugin)).reset();
    track.inserts[(size_t) index]->bypassed = bypassed;
    track.inserts[(size_t) index]->mix = mix;
    return juce::Result::ok();
}

juce::Result importProject (AudioEngine& engine, const juce::File& flpFile, Report& report)
{
    report = Report();
    Flp::Project proj;
    auto r = Flp::read (flpFile, proj);
    if (r.failed())
        return r;

    report.projectName = flpFile.getFileNameWithoutExtension();
    report.flVersion = proj.version.isNotEmpty() ? proj.version : juce::String ("(unknown)");
    report.tempo = proj.tempo;

    engine.stop();
    engine.clearTracks();
    engine.setTransportPosition (0);
    engine.bpm = proj.tempo;

    auto& master = engine.getMaster();
    if (auto* m = proj.getInsert (0))
        applyInsert (engine, master, *m, "Master", proj.tempo, report);
    else
        engine.replaceInserts (master, {}).clear();

    const double sr = engine.getSampleRate() > 0 ? engine.getSampleRate() : 48000.0;

    // Group playlist audio clips by (mixer insert, playlist track).
    struct Group { int insert; int track; std::vector<const Flp::PlaylistItem*> items; };
    std::map<std::pair<int, int>, Group> groups;
    int patternItems = 0;
    for (auto& it : proj.items)
    {
        if (it.channel < 0) { ++patternItems; continue; }
        auto ch = proj.channels.find (it.channel);
        if (ch == proj.channels.end() || ch->second.samplePath.isEmpty())
        {
            ++patternItems; // automation clips and similar
            continue;
        }
        const int insert = juce::jmax (0, ch->second.insert);
        auto& g = groups[{ insert, it.track }];
        g.insert = insert;
        g.track = it.track;
        g.items.push_back (&it);
    }

    // Inserts that carry effects or a name get a channel even without clips, so the vocal chain is there to record on.
    std::set<int> channelInserts;
    for (auto& [key, g] : groups)
        if (g.insert > 0) channelInserts.insert (g.insert);
    for (auto& ins : proj.inserts)
        if (ins.index > 0 && (! ins.plugins.empty() || ins.name.isNotEmpty()))
            channelInserts.insert (ins.index);

    std::map<juce::String, std::shared_ptr<AudioClip>> loaded;   // decode each file once
    std::map<int, Track*> channelFor;

    auto trackName = [&proj] (int track) -> juce::String
    {
        auto n = proj.trackNames.find (track);
        return n != proj.trackNames.end() && n->second.isNotEmpty() ? n->second : juce::String ("Track ") + juce::String (track + 1);
    };

    auto addClips = [&] (Track& t, const Group& g)
    {
        for (auto* it : g.items)
        {
            const auto& ch = proj.channels[it->channel];
            const auto file = Flp::resolveSamplePath (ch.samplePath, flpFile);
            if (! file.existsAsFile())
            {
                report.missingAudio.addIfNotAlreadyThere (ch.samplePath);
                continue;
            }
            const auto start = (juce::int64) std::llround (proj.ticksToSeconds (it->position) * sr);
            auto key = file.getFullPathName();
            std::shared_ptr<AudioClip> clip;
            if (auto cached = loaded.find (key); cached != loaded.end())
                clip = cached->second ? cloneClip (*cached->second, start) : nullptr;
            else
            {
                clip = engine.loadClip (file, start);
                loaded[key] = clip;
                if (clip != nullptr) clip = cloneClip (*clip, start);
            }
            if (clip == nullptr)
            {
                report.missingAudio.addIfNotAlreadyThere (file.getFullPathName() + " (unreadable)");
                continue;
            }

            if (it->startOffsetMs > 0.0)
                clip->trimOffset = (juce::int64) std::llround (it->startOffsetMs / 1000.0 * sr);
            if (it->length > 0)
                clip->trimLength = juce::jmax<juce::int64> (AudioEngine::minClipSamples, (juce::int64) std::llround (proj.ticksToSeconds (it->length) * sr));
            clip->enabled = ! it->muted;

            if (std::abs (ch.volume - 1.0f) > 0.01f) clip->settings.gainDb = gainToDb (ch.volume);
            if (std::abs (ch.pan) > 0.01f)           clip->settings.pan = ch.pan;
            if (! clip->settings.isNeutral())
                engine.renderNow (*clip);

            engine.addClip (t, std::move (clip));
            ++report.clipsImported;
        }
    };

    // 1) one channel per used insert (its effects, fader and pan)
    for (int index : channelInserts)
    {
        const auto* ins = proj.getInsert (index);
        juce::String name = ins != nullptr && ins->name.isNotEmpty() ? ins->name : juce::String ("Insert ") + juce::String (index);
        auto* t = engine.addTrack (name);
        ++report.tracksCreated;
        channelFor[index] = t;
        if (ins != nullptr)
            applyInsert (engine, *t, *ins, name, proj.tempo, report);
    }

    // 2) clips: the first playlist track of an insert goes on its channel, others become take lanes playing through it
    std::set<int> channelHasClips;
    for (auto& [key, g] : groups)
    {
        if (g.insert > 0)
        {
            auto* channel = channelFor[g.insert];
            if (channelHasClips.insert (g.insert).second)
            {
                addClips (*channel, g);
                continue;
            }
            auto* lane = engine.addTrack (trackName (g.track));
            ++report.tracksCreated;
            addClips (*lane, g);
            engine.setPlayThrough (*lane, channel);
        }
        else
        {
            auto* t = engine.addTrack (trackName (g.track));
            ++report.tracksCreated;
            addClips (*t, g);
        }
    }

    if (patternItems > 0)
        report.skipped.add (juce::String (patternItems) + " pattern / automation clips (MIDI notes and instruments aren't imported)");
    int samplerChannels = 0;
    for (auto& [iid, ch] : proj.channels)
        if (ch.samplePath.isNotEmpty() && std::none_of (proj.items.begin(), proj.items.end(), [iid = iid] (auto& it) { return it.channel == iid; }))
            ++samplerChannels;
    if (samplerChannels > 0)
        report.skipped.add (juce::String (samplerChannels) + " sampler channels used only in patterns");

    return juce::Result::ok();
}
}
