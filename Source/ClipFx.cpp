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
    // 1. Onset strength: log-spectral flux at 200 frames per second (5 ms), up to 60 s of audio.
    constexpr double fps = 200.0;
    constexpr int order = 10, N = 1 << order;   // ~21 ms frames at 48 kHz
    const int hop = juce::jmax (1, (int) std::lround (sr / fps));
    length = juce::jmin (length, (int) (sr * 60.0));
    const int frames = (length - N) / hop;
    if (frames < (int) (fps * 4.0))   // need at least ~4 seconds
        return 0.0;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) N), data ((size_t) (2 * N));
    for (int i = 0; i < N; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);
    const int bins = juce::jmin (N / 2, (int) (9000.0 * N / sr));   // ignore the top end (hiss, cymbal wash)
    std::vector<float> prev ((size_t) bins, 0.0f), onset ((size_t) frames, 0.0f);
    const int nCh = juce::jmin (2, buf.getNumChannels());
    for (int f = 0; f < frames; ++f)
    {
        std::fill (data.begin(), data.end(), 0.0f);
        for (int c = 0; c < nCh; ++c)
        {
            const float* d = buf.getReadPointer (c, start + f * hop);
            for (int i = 0; i < N; ++i) data[(size_t) i] += d[i] * window[(size_t) i];
        }
        fft.performFrequencyOnlyForwardTransform (data.data());
        float flux = 0.0f;
        for (int k = 1; k < bins; ++k)
        {
            const float m = std::log1p (1000.0f * data[(size_t) k]);
            flux += juce::jmax (0.0f, m - prev[(size_t) k]);
            prev[(size_t) k] = m;
        }
        onset[(size_t) f] = flux;
    }
    // remove the slowly-moving part (loud sections), keep the hits
    {
        std::vector<float> smooth ((size_t) frames);
        const int w = (int) (fps * 0.1);
        double acc = 0.0;
        for (int f = 0; f < frames; ++f)
        {
            acc += onset[(size_t) f];
            if (f >= 2 * w) acc -= onset[(size_t) (f - 2 * w)];
            smooth[(size_t) juce::jmax (0, f - w)] = (float) (acc / juce::jmin (f + 1, 2 * w));
        }
        for (int f = 0; f < frames; ++f) onset[(size_t) f] = juce::jmax (0.0f, onset[(size_t) f] - smooth[(size_t) f]);
    }

    // 2. Autocorrelation of the onset curve (normalised per overlap), read with fractional lags.
    const int maxLag = juce::jmin (frames / 2, (int) (fps * 60.0 / 50.0 * 4.0) + 2);   // up to 4 beats at 50 BPM
    std::vector<double> acf ((size_t) maxLag + 2, 0.0);
    for (int lag = 1; lag <= maxLag + 1; ++lag)
    {
        double s = 0.0;
        for (int f = lag; f < frames; ++f) s += (double) onset[(size_t) f] * onset[(size_t) (f - lag)];
        acf[(size_t) lag] = s / (frames - lag);
    }
    auto acfAt = [&] (double lag)
    {
        if (lag < 1.0 || lag > maxLag) return 0.0;
        const int i = (int) lag;
        const double t = lag - i;
        return acf[(size_t) i] * (1.0 - t) + acf[(size_t) i + 1] * t;
    };

    // 3. Coarse: score each tempo by its beat period and its multiples (bars), with a soft
    //    preference around 120 BPM so half / double-time mistakes are rare.
    double coarse = 0.0, bestScore = -1.0;
    for (double bpm = 60.0; bpm <= 200.0; bpm += 0.25)
    {
        const double T = fps * 60.0 / bpm;
        const double s = acfAt (T) + 0.6 * acfAt (2.0 * T) + 0.4 * acfAt (4.0 * T) + 0.25 * acfAt (T * 0.5);
        const double oct = std::log2 (bpm / 120.0);
        const double score = s * std::exp (-0.5 * oct * oct / (0.9 * 0.9));
        if (score > bestScore) { bestScore = score; coarse = bpm; }
    }
    if (bestScore <= 0.0)
        return 0.0;

    // 4. Fine: slide a beat grid (best phase per tempo) across the whole onset curve, 0.01 BPM steps.
    auto onsetAt = [&] (double pos)
    {
        const int i = (int) pos;
        if (i < 0 || i + 1 >= frames) return 0.0;
        const double t = pos - i;
        return onset[(size_t) i] * (1.0 - t) + onset[(size_t) i + 1] * t;
    };
    double fine = coarse, fineScore = -1.0;
    for (double bpm = coarse - 1.5; bpm <= coarse + 1.5; bpm += 0.01)
    {
        const double T = fps * 60.0 / bpm;
        double best = 0.0;
        for (double phase = 0.0; phase < T; phase += 1.0)
        {
            double s = 0.0;
            for (double p = phase; p < frames; p += T) s += onsetAt (p);
            best = juce::jmax (best, s);
        }
        best /= std::floor (frames / T);
        if (best > fineScore) { fineScore = best; fine = bpm; }
    }

    // Most music sits on a whole number: snap when it's that close
    const double rounded = std::round (fine);
    return std::abs (fine - rounded) <= 0.12 ? rounded : std::round (fine * 100.0) / 100.0;
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

//==============================================================================
juce::String ClipFx::KeyResult::name() const
{
    static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    if (tonic < 0 || tonic > 11)
        return "Unknown";
    return juce::String (names[tonic]) + (minor ? " minor" : " major");
}

ClipFx::KeyResult ClipFx::detectKey (const juce::AudioBuffer<float>& buf, int start, int length, double sr)
{
    KeyResult result;
    start = juce::jlimit (0, buf.getNumSamples(), start);
    length = juce::jlimit (0, buf.getNumSamples() - start, length);
    if (length < (int) (sr * 2.0) || sr <= 0.0)
        return result;

    // Mono, decimated to ~11 kHz (pitch classes only need up to a couple of kHz).
    const int decim = juce::jmax (1, (int) std::lround (sr / 11025.0));
    const double rate = sr / decim;
    const int maxSamples = (int) (rate * 180.0);   // the first 3 minutes are plenty
    std::vector<float> mono;
    mono.reserve ((size_t) juce::jmin (maxSamples, length / decim + 1));
    for (int i = 0; i + decim <= length && (int) mono.size() < maxSamples; i += decim)
    {
        float sum = 0.0f;
        for (int k = 0; k < decim; ++k)
            for (int c = 0; c < buf.getNumChannels(); ++c)
                sum += buf.getSample (c, start + i + k);
        mono.push_back (sum / (float) (decim * juce::jmax (1, buf.getNumChannels())));
    }

    constexpr int order = 12, fftSize = 1 << order, hop = fftSize / 2;
    if ((int) mono.size() < fftSize)
        return result;

    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann);
    std::vector<float> frame ((size_t) fftSize * 2);
    std::array<double, 12> chroma {};

    // Map every FFT bin in 55 Hz .. 2 kHz to a pitch class once.
    std::vector<int> binClass ((size_t) fftSize / 2, -1);
    for (int b = 1; b < fftSize / 2; ++b)
    {
        const double f = b * rate / fftSize;
        if (f < 55.0 || f > 2000.0) continue;
        const int midi = (int) std::lround (69.0 + 12.0 * std::log2 (f / 440.0));
        binClass[(size_t) b] = ((midi % 12) + 12) % 12;
    }

    for (size_t pos = 0; pos + fftSize <= mono.size(); pos += hop)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);
        std::copy (mono.begin() + (long) pos, mono.begin() + (long) pos + fftSize, frame.begin());
        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data());

        std::array<double, 12> fc {};
        double total = 0.0;
        for (int b = 1; b < fftSize / 2; ++b)
            if (binClass[(size_t) b] >= 0)
            {
                const double m = std::sqrt ((double) frame[(size_t) b]);   // soften loud bass notes
                fc[(size_t) binClass[(size_t) b]] += m;
                total += m;
            }
        if (total > 1.0e-6)
            for (int k = 0; k < 12; ++k)
                chroma[(size_t) k] += fc[(size_t) k] / total;   // each frame counts equally
    }

    static constexpr double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static constexpr double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    auto correlate = [&chroma] (const double* profile, int tonic)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) ((i + tonic) % 12)]; my += profile[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = chroma[(size_t) ((i + tonic) % 12)] - mx, y = profile[i] - my;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    };

    double best = -2.0, second = -2.0;
    for (int t = 0; t < 12; ++t)
        for (int m = 0; m < 2; ++m)
        {
            const double r = correlate (m == 0 ? major : minor, t);
            if (r > best) { second = best; best = r; result.tonic = t; result.minor = m == 1; }
            else if (r > second) second = r;
        }

    result.confidence = (float) juce::jlimit (0.0, 1.0, (best - second) * 5.0 + juce::jmax (0.0, best - 0.5));
    if (best < 0.3)
        result.tonic = -1;
    return result;
}
