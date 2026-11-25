#pragma once

#include <JuceHeader.h>

// Signed AMETHYST licenses.
//
// The "license" Edge Function signs {v, sub, tier, plan, pro_until, iat, exp} with the private key that only
// the server has (RSASSA-PKCS1-v1_5 + SHA-256 over the base64url payload). The token is
// "<base64url payload>.<base64url signature>". The app checks it with the public key in LicensePublicKey.h,
// so an edited cache, a copied license from another account, or a made-up one never verifies.
//
// Times are server milliseconds. iat = when the server issued it, exp = how long it works offline.
namespace License
{
    struct Info
    {
        bool valid = false;
        juce::String error;               // why it didn't verify
        juce::String sub, plan;           // user id, plan name
        int tier = 0;                     // 0 Free, 1 Pro, 2 Ultra Pro
        juce::int64 proUntil = 0, iat = 0, exp = 0;
    };

    // Verifies the signature (with the built-in public key unless another modulus is given) and parses it.
    Info verify (const juce::String& token, const char* modulusHex = nullptr, int exponent = 65537);

    enum class State { Active, Invalid, WrongAccount, Expired, Lapsed, ClockRolledBack };

    struct Check
    {
        State state = State::Invalid;
        int tier = 0;                     // the tier to grant right now (0 unless Active)
        juce::String reason;              // shown to the user when not Active
    };

    // What the license allows at `nowMs`. latestSeenMs = the latest time this PC has seen (saved with the
    // account); a clock more than 10 minutes behind it, or behind the server's issue time, counts as rolled back.
    Check evaluate (const Info&, juce::int64 nowMs, juce::int64 latestSeenMs, const juce::String& expectedUser);

    constexpr juce::int64 kClockTolerance = 10 * 60 * 1000;
}
