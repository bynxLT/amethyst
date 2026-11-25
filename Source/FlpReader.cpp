#include "FlpReader.h"

namespace Flp
{
namespace
{
    struct Event
    {
        int id = 0;
        const juce::uint8* data = nullptr;
        size_t size = 0;

        juce::uint32 u32() const
        {
            juce::uint32 v = 0;
            for (size_t i = 0; i < juce::jmin<size_t> (4, size); ++i)
                v |= (juce::uint32) data[i] << (8 * i);
            return v;
        }
        int i8() const { return size > 0 ? (int) (juce::int8) data[0] : 0; }
    };

    inline juce::uint16 rd16 (const juce::uint8* p) { return (juce::uint16) (p[0] | (p[1] << 8)); }
    inline juce::uint32 rd32 (const juce::uint8* p) { return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24); }
    inline juce::int32 rdI32 (const juce::uint8* p) { return (juce::int32) rd32 (p); }
    inline float rdF32 (const juce::uint8* p) { float f; auto v = rd32 (p); std::memcpy (&f, &v, 4); return f; }
    inline juce::uint64 rd64 (const juce::uint8* p) { return (juce::uint64) rd32 (p) | ((juce::uint64) rd32 (p + 4) << 32); }

    // Event payload size: 0-63 one byte, 64-127 two, 128-191 four, 192+ a varint length.
    // FL Studio 26 writes event 172 with a 3-byte payload; older versions use 4. We try the standard
    // sizes first and fall back to the 26.x rule, keeping whichever decodes to exactly the data end.
    bool decode (const juce::uint8* begin, const juce::uint8* end, bool fix172, std::vector<Event>& out)
    {
        out.clear();
        auto* p = begin;
        while (p < end)
        {
            Event e;
            e.id = *p++;
            size_t size = 0;
            if (fix172 && e.id == 172)  size = 3;
            else if (e.id < 64)         size = 1;
            else if (e.id < 128)        size = 2;
            else if (e.id < 192)        size = 4;
            else
            {
                int shift = 0;
                juce::uint8 c = 0;
                do
                {
                    if (p >= end || shift > 28)
                        return false;
                    c = *p++;
                    size |= (size_t) (c & 0x7f) << shift;
                    shift += 7;
                } while (c & 0x80);
            }
            if (size > (size_t) (end - p))
                return false;
            e.data = p;
            e.size = size;
            p += size;
            out.push_back (e);
        }
        return p == end;
    }

    struct TextDecoder
    {
        bool utf16 = true;

        juce::String operator() (const Event& e) const
        {
            if (e.size == 0)
                return {};
            if (utf16 && e.size % 2 == 0)
            {
                std::vector<juce::juce_wchar> chars;
                for (size_t i = 0; i + 1 < e.size; i += 2)
                {
                    auto c = (juce::juce_wchar) rd16 (e.data + i);
                    if (c == 0) break;
                    if (c >= 0xd800 && c < 0xdc00 && i + 3 < e.size)   // surrogate pair
                    {
                        auto lo = (juce::juce_wchar) rd16 (e.data + i + 2);
                        c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                        i += 2;
                    }
                    chars.push_back (c);
                }
                chars.push_back (0);
                return juce::String (juce::CharPointer_UTF32 ((const juce::CharPointer_UTF32::CharType*) chars.data()));
            }
            size_t n = 0;
            while (n < e.size && e.data[n] != 0) ++n;
            return juce::String::fromUTF8 ((const char*) e.data, (int) n);
        }
    };

    juce::String utf8 (const juce::uint8* p, size_t n)
    {
        while (n > 0 && p[n - 1] == 0) --n;
        return juce::String::fromUTF8 ((const char*) p, (int) n);
    }

    // Fruity Wrapper data: u32 type, then {u32 id, u64 size, payload} records.
    void readWrapper (const Event& e, Plugin& plug)
    {
        if (e.size < 4)
            return;
        size_t o = 4;
        while (o + 12 <= e.size)
        {
            const auto id = rd32 (e.data + o);
            const auto sz = rd64 (e.data + o + 4);
            o += 12;
            if (sz > e.size - o)
                break;
            const auto* pl = e.data + o;
            switch (id)
            {
                case 53: plug.vstState.replaceAll (pl, (size_t) sz); break;
                case 54: plug.vstName = utf8 (pl, (size_t) sz); break;
                case 55: plug.vstPath = utf8 (pl, (size_t) sz); break;
                case 56: plug.vstVendor = utf8 (pl, (size_t) sz); break;
                default: break;
            }
            o += (size_t) sz;
        }
    }

    void readPlaylist (const Event& e, std::vector<PlaylistItem>& out)
    {
        // Record size grew over the years (32 -> 60 -> 88 bytes). Every record carries the
        // pattern base 0x5000 at offset 4, so pick the size where that holds for all of them.
        int rec = 0;
        for (int r : { 88, 60, 32 })
        {
            if (e.size == 0 || e.size % (size_t) r != 0)
                continue;
            bool ok = true;
            for (size_t o = 0; o < e.size && ok; o += (size_t) r)
                ok = rd16 (e.data + o + 4) == 0x5000;
            if (ok) { rec = r; break; }
        }
        if (rec == 0)
            rec = e.size % 60 == 0 ? 60 : 32;

        bool newTrackBase = false;
        for (size_t o = 0; o + 32 <= e.size; o += (size_t) rec)
            if (rd16 (e.data + o + 12) > 198)
                newTrackBase = true;
        const int base = newTrackBase ? 499 : 198;

        for (size_t o = 0; o + 32 <= e.size; o += (size_t) rec)
        {
            const auto* d = e.data + o;
            PlaylistItem it;
            it.position = rd32 (d);
            const int patternBase = rd16 (d + 4);
            const int index = rd16 (d + 6);
            it.length = rd32 (d + 8);
            it.track = juce::jmax (0, base - (int) rd16 (d + 12));
            it.muted = (rd16 (d + 18) & 0x2000) != 0;
            if (index > patternBase && patternBase > 0)
                it.pattern = index - patternBase;
            else
                it.channel = index;
            const float start = rdF32 (d + 24);
            if (std::isfinite (start) && start >= 0.0f)
                it.startOffsetMs = start;
            out.push_back (it);
        }
    }
}

juce::Result read (const juce::File& file, Project& out)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb))
        return juce::Result::fail ("Couldn't read " + file.getFullPathName());
    return read (mb, out);
}

juce::Result read (const juce::MemoryBlock& mb, Project& out)
{
    out = Project();
    const auto* b = (const juce::uint8*) mb.getData();
    const size_t n = mb.getSize();
    if (n < 22 || std::memcmp (b, "FLhd", 4) != 0)
        return juce::Result::fail ("This isn't an FL Studio project.");

    const auto headerLen = rd32 (b + 4);
    if (headerLen < 6 || 8 + (size_t) headerLen + 8 > n)
        return juce::Result::fail ("The project header is damaged.");
    out.ppq = juce::jmax (1, (int) rd16 (b + 12));

    const auto* chunk = b + 8 + headerLen;
    if (std::memcmp (chunk, "FLdt", 4) != 0)
        return juce::Result::fail ("The project data is missing.");
    const auto dataLen = juce::jmin ((size_t) rd32 (chunk + 4), n - (size_t) (chunk + 8 - b));
    const auto* begin = chunk + 8;
    const auto* end = begin + dataLen;

    std::vector<Event> ev;
    if (! decode (begin, end, false, ev))
    {
        if (! decode (begin, end, true, ev))
            return juce::Result::fail ("This FL Studio project uses a layout AMETHYST can't read yet.");
        out.used172Fix = true;
    }
    out.eventCount = (int) ev.size();

    TextDecoder text;
    for (auto& e : ev)
        if (e.id == 199)
        {
            out.version = utf8 (e.data, e.size);
            const auto parts = juce::StringArray::fromTokens (out.version, ".", {});
            const int major = parts[0].getIntValue(), minor = parts[1].getIntValue();
            text.utf16 = major > 11 || (major == 11 && minor >= 5);
            break;
        }

    // ---- tempo ------------------------------------------------------------
    double tempo = 0.0;
    for (auto& e : ev)
        if (e.id == 156)
        {
            const double t = e.u32() / 1000.0;
            if (t >= 10.0 && t <= 999.0) { tempo = t; break; }
        }
    if (tempo <= 0.0)
        for (auto& e : ev)
            if (e.id == 66 && e.u32() >= 10 && e.u32() <= 999) { tempo = (double) e.u32(); break; }
    out.tempo = tempo > 0.0 ? tempo : 120.0;

    // ---- walk ---------------------------------------------------------------
    Channel* channel = nullptr;
    bool channelHasRoute22 = false;
    bool inMixer = false;
    int arrangement = -1, currentArrangement = -1;
    std::map<int, std::vector<PlaylistItem>> itemsByArrangement;
    std::map<int, std::map<int, juce::String>> namesByArrangement;
    int lastTrackIid = -1;

    Insert insert;
    std::optional<Plugin> pending;
    auto flushPlugin = [&] (int slot)
    {
        if (! pending) return;
        pending->slot = slot >= 0 ? slot : (int) insert.plugins.size();
        if (pending->internalName.isNotEmpty() || pending->vstName.isNotEmpty())
            insert.plugins.push_back (std::move (*pending));
        pending.reset();
    };

    const Event* mixerParams = nullptr;

    for (auto& e : ev)
    {
        if (e.id == 225) { mixerParams = &e; continue; }
        if (e.id == 100) { currentArrangement = (int) e.u32(); continue; }

        if (! inMixer && (e.id == 236 || e.id == 204))
        {
            inMixer = true;
            channel = nullptr;
        }

        if (inMixer)
        {
            switch (e.id)
            {
                case 204: insert.name = text (e); break;
                case 201: flushPlugin (-1); pending = Plugin(); pending->internalName = text (e); break;
                case 203: if (pending) pending->displayName = text (e); break;
                case 213:
                    if (pending && pending->isWrapper()) readWrapper (e, *pending);
                    else if (pending)                    pending->state.replaceAll (e.data, e.size);
                    break;
                case 98:  flushPlugin ((int) e.u32()); break;
                case 147:
                    flushPlugin (-1);
                    insert.index = (int) out.inserts.size();
                    out.inserts.push_back (std::move (insert));
                    insert = Insert();
                    break;
                default: break;
            }
            continue;
        }

        switch (e.id)
        {
            case 64:
            {
                const int iid = (int) e.u32();
                channel = &out.channels[iid];
                channel->iid = iid;
                channelHasRoute22 = false;
                break;
            }
            case 99:
                arrangement = (int) e.u32();
                channel = nullptr;
                break;
            case 233:
                readPlaylist (e, itemsByArrangement[arrangement]);
                break;
            case 238:
                if (e.size >= 4) lastTrackIid = (int) rd32 (e.data);
                break;
            case 239:
                if (lastTrackIid > 0) namesByArrangement[arrangement][lastTrackIid - 1] = text (e);
                break;
            default:
                if (channel == nullptr)
                    break;
                switch (e.id)
                {
                    case 21:  channel->type = (int) e.u32(); break;
                    case 192: if (channel->name.isEmpty()) channel->name = text (e); break;
                    case 203: channel->name = text (e); break;
                    case 196: channel->samplePath = text (e); break;
                    case 22:  channel->insert = e.i8(); channelHasRoute22 = true; break;
                    case 104: if (! channelHasRoute22) channel->insert = (int) e.u32(); break;   // FL 25+ keeps the route here
                    case 219:
                        if (e.size >= 8)
                        {
                            channel->pan = juce::jlimit (-1.0f, 1.0f, (float) (rdI32 (e.data) - 6400) / 6400.0f);
                            channel->volume = juce::jlimit (0.0f, 2.0f, (float) rd32 (e.data + 4) / 10000.0f);
                        }
                        break;
                    default: break;
                }
                break;
        }
    }
    if (inMixer && (insert.name.isNotEmpty() || ! insert.plugins.empty() || pending))
    {
        flushPlugin (-1);
        insert.index = (int) out.inserts.size();
        out.inserts.push_back (std::move (insert));
    }

    // Pick the arrangement FL had open, else the first one that has clips.
    auto chosen = itemsByArrangement.find (currentArrangement);
    if (chosen == itemsByArrangement.end() || chosen->second.empty())
        for (auto it = itemsByArrangement.begin(); it != itemsByArrangement.end(); ++it)
            if (! it->second.empty()) { chosen = it; break; }
    if (chosen != itemsByArrangement.end())
    {
        out.items = chosen->second;
        out.trackNames = namesByArrangement[chosen->first];
    }

    // ---- mixer parameters: 12-byte records {u32, u8 id, u8, u16 insert/slot, i32 value} ----
    if (mixerParams != nullptr && mixerParams->size >= 12)
    {
        const auto* d = mixerParams->data;
        const size_t count = mixerParams->size / 12;
        // The insert number sits above the 6 slot bits. Newer projects add a base to it
        // (master = 0x1C0 in FL 26, 0 in FL 20), so measure the base from the master's volume record.
        int minInsert = 1 << 20;
        for (size_t r = 0; r < count; ++r)
            if (d[r * 12 + 4] == 192)
                minInsert = juce::jmin (minInsert, (int) (rd16 (d + r * 12 + 6) >> 6));
        const int offset = minInsert != (1 << 20) ? minInsert : 0;

        // FL 3-5 stored the slot mix knob as 0..128, later versions as 0..12800
        float mixScale = 128.0f;
        for (size_t r = 0; r < count; ++r)
            if (d[r * 12 + 4] == 1 && rdI32 (d + r * 12 + 8) > 128)
                mixScale = 12800.0f;

        for (size_t r = 0; r < count; ++r)
        {
            const auto* rec = d + r * 12;
            const int id = rec[4];
            const int cd = rd16 (rec + 6);
            const int insertIndex = (cd >> 6) - offset;
            const int slot = cd & 0x3f;
            const int value = rdI32 (rec + 8);
            if (insertIndex < 0 || insertIndex >= (int) out.inserts.size())
                continue;
            auto& ins = out.inserts[(size_t) insertIndex];
            switch (id)
            {
                case 192: ins.volume = juce::jlimit (0.0f, 2.0f, (float) value / 12800.0f); break;
                case 193: ins.pan = juce::jlimit (-1.0f, 1.0f, (float) value / 6400.0f); break;
                case 0:
                case 1:
                    for (auto& p : ins.plugins)
                        if (p.slot == slot)
                        {
                            if (id == 0) p.enabled = value != 0;
                            else         p.mix = juce::jlimit (0.0f, 1.0f, (float) value / mixScale);
                        }
                    break;
                default: break;
            }
        }
    }

    return juce::Result::ok();
}

juce::Result readPreset (const juce::File& file, Preset& out)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb))
        return juce::Result::fail ("Couldn't read " + file.getFullPathName());
    return readPreset (mb, out);
}

// .fst files use the same container as projects. A mixer track state holds plug-in events
// (201 name, 203 display name, 213 settings, 98 slot), the track name (204) and a 225 parameter block
// for its fader / pan / slot switches. Old presets (FL 3-8) skip the slot events and write ANSI text.
juce::Result readPreset (const juce::MemoryBlock& mb, Preset& out)
{
    out = Preset();
    const auto* b = (const juce::uint8*) mb.getData();
    const size_t n = mb.getSize();
    if (n < 22 || std::memcmp (b, "FLhd", 4) != 0)
        return juce::Result::fail ("This isn't an FL Studio preset.");
    const auto headerLen = rd32 (b + 4);
    if (headerLen < 6 || 8 + (size_t) headerLen + 8 > n)
        return juce::Result::fail ("The preset header is damaged.");
    const auto* chunk = b + 8 + headerLen;
    if (std::memcmp (chunk, "FLdt", 4) != 0)
        return juce::Result::fail ("The preset data is missing.");
    const auto dataLen = juce::jmin ((size_t) rd32 (chunk + 4), n - (size_t) (chunk + 8 - b));
    const auto* begin = chunk + 8;
    const auto* end = begin + dataLen;

    std::vector<Event> ev;
    if (! decode (begin, end, false, ev) && ! decode (begin, end, true, ev))
        return juce::Result::fail ("This FL Studio preset uses a layout AMETHYST can't read yet.");

    TextDecoder text;
    for (auto& e : ev)
        if (e.id == 199)
        {
            out.version = utf8 (e.data, e.size);
            const auto parts = juce::StringArray::fromTokens (out.version, ".", {});
            const int major = parts[0].getIntValue(), minor = parts[1].getIntValue();
            text.utf16 = major > 11 || (major == 11 && minor >= 5);
            break;
        }

    auto& insert = out.insert;
    std::optional<Plugin> pending;
    auto flushPlugin = [&] (int slot)
    {
        if (! pending) return;
        pending->slot = slot >= 0 ? slot : (int) insert.plugins.size();
        if (pending->internalName.isNotEmpty() || pending->vstName.isNotEmpty())
            insert.plugins.push_back (std::move (*pending));
        pending.reset();
    };

    const Event* mixerParams = nullptr;
    for (auto& e : ev)
    {
        switch (e.id)
        {
            case 201: flushPlugin (-1); pending = Plugin(); pending->internalName = text (e); break;
            case 203: if (pending) pending->displayName = text (e); break;
            case 213:
                if (pending && pending->isWrapper()) readWrapper (e, *pending);
                else if (pending)                    pending->state.replaceAll (e.data, e.size);
                break;
            case 98:  flushPlugin ((int) e.u32()); break;
            case 204: insert.name = text (e); out.isMixerState = true; break;
            case 225: mixerParams = &e; out.isMixerState = true; break;
            default: break;
        }
    }
    flushPlugin (-1);

    if (insert.plugins.empty())
        return juce::Result::fail ("This preset has no effects in it (instrument and channel presets can't be loaded onto a track).");
    if (insert.plugins.size() > 1)
        out.isMixerState = true;

    // One insert only, so its number is whatever the smallest record says.
    if (mixerParams != nullptr && mixerParams->size >= 12)
    {
        const auto* d = mixerParams->data;
        const size_t count = mixerParams->size / 12;
        int base = 1 << 20;
        for (size_t r = 0; r < count; ++r)
            base = juce::jmin (base, (int) (rd16 (d + r * 12 + 6) >> 6));

        // FL 3-5 stored the slot mix knob as 0..128, later versions as 0..12800
        float mixScale = 128.0f;
        for (size_t r = 0; r < count; ++r)
            if (d[r * 12 + 4] == 1 && rdI32 (d + r * 12 + 8) > 128)
                mixScale = 12800.0f;
        for (size_t r = 0; r < count; ++r)
        {
            const auto* rec = d + r * 12;
            const int cd = rd16 (rec + 6);
            if ((cd >> 6) != base)
                continue;
            const int id = rec[4], slot = cd & 0x3f, value = rdI32 (rec + 8);
            switch (id)
            {
                case 192: insert.volume = juce::jlimit (0.0f, 2.0f, (float) value / 12800.0f); out.hasLevels = true; break;
                case 193: insert.pan = juce::jlimit (-1.0f, 1.0f, (float) value / 6400.0f); out.hasLevels = true; break;
                case 0:
                case 1:
                    for (auto& p : insert.plugins)
                        if (p.slot == slot)
                        {
                            if (id == 0) p.enabled = value != 0;
                            else         p.mix = juce::jlimit (0.0f, 1.0f, (float) value / mixScale);
                        }
                    break;
                default: break;
            }
        }
    }
    return juce::Result::ok();
}

juce::File resolveSamplePath (const juce::String& stored, const juce::File& projectFile)
{
    if (stored.isEmpty())
        return {};

    juce::String path = stored;
    auto expand = [&path] (const juce::String& token, const juce::Array<juce::File>& candidates)
    {
        if (! path.containsIgnoreCase (token))
            return;
        const auto rest = path.fromFirstOccurrenceOf (token, false, true).trimCharactersAtStart ("\\/");
        for (auto& dir : candidates)
        {
            auto f = dir.getChildFile (rest.replaceCharacter ('\\', '/'));
            if (f.existsAsFile()) { path = f.getFullPathName(); return; }
        }
    };

    juce::Array<juce::File> factoryDirs;
   #if JUCE_WINDOWS
    for (auto root : { juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory),
                       juce::File::getSpecialLocation (juce::File::globalApplicationsDirectoryX86) })
    {
        auto il = root.getChildFile ("Image-Line");
        auto found = il.findChildFiles (juce::File::findDirectories, false, "FL Studio*");
        found.sort();
        for (int i = found.size(); --i >= 0;)
            factoryDirs.add (found[i]);
    }
   #elif JUCE_MAC
    for (auto& app : juce::File ("/Applications").findChildFiles (juce::File::findDirectories, false, "FL Studio*.app"))
        factoryDirs.add (app.getChildFile ("Contents/Resources/FL"));
   #endif
    expand ("%FLStudioFactoryData%", factoryDirs);

    juce::Array<juce::File> userDirs { juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Image-Line/FL Studio") };
    expand ("%FLStudioUserData%", userDirs);

    const auto direct = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    if (direct.existsAsFile())
        return direct;

    // Moved projects: look next to the .flp by file name.
    const auto fileName = path.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
    if (fileName.isNotEmpty() && projectFile != juce::File())
    {
        const auto dir = projectFile.getParentDirectory();
        for (auto f : { dir.getChildFile (fileName), dir.getChildFile ("Audio").getChildFile (fileName),
                        dir.getChildFile ("Samples").getChildFile (fileName), dir.getChildFile ("Recorded").getChildFile (fileName) })
            if (f.existsAsFile())
                return f;
    }
    return direct;
}
}
