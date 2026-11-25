#include "AvatarMedia.h"

bool amethystDecodeVideoSquares (const wchar_t* path, int maxFrames, double intervalSeconds, int side,
                                 std::vector<std::uint8_t>& out, int& frameCount);

namespace AvatarMedia
{
namespace
{
    juce::Image squareOf (const juce::Image& src, int side)
    {
        const int s = juce::jmin (src.getWidth(), src.getHeight());
        return src.getClippedImage ({ (src.getWidth() - s) / 2, (src.getHeight() - s) / 2, s, s })
                  .rescaled (side, side, juce::Graphics::highResamplingQuality);
    }

    juce::Image stripFrom (const std::vector<juce::Image>& frames)
    {
        if (frames.empty())
            return {};
        const int side = frames.front().getHeight();
        juce::Image strip (juce::Image::RGB, side * (int) frames.size(), side, true);
        juce::Graphics g (strip);
        for (size_t i = 0; i < frames.size(); ++i)
        {
            g.setColour (juce::Colour (0xff1a1029));   // transparent GIF areas: AMETHYST purple, not black
            g.fillRect ((int) i * side, 0, side, side);
            g.drawImageAt (frames[i], (int) i * side, 0);
        }
        return strip;
    }

    //==========================================================================
    // GIF: LZW-coded indexed frames with optional transparency and disposal rules.
    struct Reader
    {
        const juce::uint8* p; size_t n, pos = 0;
        bool has (size_t k) const { return pos + k <= n; }
        int u8() { return has (1) ? p[pos++] : -1; }
        int u16() { if (! has (2)) return -1; const int v = p[pos] | (p[pos + 1] << 8); pos += 2; return v; }
        void skipSubBlocks() { for (int len; (len = u8()) > 0 && has ((size_t) len);) pos += (size_t) len; }
    };

    bool lzwDecode (const std::vector<juce::uint8>& data, int minCodeSize, std::vector<juce::uint8>& out, size_t pixels)
    {
        if (minCodeSize < 2 || minCodeSize > 11)
            return false;
        const int clear = 1 << minCodeSize, eoi = clear + 1;
        std::vector<juce::uint16> prefix (4096);
        std::vector<juce::uint8> suffix (4096), stack (4097);
        int codeSize = minCodeSize + 1, next = eoi + 1, prev = -1;
        juce::uint8 first = 0;
        size_t bitPos = 0;
        const size_t totalBits = data.size() * 8;
        out.clear();
        out.reserve (pixels);

        for (int c = 0; c < clear; ++c) { prefix[(size_t) c] = 0xffff; suffix[(size_t) c] = (juce::uint8) c; }

        while (bitPos + (size_t) codeSize <= totalBits && out.size() < pixels)
        {
            int code = 0;
            for (int b = 0; b < codeSize; ++b, ++bitPos)
                code |= ((data[bitPos >> 3] >> (bitPos & 7)) & 1) << b;

            if (code == clear) { codeSize = minCodeSize + 1; next = eoi + 1; prev = -1; continue; }
            if (code == eoi) break;

            int cur = code, sp = 0;
            if (prev < 0)
            {
                if (code >= clear) return false;
                out.push_back ((juce::uint8) code);
                first = (juce::uint8) code;
                prev = code;
                continue;
            }
            if (code >= next)          // KwKwK case
            {
                if (code > next) return false;
                stack[(size_t) sp++] = first;
                cur = prev;
            }
            while (cur >= clear && sp < 4096)
            {
                stack[(size_t) sp++] = suffix[(size_t) cur];
                cur = prefix[(size_t) cur];
            }
            first = (juce::uint8) cur;
            stack[(size_t) sp++] = first;
            while (sp > 0 && out.size() < pixels)
                out.push_back (stack[(size_t) --sp]);

            if (next < 4096)
            {
                prefix[(size_t) next] = (juce::uint16) prev;
                suffix[(size_t) next] = first;
                ++next;
                if (next == (1 << codeSize) && codeSize < 12)
                    ++codeSize;
            }
            prev = code;
        }
        out.resize (pixels, 0);
        return true;
    }
}

bool decodeGif (const void* bytes, size_t size, const std::function<bool (const juce::Image&, int)>& onFrame)
{
    Reader r { (const juce::uint8*) bytes, size };
    if (size < 13 || std::memcmp (bytes, "GIF8", 4) != 0)
        return false;
    r.pos = 6;
    const int w = r.u16(), h = r.u16(), packed = r.u8();
    r.u8(); r.u8();   // background index, aspect
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return false;

    std::vector<juce::PixelARGB> globalTable;
    auto readTable = [&r] (int count, std::vector<juce::PixelARGB>& table)
    {
        table.resize ((size_t) count);
        for (auto& c : table)
        {
            const int rr = r.u8(), gg = r.u8(), bb = r.u8();
            c.setARGB (255, (juce::uint8) rr, (juce::uint8) gg, (juce::uint8) bb);
        }
        return r.pos <= r.n;
    };
    if ((packed & 0x80) && ! readTable (2 << (packed & 7), globalTable))
        return false;

    juce::Image canvas (juce::Image::ARGB, w, h, true), previous;
    int delayMs = 100, transparent = -1, disposal = 0;
    bool any = false;

    while (r.has (1))
    {
        const int block = r.u8();
        if (block == 0x3b)   // trailer
            break;
        if (block == 0x21)   // extension
        {
            const int label = r.u8();
            if (label == 0xf9)
            {
                const int len = r.u8();
                if (len >= 4 && r.has ((size_t) len))
                {
                    const int pk = r.u8();
                    const int d = r.u16();
                    const int ti = r.u8();
                    r.pos += (size_t) (len - 4);
                    disposal = (pk >> 2) & 7;
                    transparent = (pk & 1) ? ti : -1;
                    delayMs = d <= 1 ? 100 : d * 10;   // 0/1 cs is treated as 100 ms by browsers
                }
                r.skipSubBlocks();
            }
            else
                r.skipSubBlocks();
            continue;
        }
        if (block != 0x2c)
            return any;

        const int fx = r.u16(), fy = r.u16(), fw = r.u16(), fh = r.u16(), fp = r.u8();
        if (fw <= 0 || fh <= 0)
            return any;
        std::vector<juce::PixelARGB> localTable;
        if ((fp & 0x80) && ! readTable (2 << (fp & 7), localTable))
            return any;
        const auto& table = localTable.empty() ? globalTable : localTable;
        const bool interlaced = (fp & 0x40) != 0;

        const int minCode = r.u8();
        std::vector<juce::uint8> lzw;
        for (int len; (len = r.u8()) > 0;)
        {
            if (! r.has ((size_t) len)) return any;
            lzw.insert (lzw.end(), r.p + r.pos, r.p + r.pos + len);
            r.pos += (size_t) len;
        }
        std::vector<juce::uint8> indices;
        if (! lzwDecode (lzw, minCode, indices, (size_t) fw * (size_t) fh))
            return any;

        if (disposal == 3)
            previous = canvas.createCopy();

        {
            juce::Image::BitmapData bd (canvas, juce::Image::BitmapData::readWrite);
            // interlaced GIFs store rows in four passes: every 8th from 0, every 8th from 4, every 4th from 2, every 2nd from 1
            std::vector<int> rowOrder;
            if (interlaced)
            {
                for (int y = 0; y < fh; y += 8) rowOrder.push_back (y);
                for (int y = 4; y < fh; y += 8) rowOrder.push_back (y);
                for (int y = 2; y < fh; y += 4) rowOrder.push_back (y);
                for (int y = 1; y < fh; y += 2) rowOrder.push_back (y);
            }
            for (int row = 0; row < fh; ++row)
            {
                const int y = interlaced ? rowOrder[(size_t) row] : row;
                const int cy = fy + y;
                if (cy < 0 || cy >= h) continue;
                for (int x = 0; x < fw; ++x)
                {
                    const int cx = fx + x;
                    if (cx < 0 || cx >= w) continue;
                    const int idx = indices[(size_t) row * (size_t) fw + (size_t) x];
                    if (idx == transparent || idx >= (int) table.size()) continue;
                    *reinterpret_cast<juce::PixelARGB*> (bd.getPixelPointer (cx, cy)) = table[(size_t) idx];
                }
            }
        }

        any = true;
        if (! onFrame (canvas, delayMs))
            return true;

        // what happens to this frame before the next one is drawn
        if (disposal == 2)
            canvas.clear ({ fx, fy, fw, fh });
        else if (disposal == 3 && previous.isValid())
            canvas = previous;
        transparent = -1; disposal = 0; delayMs = 100;
    }
    return any;
}

juce::Image load (const juce::File& file, juce::String& error)
{
    const auto ext = file.getFileExtension().toLowerCase();

    if (ext == ".gif")
    {
        juce::MemoryBlock mb;
        if (! file.loadFileAsData (mb)) { error = "Couldn't read that GIF."; return {}; }

        // resample the GIF's own timing onto a fixed 80 ms grid while decoding (keeps memory small)
        std::vector<juce::Image> frames;
        double t = 0.0;
        const bool ok = decodeGif (mb.getData(), mb.getSize(), [&] (const juce::Image& canvas, int delayMs)
        {
            const double end = t + delayMs;
            while ((double) frames.size() * kFrameMs < end && (int) frames.size() < kMaxFrames)
                frames.push_back (squareOf (canvas, kAnimSide));
            t = end;
            return (int) frames.size() < kMaxFrames;
        });
        if (! ok || frames.empty()) { error = "That GIF couldn't be read."; return {}; }
        if (frames.size() == 1)
            return squareOf (frames.front(), 256);
        return stripFrom (frames);
    }

    if (ext == ".mp4" || ext == ".mov" || ext == ".m4v" || ext == ".wmv")
    {
       #if JUCE_WINDOWS
        std::vector<std::uint8_t> bgra;
        int count = 0;
        if (! amethystDecodeVideoSquares (file.getFullPathName().toWideCharPointer(), kMaxFrames, kFrameMs / 1000.0, kAnimSide, bgra, count) || count == 0)
        {
            error = "That video couldn't be read (MP4 with H.264 works best).";
            return {};
        }
        std::vector<juce::Image> frames;
        for (int i = 0; i < count; ++i)
        {
            juce::Image f (juce::Image::ARGB, kAnimSide, kAnimSide, false);
            juce::Image::BitmapData bd (f, juce::Image::BitmapData::writeOnly);
            const auto* src = bgra.data() + (size_t) i * kAnimSide * kAnimSide * 4;
            for (int y = 0; y < kAnimSide; ++y)
                for (int x = 0; x < kAnimSide; ++x)
                {
                    const auto* s = src + ((size_t) y * kAnimSide + x) * 4;
                    bd.setPixelColour (x, y, juce::Colour (s[2], s[1], s[0]));
                }
            frames.push_back (f);
        }
        return frames.size() == 1 ? squareOf (frames.front(), 256) : stripFrom (frames);
       #else
        error = "Video profile pictures work on Windows for now - use a GIF on this computer.";
        return {};
       #endif
    }

    auto img = juce::ImageFileFormat::loadFrom (file);
    if (! img.isValid()) { error = "That file isn't a picture AMETHYST can read (PNG, JPG, GIF or MP4)."; return {}; }
    return squareOf (img, 256);
}

bool isAnimated (const juce::Image& img)
{
    return img.isValid() && img.getHeight() > 0 && img.getWidth() >= img.getHeight() * 2;
}

int frameCount (const juce::Image& img)
{
    return isAnimated (img) ? img.getWidth() / img.getHeight() : 1;
}

juce::Image currentFrame (const juce::Image& img)
{
    if (! isAnimated (img))
        return img;
    const int side = img.getHeight();
    const int idx = (int) ((juce::Time::getMillisecondCounter() / (juce::uint32) kFrameMs) % (juce::uint32) frameCount (img));
    return img.getClippedImage ({ idx * side, 0, side, side });
}

juce::MemoryBlock encode (const juce::Image& img, juce::String& mimeType, size_t maxBytes)
{
    juce::MemoryOutputStream out;
    if (! isAnimated (img))
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (img, out);
        mimeType = "image/png";
        return { out.getData(), out.getDataSize() };
    }
    // strips go up as JPEG (the avatars bucket allows PNG and JPEG, up to 1 MB)
    mimeType = "image/jpeg";
    for (float q : { 0.85f, 0.7f, 0.55f, 0.4f })
    {
        juce::MemoryOutputStream jpg;
        juce::JPEGImageFormat fmt;
        fmt.setQuality (q);
        fmt.writeImageToStream (img, jpg);
        if (jpg.getDataSize() <= maxBytes || q < 0.45f)
            return { jpg.getData(), jpg.getDataSize() };
    }
    return {};
}
}
