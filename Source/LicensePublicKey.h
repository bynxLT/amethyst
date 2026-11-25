#pragma once

// Public half of the AMETHYST license signing key (made by tools/make-license-keys.js).
// Public on purpose: it can only CHECK signatures. The private half lives in Supabase secrets.
namespace LicensePublicKey
{
    inline constexpr const char* modulusHex =
        "901e2ac672224e6172490dd182579188238d2bcf56476d14725ce1180460ec3a156ecf3f579d75067452f69aaf449dbb"
        "dfc22f1a439c6b68f490f502815bf08bda6cc8dd9fb3af0c5113f87c3a6ed3977cff28efcc6c9016c0d00fd2e844fc7f"
        "04a76f80e603dcf6cfb829ab772c9eacc3599f0a967c5f70f8f9ad7a824f09b44b864688e1377f663776a2260cf64e91"
        "10e39d9247fb68f1d0f602b43c8fd9fd53f4e399502679239ea491af14d46abaa9eb2dc7999474fad60bf5de747e8d53"
        "87df88e970c7518dc05eb6eefaf1ce4cf9636551d70efe450d61eff6920b1abad1bf5c46475d0f1af6045653e99b672f"
        "bfc3c57d4ad82b4756e75c6604d290bb";
    inline constexpr int exponent = 65537;
}
