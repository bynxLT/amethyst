#include "License.h"
#include "LicensePublicKey.h"

namespace License
{
namespace
{
    bool base64UrlDecode (const juce::String& in, juce::MemoryBlock& out)
    {
        auto s = in.replaceCharacter ('-', '+').replaceCharacter ('_', '/');
        while (s.length() % 4 != 0) s << "=";
        juce::MemoryOutputStream mo;
        if (! juce::Base64::convertFromBase64 (mo, s))
            return false;
        out = mo.getMemoryBlock();
        return true;
    }

    // big-endian bytes <-> BigInteger (JUCE's memory-block form is little-endian)
    juce::BigInteger fromBigEndian (const juce::MemoryBlock& be)
    {
        juce::MemoryBlock le (be.getSize());
        for (size_t i = 0; i < be.getSize(); ++i)
            static_cast<juce::uint8*> (le.getData())[i] = static_cast<const juce::uint8*> (be.getData())[be.getSize() - 1 - i];
        juce::BigInteger b;
        b.loadFromMemoryBlock (le);
        return b;
    }

    std::vector<juce::uint8> toBigEndian (const juce::BigInteger& b, size_t length)
    {
        const auto le = b.toMemoryBlock();
        std::vector<juce::uint8> out (length, 0);
        for (size_t i = 0; i < le.getSize() && i < length; ++i)
            out[length - 1 - i] = static_cast<const juce::uint8*> (le.getData())[i];
        return out;
    }
}

Info verify (const juce::String& token, const char* modulusHex, int exponent)
{
    Info info;
    const int dot = token.indexOfChar ('.');
    if (dot <= 0 || dot == token.length() - 1)
    {
        info.error = "not a license";
        return info;
    }
    const auto payloadB64 = token.substring (0, dot);
    juce::MemoryBlock sig;
    if (! base64UrlDecode (token.substring (dot + 1), sig) || sig.getSize() < 64)
    {
        info.error = "bad signature encoding";
        return info;
    }

    juce::BigInteger n;
    n.parseString (modulusHex != nullptr ? modulusHex : LicensePublicKey::modulusHex, 16);
    const size_t k = (size_t) ((n.getHighestBit() + 8) / 8);
    if (sig.getSize() != k)
    {
        info.error = "signature has the wrong length";
        return info;
    }
    auto m = fromBigEndian (sig);
    if (m.compareAbsolute (n) >= 0)
    {
        info.error = "signature out of range";
        return info;
    }
    m.exponentModulo (juce::BigInteger ((juce::int64) exponent), n);
    const auto em = toBigEndian (m, k);

    // EMSA-PKCS1-v1_5 with SHA-256: 00 01 FF..FF 00 || DigestInfo || hash
    static const juce::uint8 digestInfo[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    const auto utf8 = payloadB64.toUTF8();
    const juce::SHA256 sha (utf8.getAddress(), std::strlen (utf8.getAddress()));
    const auto hash = sha.getRawData();
    const size_t tLen = sizeof (digestInfo) + 32;
    std::vector<juce::uint8> expected (k, 0xff);
    expected[0] = 0x00;
    expected[1] = 0x01;
    expected[k - tLen - 1] = 0x00;
    std::memcpy (expected.data() + (k - tLen), digestInfo, sizeof (digestInfo));
    std::memcpy (expected.data() + (k - 32), hash.getData(), 32);
    juce::uint8 diff = 0;
    for (size_t i = 0; i < k; ++i)
        diff |= (juce::uint8) (expected[i] ^ em[i]);
    if (diff != 0)
    {
        info.error = "signature doesn't match";
        return info;
    }

    juce::MemoryBlock payload;
    if (! base64UrlDecode (payloadB64, payload))
    {
        info.error = "bad payload encoding";
        return info;
    }
    const auto json = juce::JSON::parse (payload.toString());
    if (! json.isObject() || (int) json.getProperty ("v", 0) != 1)
    {
        info.error = "unknown license format";
        return info;
    }
    info.sub = json.getProperty ("sub", {}).toString();
    info.plan = json.getProperty ("plan", "free").toString();
    info.tier = juce::jlimit (0, 2, (int) json.getProperty ("tier", 0));
    info.proUntil = (juce::int64) json.getProperty ("pro_until", 0);
    info.iat = (juce::int64) json.getProperty ("iat", 0);
    info.exp = (juce::int64) json.getProperty ("exp", 0);
    info.valid = info.sub.isNotEmpty() && info.iat > 0 && info.exp > info.iat;
    if (! info.valid)
        info.error = "incomplete license";
    return info;
}

Check evaluate (const Info& lic, juce::int64 nowMs, juce::int64 latestSeenMs, const juce::String& expectedUser)
{
    Check c;
    if (! lic.valid)
    {
        c.state = State::Invalid;
        c.reason = "No valid license yet - go online and sign in to check your plan.";
        return c;
    }
    if (expectedUser.isNotEmpty() && lic.sub != expectedUser)
    {
        c.state = State::WrongAccount;
        c.reason = "This license belongs to another account.";
        return c;
    }
    if (nowMs + kClockTolerance < juce::jmax (latestSeenMs, lic.iat))
    {
        c.state = State::ClockRolledBack;
        c.reason = "Your PC's clock looks wrong (it's behind the last time AMETHYST checked your plan). "
                   "Set the date and time correctly, then click Refresh status.";
        return c;
    }
    if (lic.proUntil > 0 && nowMs > lic.proUntil)
    {
        c.state = State::Lapsed;
        c.reason = "Your subscription period has ended.";
        return c;
    }
    if (nowMs > lic.exp)
    {
        c.state = State::Expired;
        c.reason = "It's been a while since AMETHYST could check your plan - go online once to keep your features.";
        return c;
    }
    c.state = State::Active;
    c.tier = lic.tier;
    return c;
}
}
