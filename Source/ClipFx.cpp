#include "ClipFx.h"

bool ClipSettings::operator== (const ClipSettings& o) const
{
   #define X(name, def) if (std::abs (name - o.name) > 1.0e-6f) return false;
    VOCALDAW_CLIP_FLOATS (X)
   #undef X
   #define X(name, def) if (name != o.name) return false;
    VOCALDAW_CLIP_BOOLS (X)
    VOCALDAW_CLIP_INTS (X)
   #undef X
    return true;
}

void ClipSettings::writeTo (juce::XmlElement& xml) const
{
    const ClipSettings defaults;
   #define X(name, def) if (name != defaults.name) xml.setAttribute ("fx_" #name, (double) name);
    VOCALDAW_CLIP_FLOATS (X)
   #undef X
   #define X(name, def) if (name != defaults.name) xml.setAttribute ("fx_" #name, name);
    VOCALDAW_CLIP_BOOLS (X)
    VOCALDAW_CLIP_INTS (X)
   #undef X
}

void ClipSettings::readFrom (const juce::XmlElement& xml)
{
   #define X(name, def) name = (float) xml.getDoubleAttribute ("fx_" #name, def);
    VOCALDAW_CLIP_FLOATS (X)
   #undef X
   #define X(name, def) name = xml.getBoolAttribute ("fx_" #name, def);
    VOCALDAW_CLIP_BOOLS (X)
   #undef X
   #define X(name, def) name = xml.getIntAttribute ("fx_" #name, def);
    VOCALDAW_CLIP_INTS (X)
   #undef X
}

//==============================================================================
namespace
{
    using Buf = juce::AudioBuffer<float>;
    constexpr float pi = juce::MathConstants<float>::pi;

    // ratio = input samples consumed per output sample (>1 = shorter and higher)
    Buf resample (const Buf& in, double ratio)
    {
        if (std::abs (ratio - 1.0) < 1.0e-6 || in.getNumSamples() < 16)
            return in;

        const int outLen = juce::jmax (1, (int) std::floor ((in.getNumSamples() - 8) / ratio));
        Buf out (in.getNumChannels(), outLen);
        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            juce::LagrangeInterpolator li;
            li.process (ratio, in.getReadPointer (c), out.getWritePointer (c), outLen);
        }
        return out;
    }

    // WSOLA time-stretch: factor = output length / input length, pitch unchanged.
    Buf timeStretch (const Buf& in, double factor)
    {
        if (std::abs (factor - 1.0) < 1.0e-4)
            return in;

        constexpr int N = 2048, Hs = N / 2, tol = 512;
        const int inLen = in.getNumSamples();
        const int nCh = in.getNumChannels();
        if (inLen < N * 2)
            return resample (in, 1.0 / factor);

        const double Ha = Hs / factor;
        const int outLen = (int) (inLen * factor);

        Buf out (nCh, outLen + N);
        out.clear();
        std::vector<float> norm ((size_t) (outLen + N), 0.0f), win ((size_t) N), mono ((size_t) inLen, 0.0f);

        for (int i = 0; i < N; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) (N - 1));

        for (int c = 0; c < nCh; ++c)
        {
            auto* src = in.getReadPointer (c);
            for (int i = 0; i < inLen; ++i)
                mono[(size_t) i] += src[i] / (float) nCh;
        }

        int prev = 0;
        for (int k = 0;; ++k)
        {
            const int outPos = k * Hs;
            if (outPos >= outLen)
                break;

            int pos = (int) std::llround (k * Ha);

            if (k > 0)
            {
                const int natural = prev + Hs;
                if (natural + N / 2 < inLen)
                {
                    int best = pos;
                    double bestScore = -1.0e30;
                    for (int d = -tol; d <= tol; d += 8)
                    {
                        const int cand = pos + d;
                        if (cand < 0 || cand + N >= inLen)
                            continue;
                        double score = 0.0;
                        for (int i = 0; i < N / 2; i += 4)
                            score += (double) mono[(size_t) (cand + i)] * mono[(size_t) (natural + i)];
                        if (score > bestScore)
                        {
                            bestScore = score;
                            best = cand;
                        }
                    }
                    pos = best;
                }
            }

            pos = juce::jlimit (0, inLen - N - 1, pos);

            for (int c = 0; c < nCh; ++c)
            {
                auto* src = in.getReadPointer (c, pos);
                auto* dst = out.getWritePointer (c, outPos);
                for (int i = 0; i < N; ++i)
                    dst[i] += src[i] * win[(size_t) i];
            }
            for (int i = 0; i < N; ++i)
                norm[(size_t) (outPos + i)] += win[(size_t) i];

            prev = pos;
        }

        for (int c = 0; c < nCh; ++c)
        {
            auto* d = out.getWritePointer (c);
            for (int i = 0; i < outLen; ++i)
                if (norm[(size_t) i] > 1.0e-3f)
                    d[i] /= norm[(size_t) i];
        }

        out.setSize (nCh, outLen, true);
        return out;
    }

    // Pitch glide across the clip: amount -1..1 = down/up an octave by the end (varispeed).
    Buf pogoBend (const Buf& in, float amount)
    {
        if (std::abs (amount) < 0.001f || in.getNumSamples() < 4)
            return in;

        const int inLen = in.getNumSamples();
        const int nCh = in.getNumChannels();
        std::vector<std::vector<float>> outs ((size_t) nCh);
        for (auto& o : outs)
            o.reserve ((size_t) inLen * 2 + 16);

        double rp = 0.0;
        while (rp < inLen - 2)
        {
            const int i = (int) rp;
            const float f = (float) (rp - i);
            for (int c = 0; c < nCh; ++c)
            {
                auto* s = in.getReadPointer (c);
                outs[(size_t) c].push_back (s[i] + (s[i + 1] - s[i]) * f);
            }
            rp += std::pow (2.0, amount * rp / inLen);
        }

        Buf out (nCh, (int) outs[0].size());
        for (int c = 0; c < nCh; ++c)
            std::copy (outs[(size_t) c].begin(), outs[(size_t) c].end(), out.getWritePointer (c));
        return out;
    }

    void makeStereo (Buf& b)
    {
        if (b.getNumChannels() >= 2)
            return;
        Buf s (2, b.getNumSamples());
        s.copyFrom (0, 0, b, 0, 0, b.getNumSamples());
        s.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
        b = std::move (s);
    }

    void extend (Buf& b, int extra)
    {
        if (extra <= 0)
            return;
        const int oldLen = b.getNumSamples();
        b.setSize (b.getNumChannels(), oldLen + extra, true, true);
        b.clear (oldLen, extra);
    }

    void applyFilter (Buf& b, const juce::IIRCoefficients& coeffs)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            juce::IIRFilter f;
            f.setCoefficients (coeffs);
            f.processSamples (b.getWritePointer (c), b.getNumSamples());
        }
    }

    void fade (Buf& b, int start, int len, bool fadeIn)
    {
        len = juce::jmin (len, b.getNumSamples() - start);
        if (len <= 0)
            return;
        for (int c = 0; c < b.getNumChannels(); ++c)
            b.applyGainRamp (c, start, len, fadeIn ? 0.0f : 1.0f, fadeIn ? 1.0f : 0.0f);
    }
}

double ClipFx::detectTempo (const juce::AudioBuffer<float>& buf, int start, int length, double sr)
{
    const int hop = juce::jmax (1, (int) (sr * 0.01)); // 10 ms frames -> 100 frames per second
    const int frames = length / hop;
    if (frames < 300) // need a few seconds
        return 0.0;

    std::vector<float> env ((size_t) frames, 0.0f), onset ((size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        double e = 0.0;
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            const float* d = buf.getReadPointer (c, start + f * hop);
            for (int i = 0; i < hop; ++i)
                e += d[i] * d[i];
        }
        env[(size_t) f] = (float) std::log1p (e * 100.0);
        if (f > 0)
            onset[(size_t) f] = juce::jmax (0.0f, env[(size_t) f] - env[(size_t) f - 1]);
    }

    // autocorrelation over the 70..180 BPM range, with a gentle preference for 90..140
    double best = 0.0, bestScore = 0.0;
    for (int lag = 33; lag <= 86; ++lag)
    {
        double score = 0.0;
        for (int f = lag; f < frames; ++f)
            score += onset[(size_t) f] * onset[(size_t) (f - lag)];
        const double bpm = 6000.0 / lag;
        score *= 1.0 - 0.15 * std::abs (bpm - 115.0) / 65.0;
        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }
    return bestScore > 0.0 ? best : 0.0;
}

std::vector<std::pair<int, int>> ClipFx::findSilences (const juce::AudioBuffer<float>& buf, int start, int length, double sr,
                                                        float thresholdDb, double minSilenceMs)
{
    std::vector<std::pair<int, int>> spans;
    const int win = juce::jmax (1, (int) (sr * 0.01));
    const float thr = juce::Decibels::decibelsToGain (thresholdDb);
    const int minLen = (int) (sr * minSilenceMs / 1000.0);

    int runStart = -1;
    for (int pos = 0; pos < length; pos += win)
    {
        const int n = juce::jmin (win, length - pos);
        float peak = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c)
            peak = juce::jmax (peak, buf.getMagnitude (c, start + pos, n));

        const bool quiet = peak < thr;
        if (quiet && runStart < 0)
            runStart = pos;
        if ((! quiet || pos + n >= length) && runStart >= 0)
        {
            const int runEnd = quiet ? pos + n : pos;
            if (runEnd - runStart >= minLen)
                spans.emplace_back (runStart, runEnd);
            runStart = -1;
        }
    }
    return spans;
}

juce::AudioBuffer<float> ClipFx::render (const juce::AudioBuffer<float>& source, const ClipSettings& s, double sr, double bpm)
{
    const int srcLen = source.getNumSamples();
    if (srcLen <= 0)
        return source;

    sr = juce::jmax (8000.0, sr);

    // 1. Sample start / length
    const int a = juce::jlimit (0, srcLen - 1, (int) (srcLen * juce::jlimit (0.0f, 100.0f, s.startPct) / 100.0f));
    const int n = juce::jlimit (1, srcLen - a, (int) (srcLen * juce::jlimit (0.0f, 100.0f, s.lengthPct) / 100.0f));
    Buf b (source.getNumChannels(), n);
    for (int c = 0; c < b.getNumChannels(); ++c)
        b.copyFrom (c, 0, source, c, a, n);

    // 2. Reverse
    if (s.reverse)
        for (int c = 0; c < b.getNumChannels(); ++c)
            std::reverse (b.getWritePointer (c), b.getWritePointer (c) + b.getNumSamples());

    // 3. Pitch / time
    const double pitchRatio = std::pow (2.0, (s.pitchSemis + s.fineCents / 100.0) / 12.0);
    const double speed = juce::jlimit (0.25, 4.0, (double) s.speed);
    if (s.timeMode == 0)
    {
        b = resample (b, pitchRatio * speed);
    }
    else
    {
        b = timeStretch (b, pitchRatio / speed);
        b = resample (b, pitchRatio);
    }

    // 4. Pogo (pitch glide)
    b = pogoBend (b, s.pogo);

    // 5. Utility
    if (s.removeDC)
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* d = b.getWritePointer (c);
            double mean = 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i) mean += d[i];
            mean /= juce::jmax (1, b.getNumSamples());
            juce::FloatVectorOperations::add (d, (float) -mean, b.getNumSamples());
        }

    if (s.invert)
        b.applyGain (-1.0f);

    const bool needStereo = s.swapStereo || s.stereoFade || std::abs (s.pan) > 0.001f || s.reverb > 0.001f || s.delay > 0.001f;
    if (needStereo)
        makeStereo (b);

    if (s.swapStereo)
    {
        Buf tmp (1, b.getNumSamples());
        tmp.copyFrom (0, 0, b, 0, 0, b.getNumSamples());
        b.copyFrom (0, 0, b, 1, 0, b.getNumSamples());
        b.copyFrom (1, 0, tmp, 0, 0, b.getNumSamples());
    }

    // 6. Colour
    if (std::abs (s.eqTilt) > 0.01f)
    {
        const float db = s.eqTilt * 9.0f;
        applyFilter (b, juce::IIRCoefficients::makeLowShelf (sr, 300.0, 0.7, juce::Decibels::decibelsToGain (-db)));
        applyFilter (b, juce::IIRCoefficients::makeHighShelf (sr, 3000.0, 0.7, juce::Decibels::decibelsToGain (db)));
    }

    if (s.cutHz < 19500.0f)
    {
        const double q = 0.707 + s.resonance * 9.0;
        applyFilter (b, juce::IIRCoefficients::makeLowPass (sr, juce::jlimit (30.0, sr * 0.45, (double) s.cutHz), q));
    }

    if (s.ringMix > 0.001f)
    {
        const double w = juce::MathConstants<double>::twoPi * s.ringFreq / sr;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* d = b.getWritePointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] = d[i] * (1.0f - s.ringMix) + d[i] * (float) std::sin (w * i) * s.ringMix;
        }
    }

    if (std::abs (s.boostDb) > 0.01f)
        b.applyGain (juce::Decibels::decibelsToGain (s.boostDb));

    if (s.softClip)
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* d = b.getWritePointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] = std::tanh (d[i]);
        }

    // 7. Space (adds a tail)
    if (s.delay > 0.001f)
    {
        const int delaySamples = juce::jmax (1, (int) (sr * 60.0 / juce::jmax (20.0, bpm) * 0.75)); // dotted 1/8
        extend (b, (int) (sr * 2.5));
        std::vector<float> dl ((size_t) delaySamples, 0.0f), dr ((size_t) delaySamples, 0.0f);
        auto* L = b.getWritePointer (0);
        auto* R = b.getWritePointer (1);
        const float fb = 0.42f;
        int idx = 0;
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float yl = dl[(size_t) idx], yr = dr[(size_t) idx];
            dl[(size_t) idx] = (L[i] + R[i]) * 0.5f + yr * fb;  // ping-pong
            dr[(size_t) idx] = yl * fb;
            L[i] += yl * s.delay;
            R[i] += yr * s.delay;
            if (++idx >= delaySamples)
                idx = 0;
        }
    }

    if (s.reverb > 0.001f)
    {
        extend (b, (int) (sr * (s.reverbType == 1 ? 4.0 : 2.0)));
        juce::Reverb rv;
        juce::Reverb::Parameters p;
        p.roomSize = s.reverbType == 1 ? 0.88f : 0.45f;
        p.damping = s.reverbType == 1 ? 0.35f : 0.6f;
        p.wetLevel = s.reverb * 0.55f;
        p.dryLevel = 1.0f - s.reverb * 0.35f;
        p.width = 1.0f;
        rv.setSampleRate (sr);
        rv.setParameters (p);
        rv.processStereo (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples());
    }

    // 8. Shape
    if (s.stereoFade && b.getNumChannels() == 2)
    {
        auto* L = b.getWritePointer (0);
        auto* R = b.getWritePointer (1);
        const int len = b.getNumSamples();
        for (int i = 0; i < len; ++i)
        {
            const float theta = (pi * 0.5f) * (float) i / (float) juce::jmax (1, len - 1);
            L[i] *= juce::jmin (1.0f, std::cos (theta) * juce::MathConstants<float>::sqrt2);
            R[i] *= juce::jmin (1.0f, std::sin (theta) * juce::MathConstants<float>::sqrt2);
        }
    }

    if (s.trimDb > -89.0f)
    {
        const float thr = juce::Decibels::decibelsToGain (s.trimDb);
        int first = b.getNumSamples(), last = -1;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* d = b.getReadPointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (std::abs (d[i]) > thr) { first = juce::jmin (first, i); break; }
            for (int i = b.getNumSamples(); --i >= 0;)
                if (std::abs (d[i]) > thr) { last = juce::jmax (last, i); break; }
        }
        if (last > first)
        {
            Buf t (b.getNumChannels(), last - first + 1);
            for (int c = 0; c < b.getNumChannels(); ++c)
                t.copyFrom (c, 0, b, c, first, t.getNumSamples());
            b = std::move (t);
        }
    }

    const int len = b.getNumSamples();
    if (s.fadeInMs > 0.5f)  fade (b, 0, (int) (sr * s.fadeInMs / 1000.0), true);
    if (s.fadeOutMs > 0.5f) { const int fl = juce::jmin (len, (int) (sr * s.fadeOutMs / 1000.0)); fade (b, len - fl, fl, false); }

    if (s.declick > 0)
    {
        const int dl = juce::jmin (len / 4, (int) (sr * (s.declick == 1 ? 0.003 : 0.020)));
        fade (b, 0, dl, true);
        fade (b, len - dl, dl, false);
    }

    if (s.normalize)
    {
        const float peak = b.getMagnitude (0, len);
        if (peak > 1.0e-5f)
            b.applyGain (0.977f / peak); // -0.2 dB
    }

    // 9. Clip gain / pan
    if (std::abs (s.gainDb) > 0.01f)
        b.applyGain (s.gainDb <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (s.gainDb));

    if (std::abs (s.pan) > 0.001f && b.getNumChannels() == 2)
    {
        const float angle = (s.pan + 1.0f) * pi * 0.25f;
        b.applyGain (0, 0, len, std::cos (angle) * juce::MathConstants<float>::sqrt2);
        b.applyGain (1, 0, len, std::sin (angle) * juce::MathConstants<float>::sqrt2);
    }

    return b;
}
