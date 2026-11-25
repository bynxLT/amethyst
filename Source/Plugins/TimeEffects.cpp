#include "Builtins.h"

namespace
{
    // Fractional-delay ring buffer.
    struct DelayLine
    {
        std::vector<float> buf;
        int w = 0;

        void setMax (int samples) { buf.assign ((size_t) juce::jmax (4, samples), 0.0f); w = 0; }
        void push (float x) { buf[(size_t) w] = x; w = (w + 1) % (int) buf.size(); }
        float read (double delaySamples) const
        {
            const int len = (int) buf.size();
            delaySamples = juce::jlimit (1.0, (double) len - 2.0, delaySamples);
            double pos = w - 1 - delaySamples;
            while (pos < 0) pos += len;
            const int i0 = (int) pos;
            const float f = (float) (pos - i0);
            const float a = buf[(size_t) i0], b = buf[(size_t) ((i0 + 1) % len)];
            return a + (b - a) * f;
        }
    };

    //==========================================================================
    // Crystal Echo: tempo-synced stereo delay with four characters (Clean, Tape, Lo-fi, Diffuse),
    // wow / flutter, a band-limited feedback path with drive, and ducking while the dry signal plays.
    class EchoPlugin : public AmethystPlugin
    {
    public:
        EchoPlugin() : AmethystPlugin ("amethyst.echo", "Crystal Echo", "Delay", "Tempo-synced character delay", makeLayout())
        {
            tailSeconds = 8.0;
            sections = {
                { "Character", { "style", "mod", "drive" }, 0 },
                { "Time", { "sync", "div", "time", "offset" }, 0 },
                { "Feedback", { "fb_mode", "fb", "fb_low", "fb_cut" }, 1 },
                { "Input", { "in_pan", "in_vol" }, 1 },
                { "Mix", { "duck", "dry", "wet" }, 1 },
            };

            presets = {
                { "Default", {} },
                { "Slapback",       { { "sync", 0 }, { "time", 95 }, { "fb", 10 }, { "wet", 30 } } },
                { "Vocal Quarter",  { { "div", 8 }, { "fb", 30 }, { "wet", 22 }, { "fb_cut", 5000 }, { "fb_low", 250 }, { "duck", 40 } } },
                { "Dotted Eighth",  { { "div", 6 }, { "fb", 35 }, { "wet", 25 }, { "duck", 30 } } },
                { "Ping-pong",      { { "fb_mode", 2 }, { "div", 5 }, { "fb", 45 }, { "wet", 30 } } },
                { "Wide Doubler",   { { "sync", 0 }, { "time", 18 }, { "offset", 100 }, { "fb", 0 }, { "wet", 50 } } },
                { "Tape Echo",      { { "style", 1 }, { "div", 6 }, { "fb", 50 }, { "mod", 35 }, { "drive", 40 }, { "fb_cut", 4000 }, { "wet", 30 } } },
                { "Dub Echo",       { { "style", 1 }, { "div", 9 }, { "fb", 72 }, { "fb_cut", 2500 }, { "fb_low", 200 }, { "drive", 55 }, { "wet", 40 } } },
                { "Lo-fi Phone",    { { "style", 2 }, { "div", 5 }, { "fb", 40 }, { "fb_low", 400 }, { "fb_cut", 3000 }, { "wet", 35 } } },
                { "Diffuse Cloud",  { { "style", 3 }, { "div", 8 }, { "fb", 60 }, { "mod", 20 }, { "wet", 35 }, { "duck", 50 } } },
                { "Ducked Vocal Throw", { { "div", 8 }, { "fb", 45 }, { "fb_low", 300 }, { "fb_cut", 6000 }, { "wet", 40 }, { "duck", 80 } } },
                { "Weird",          { { "fb_mode", 1 }, { "div", 1 }, { "fb", 80 }, { "offset", 60 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("in_pan", "Pan", -100.0f, 100.0f, 0.0f, {}, 0));
            l.add (AmParams::num ("in_vol", "Volume", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::choice ("fb_mode", "Mode", { "Normal", "Invert", "Ping-pong" }, 0));
            l.add (AmParams::num ("fb", "Amount", 0.0f, 95.0f, 40.0f, "%", 0));
            l.add (AmParams::num ("fb_cut", "High cut", 500.0f, 20000.0f, 7000.0f, "Hz", 0, 4000.0f));
            l.add (AmParams::num ("fb_low", "Low cut", 20.0f, 1000.0f, 20.0f, "Hz", 0, 150.0f));
            l.add (AmParams::toggle ("sync", "Sync", true));
            l.add (AmParams::choice ("div", "Division", { "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1" }, 6));
            l.add (AmParams::num ("time", "Time", 1.0f, 2000.0f, 350.0f, "ms", 0, 300.0f));
            l.add (AmParams::num ("offset", "Offset", -100.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::choice ("style", "Style", { "Clean", "Tape", "Lo-fi", "Diffuse" }, 0));
            l.add (AmParams::num ("mod", "Wow", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("duck", "Duck", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("dry", "Dry", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("wet", "Wet", 0.0f, 100.0f, 35.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            for (auto& d : lines) d.setMax ((int) (sr * 4.5));
            for (auto& s : smooth) s.reset (sr, 0.12);
            for (auto& a : smear) a.setMax ((int) (sr * 0.03));
            lp.fill (0.0f);
            hp.fill (0.0f);
            hold.fill (0.0f);
            holdCount = 0;
            wow = flutter = 0.0;
            duckEnv = 0.0f;
            first = true;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());

            static constexpr float beats[] = { 0.125f, 1.0f / 6.0f, 0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f, 2.0f / 3.0f, 1.0f, 1.5f, 2.0f, 4.0f };
            double ms = getBool ("sync") ? beats[juce::jlimit (0, 11, getInt ("div"))] * 60000.0 / getHostBpm() : get ("time");
            ms = juce::jmin (ms, 4000.0);
            const float off = get ("offset") * 0.01f;
            const double dl = ms * (1.0 - off * 0.1) * 0.001 * sampleRate, dr = ms * (1.0 + off * 0.1) * 0.001 * sampleRate;
            if (first) { smooth[0].setCurrentAndTargetValue (dl); smooth[1].setCurrentAndTargetValue (dr); first = false; }
            smooth[0].setTargetValue (dl);
            smooth[1].setTargetValue (dr);

            const float panAngle = (get ("in_pan") * 0.01f + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            const float inVol = get ("in_vol") * 0.01f;
            const float gl = inVol * std::cos (panAngle) * juce::MathConstants<float>::sqrt2, gr = inVol * std::sin (panAngle) * juce::MathConstants<float>::sqrt2;
            const int mode = getInt ("fb_mode");
            const int style = getInt ("style");
            const float fb = get ("fb") * 0.01f * (mode == 1 ? -1.0f : 1.0f);
            const float lpc = (float) std::exp (-juce::MathConstants<double>::twoPi * (style == 2 ? juce::jmin (get ("fb_cut"), 3500.0f) : get ("fb_cut")) / sampleRate);
            const float hpc = (float) std::exp (-juce::MathConstants<double>::twoPi * (style == 2 ? juce::jmax (get ("fb_low"), 300.0f) : get ("fb_low")) / sampleRate);
            const float dry = get ("dry") * 0.01f, wet = get ("wet") * 0.01f;
            const float drive = get ("drive") * 0.01f + (style == 1 ? 0.25f : 0.0f);
            const float driveK = 1.0f + 3.0f * drive, driveNorm = std::tanh (driveK);
            const double modDepth = get ("mod") * 0.01 * sampleRate * (style == 1 ? 0.004 : 0.0015) + (style == 1 ? sampleRate * 0.0004 : 0.0);
            const double wowInc = juce::MathConstants<double>::twoPi * 0.55 / sampleRate, flutterInc = juce::MathConstants<double>::twoPi * 6.3 / sampleRate;
            const float duck = get ("duck") * 0.01f;
            const float duckAtt = coef (6.0, sampleRate), duckRel = coef (300.0, sampleRate);
            const int smearA = (int) (0.0071 * sampleRate), smearB = (int) (0.0113 * sampleRate);
            const int crush = juce::jmax (1, (int) std::lround (sampleRate / 11025.0));

            for (int i = 0; i < n; ++i)
            {
                const float xl = buffer.getSample (0, i), xr = nCh > 1 ? buffer.getSample (1, i) : xl;

                // wow + flutter move the read heads
                const double m = modDepth > 0.0 ? modDepth * (0.75 * (1.0 + std::sin (wow)) + 0.25 * (1.0 + std::sin (flutter))) : 0.0;
                wow += wowInc; flutter += flutterInc;
                float y[2] = { lines[0].read (smooth[0].getNextValue() + m), lines[1].read (smooth[1].getNextValue() + m * 0.9) };

                // character on the echoes
                for (int c = 0; c < 2; ++c)
                {
                    lp[(size_t) c] = (1.0f - lpc) * y[c] + lpc * lp[(size_t) c];
                    hp[(size_t) c] = hpc * hp[(size_t) c] + (1.0f - hpc) * lp[(size_t) c];
                    float v = lp[(size_t) c] - hp[(size_t) c];
                    if (drive > 0.001f)
                        v = v + drive * (std::tanh (driveK * v) / driveNorm - v);
                    if (style == 2)   // lo-fi: lower sample rate + 8-bit
                    {
                        if (c == 0 && ++holdCount >= crush) holdCount = 0;
                        if (holdCount == 0) hold[(size_t) c] = std::round (v * 127.0f) / 127.0f;
                        v = hold[(size_t) c];
                    }
                    else if (style == 3)   // diffuse: smear each repeat
                    {
                        v = smear[(size_t) c * 2].process (v, smearA + c * 37, 0.6f);
                        v = smear[(size_t) c * 2 + 1].process (v, smearB + c * 53, 0.55f);
                    }
                    y[c] = v;
                }

                const float il = xl * gl, ir = xr * gr;
                if (mode == 2)
                {
                    lines[0].push ((il + ir) * 0.5f + y[1] * fb);
                    lines[1].push (y[0] * fb);
                }
                else
                {
                    lines[0].push (il + y[0] * fb);
                    lines[1].push (ir + y[1] * fb);
                }

                // ducking: echoes dip while you sing, bloom in the gaps
                float duckGain = 1.0f;
                if (duck > 0.0f)
                {
                    const float lvl = juce::jmax (std::abs (xl), std::abs (xr));
                    duckEnv = lvl > duckEnv ? duckAtt * duckEnv + (1.0f - duckAtt) * lvl : duckRel * duckEnv + (1.0f - duckRel) * lvl;
                    const float amount = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (duckEnv, -100.0f) + 45.0f) / 30.0f);
                    duckGain = juce::Decibels::decibelsToGain (-20.0f * duck * amount);
                }

                buffer.setSample (0, i, xl * dry + y[0] * wet * duckGain);
                if (nCh > 1) buffer.setSample (1, i, xr * dry + y[1] * wet * duckGain);
            }
            if (wow > 1000.0) wow = std::fmod (wow, juce::MathConstants<double>::twoPi);
            if (flutter > 1000.0) flutter = std::fmod (flutter, juce::MathConstants<double>::twoPi);
        }

    private:
        struct Smear
        {
            std::vector<float> buf;
            int w = 0;
            void setMax (int n) { buf.assign ((size_t) juce::jmax (4, n), 0.0f); w = 0; }
            float process (float x, int delay, float g)
            {
                delay = juce::jlimit (1, (int) buf.size() - 1, delay);
                int r = w - delay;
                if (r < 0) r += (int) buf.size();
                const float d = buf[(size_t) r];
                const float v = x + g * d;
                buf[(size_t) w] = v;
                if (++w >= (int) buf.size()) w = 0;
                return d - g * v;
            }
        };

        static float coef (double timeMs, double sr) { return (float) std::exp (-1.0 / (juce::jmax (0.01, timeMs) * 0.001 * sr)); }

        std::array<DelayLine, 2> lines;
        std::array<juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>, 2> smooth;
        std::array<Smear, 4> smear;
        std::array<float, 2> lp {}, hp {}, hold {};
        int holdCount = 0;
        double wow = 0.0, flutter = 0.0;
        float duckEnv = 0.0f;
        bool first = true;
    };

    //==========================================================================
    class PrismPlugin : public AmethystPlugin
    {
    public:
        PrismPlugin() : AmethystPlugin ("amethyst.prism", "Prism Mod", "Modulation", "Flanger / chorus / vibrato", makeLayout())
        {
            sections = {
                { "Character", { "mode", "delay", "depth", "speed" }, 0 },
                { "Stereo", { "spread", "fb" }, 0 },
                { "Mix", { "dry", "wet" }, 0 },
            };

            presets = {
                { "Default", {} },
                { "Classic Flanger", { { "delay", 2.5f }, { "depth", 70 }, { "speed", 0.25f }, { "fb", 50 } } },
                { "Fast Flanger",    { { "delay", 1.5f }, { "depth", 60 }, { "speed", 3.0f }, { "fb", 40 } } },
                { "Jet",             { { "delay", 1.0f }, { "depth", 90 }, { "speed", 0.12f }, { "fb", 85 } } },
                { "Deep Chorus",     { { "mode", 1 }, { "delay", 18 }, { "depth", 60 }, { "speed", 0.4f }, { "spread", 100 } } },
                { "Thick",           { { "mode", 1 }, { "delay", 25 }, { "depth", 35 }, { "speed", 0.8f }, { "wet", 60 } } },
                { "Stereoized",      { { "mode", 1 }, { "delay", 12 }, { "depth", 20 }, { "speed", 0.3f }, { "spread", 100 }, { "wet", 50 } } },
                { "Surround Flange", { { "delay", 3.0f }, { "depth", 50 }, { "speed", 0.2f }, { "spread", 100 }, { "fb", -60 } } },
                { "Vibrato",         { { "mode", 2 }, { "delay", 3.0f }, { "depth", 30 }, { "speed", 5.0f } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("mode", "Mode", { "Flanger", "Chorus", "Vibrato" }, 0));
            l.add (AmParams::num ("delay", "Delay", 0.1f, 30.0f, 2.0f, "ms", 1, 5.0f));
            l.add (AmParams::num ("depth", "Depth", 0.0f, 100.0f, 50.0f, "%", 0));
            l.add (AmParams::num ("speed", "Speed", 0.02f, 10.0f, 0.5f, "Hz", 2, 1.0f));
            l.add (AmParams::num ("spread", "Spread", 0.0f, 100.0f, 50.0f, "%", 0));
            l.add (AmParams::num ("fb", "Feedback", -95.0f, 95.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("dry", "Dry", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("wet", "Wet", 0.0f, 100.0f, 70.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            for (auto& d : lines) d.setMax ((int) (sr * 0.08));
            phase = 0.0;
            fbL = fbR = 0.0f;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const int mode = getInt ("mode");
            const double base = get ("delay") * 0.001 * sampleRate;
            const double depth = get ("depth") * 0.01;
            const double inc = juce::MathConstants<double>::twoPi * get ("speed") / sampleRate;
            const double spread = get ("spread") * 0.01 * juce::MathConstants<double>::pi;
            const float fb = mode == 2 ? 0.0f : get ("fb") * 0.01f;
            const float dry = mode == 2 ? 0.0f : get ("dry") * 0.01f;
            const float wet = (mode == 2 ? 1.0f : get ("wet") * 0.01f) * (1.0f - 0.45f * std::abs (fb)); // tame feedback resonance
            const int voices = mode == 1 ? 3 : 1;

            for (int i = 0; i < n; ++i)
            {
                const float xl = buffer.getSample (0, i), xr = nCh > 1 ? buffer.getSample (1, i) : xl;
                lines[0].push (xl + fbL * fb);
                lines[1].push (xr + fbR * fb);

                float yl = 0.0f, yr = 0.0f;
                for (int v = 0; v < voices; ++v)
                {
                    const double vp = phase + v * juce::MathConstants<double>::twoPi / 3.0;
                    const double dL = base * (1.0 + depth * 0.9 * std::sin (vp));
                    const double dR = base * (1.0 + depth * 0.9 * std::sin (vp + spread));
                    yl += lines[0].read (juce::jmax (1.0, dL));
                    yr += lines[1].read (juce::jmax (1.0, dR));
                }
                yl /= (float) voices;
                yr /= (float) voices;
                fbL = yl; fbR = yr;

                buffer.setSample (0, i, xl * dry + yl * wet);
                if (nCh > 1) buffer.setSample (1, i, xr * dry + yr * wet);

                phase += inc;
                if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            }
        }

    private:
        std::array<DelayLine, 2> lines;
        double phase = 0.0;
        float fbL = 0.0f, fbR = 0.0f;
    };

    //==========================================================================
    // One-knob enhancer: 3-band compression + saturation with four flavours.
    class ShinePlugin : public AmethystPlugin
    {
    public:
        ShinePlugin() : AmethystPlugin ("amethyst.shine", "Lustre", "Mastering", "One-knob shine & loudness", makeLayout())
        {
            sections = { { "Flavour", { "mode" }, 0 } };
            presets = {
                { "Default", {} },
                { "Gentle Shine", { { "amount", 30 } } },
                { "Vocal Gloss",  { { "mode", 2 }, { "amount", 45 } } },
                { "Warm Body",    { { "mode", 1 }, { "amount", 40 } } },
                { "Loud Mix",     { { "mode", 3 }, { "amount", 60 } } },
                { "Full Send",    { { "mode", 3 }, { "amount", 100 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("amount", "Amount", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::choice ("mode", "Flavour", { "A", "B", "C", "D" }, 0));
            return l;
        }

        void prepareToPlay (double sr, int bs) override
        {
            sampleRate = sr;
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, bs), 2 };
            for (auto* f : { &split1, &split2 })
            {
                f->prepare (spec);
                f->setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
            }
            split1.setCutoffFrequency (180.0f);
            split2.setCutoffFrequency (3500.0f);
            env.fill (0.0f);
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const float amt = get ("amount") * 0.01f;
            if (amt < 0.001f)
                return;

            struct Flavour { float drive, ratio, thrOff, gainDb; };
            static constexpr Flavour table[4][3] = {
                { { 1.0f, 3.0f, 0.0f, 1.0f },  { 1.0f, 3.0f, 0.0f, 0.0f },  { 1.0f, 3.0f, 0.0f, 1.5f } },   // A balanced
                { { 1.6f, 4.0f, -3.0f, 3.0f }, { 1.2f, 3.0f, 0.0f, 0.0f },  { 0.8f, 2.0f, 0.0f, -1.0f } },  // B warm
                { { 0.9f, 2.5f, 0.0f, 0.0f },  { 1.1f, 3.0f, 0.0f, 0.5f },  { 1.5f, 4.0f, -2.0f, 4.0f } },  // C bright
                { { 1.8f, 6.0f, -6.0f, 2.0f }, { 1.8f, 6.0f, -6.0f, 1.0f }, { 1.8f, 6.0f, -6.0f, 2.0f } },  // D loud
            };
            const auto& fl = table[juce::jlimit (0, 3, getInt ("mode"))];

            float thr[3], slope[3], makeup[3], drive[3], driveNorm[3];
            for (int b = 0; b < 3; ++b)
            {
                thr[b] = -6.0f - 24.0f * amt + fl[b].thrOff * amt;
                const float ratio = 1.0f + (fl[b].ratio - 1.0f) * amt;
                slope[b] = 1.0f - 1.0f / ratio;
                makeup[b] = juce::Decibels::decibelsToGain (-thr[b] * slope[b] * 0.5f + fl[b].gainDb * amt);
                drive[b] = 1.0f + (fl[b].drive - 1.0f + 1.5f * amt) * amt;
                driveNorm[b] = std::tanh (drive[b]);
            }

            const float att = (float) std::exp (-1.0 / (0.010 * sampleRate)), rel = (float) std::exp (-1.0 / (0.120 * sampleRate));
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const float ceiling = juce::Decibels::decibelsToGain (-0.5f);

            for (int i = 0; i < n; ++i)
            {
                float bands[2][3];
                float level[3] = { 0, 0, 0 };
                for (int c = 0; c < nCh; ++c)
                {
                    float low, rest, mid, high;
                    split1.processSample (c, buffer.getSample (c, i), low, rest);
                    split2.processSample (c, rest, mid, high);
                    bands[c][0] = low; bands[c][1] = mid; bands[c][2] = high;
                    for (int b = 0; b < 3; ++b)
                        level[b] = juce::jmax (level[b], std::abs (bands[c][b]));
                }

                float gain[3];
                for (int b = 0; b < 3; ++b)
                {
                    env[(size_t) b] = level[b] > env[(size_t) b] ? att * env[(size_t) b] + (1.0f - att) * level[b] : rel * env[(size_t) b] + (1.0f - rel) * level[b];
                    const float over = juce::Decibels::gainToDecibels (env[(size_t) b], -120.0f) - thr[b];
                    gain[b] = juce::Decibels::decibelsToGain (-juce::jmax (0.0f, over) * slope[b]) * makeup[b];
                }

                for (int c = 0; c < nCh; ++c)
                {
                    const float dryS = buffer.getSample (c, i);
                    float sum = 0.0f;
                    for (int b = 0; b < 3; ++b)
                    {
                        const float x = bands[c][b] * gain[b];
                        sum += x + amt * (std::tanh (drive[b] * x) / driveNorm[b] - x);
                    }
                    const float out = dryS + amt * (sum - dryS) / juce::jmax (amt, 0.0001f) * juce::jmin (1.0f, amt * 4.0f);
                    buffer.setSample (c, i, softCeiling (out, ceiling));
                }
            }
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 250; }
        int getMinimumEditorWidth() const override { return 300; }

    private:
        juce::dsp::LinkwitzRileyFilter<float> split1, split2;
        std::array<float, 3> env {};
    };

    // Big glass dial with a glowing segment ring.
    class LustreDial : public juce::Component, private juce::Timer
    {
    public:
        explicit LustreDial (ShinePlugin& p) : plugin (p), attachment (p.apvts, "amount", slider)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
            slider.setMouseDragSensitivity (300);
            slider.setAlpha (0.0f); // invisible hit area; we draw the dial ourselves
            slider.setWantsKeyboardFocus (false);
            addAndMakeVisible (slider);
            startTimerHz (30);
        }

        void resized() override { slider.setBounds (dialBounds().toNearestInt()); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            Glass::well (g, r, 14.0f);
            const auto d = dialBounds();
            const auto c = d.getCentre();
            const float outer = d.getWidth() * 0.5f;
            const float amt = (float) slider.valueToProportionOfLength (slider.getValue());

            constexpr int segs = 36;
            const float a0 = juce::MathConstants<float>::pi * 1.2f, a1 = juce::MathConstants<float>::pi * 2.8f;
            for (int i = 0; i < segs; ++i)
            {
                const float t = (float) i / (segs - 1);
                const float a = a0 + (a1 - a0) * t;
                const bool lit = t <= amt + 0.001f && amt > 0.001f;
                const auto p1 = c.getPointOnCircumference (outer * 0.82f, a), p2 = c.getPointOnCircumference (outer * 0.98f, a);
                const auto col = Theme::accent.interpolatedWith (Theme::accent2, t);
                if (lit)
                {
                    g.setColour (col.withAlpha (0.35f * (0.6f + 0.4f * pulse)));
                    g.drawLine ({ p1, p2 }, 7.0f);
                }
                g.setColour (lit ? col : juce::Colours::white.withAlpha (0.08f));
                g.drawLine ({ p1, p2 }, 3.0f);
            }

            const auto knob = d.reduced (outer * 0.26f);
            g.setColour (Theme::accent.withAlpha (0.15f + 0.25f * amt));
            g.fillEllipse (knob.expanded (6.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a3477), knob.getCentreX(), knob.getY(),
                                                     juce::Colour (0xff120a22), knob.getCentreX(), knob.getBottom(), false));
            g.fillEllipse (knob);
            for (int ring = 1; ring < 9; ++ring)
            {
                g.setColour (juce::Colours::white.withAlpha (0.025f));
                g.drawEllipse (knob.reduced ((float) ring * knob.getWidth() * 0.05f), 1.0f);
            }
            auto sheen = knob.reduced (knob.getWidth() * 0.1f).withHeight (knob.getHeight() * 0.45f);
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.3f), sheen.getCentreX(), sheen.getY(),
                                                     juce::Colours::white.withAlpha (0.0f), sheen.getCentreX(), sheen.getBottom(), false));
            g.fillEllipse (sheen);
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.drawEllipse (knob, 1.2f);

            const float a = a0 + (a1 - a0) * amt;
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (c.getPointOnCircumference (knob.getWidth() * 0.36f, a)));

            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
            g.drawText (juce::String (juce::roundToInt (amt * 100.0f)) + "%", knob.toNearestInt(), juce::Justification::centred);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("DRAG TO SHINE", r.reduced (10.0f).toNearestInt(), juce::Justification::centredBottom);
        }

    private:
        juce::Rectangle<float> dialBounds() const
        {
            const auto r = getLocalBounds().toFloat().reduced (20.0f, 16.0f).withTrimmedBottom (14.0f);
            const float s = juce::jmin (r.getWidth(), r.getHeight());
            return juce::Rectangle<float> (s, s).withCentre (r.getCentre());
        }

        void timerCallback() override
        {
            pulse = 0.5f + 0.5f * std::sin ((float) juce::Time::getMillisecondCounter() * 0.004f);
            repaint();
        }

        ShinePlugin& plugin;
        juce::Slider slider;
        juce::AudioProcessorValueTreeState::SliderAttachment attachment;
        float pulse = 0.0f;
    };

    std::unique_ptr<juce::Component> ShinePlugin::createVisual() { return std::make_unique<LustreDial> (*this); }
}

std::unique_ptr<AmethystPlugin> Builtins::makeEcho()  { return std::make_unique<EchoPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makePrism() { return std::make_unique<PrismPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeShine() { return std::make_unique<ShinePlugin>(); }
