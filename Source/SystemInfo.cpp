// Platform-specific helpers, kept separate from JUCE headers so system headers can't clash with JUCE names.
#include <cstddef>
#include <cstring>
#include <vector>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #include <wincrypt.h>
 #pragma comment (lib, "psapi.lib")
 #pragma comment (lib, "crypt32.lib")
#elif defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
 #include <Security/Security.h>
 #include <mach/mach.h>
#endif

std::size_t vocaldawProcessMemoryMB()
{
   #if defined (_WIN32)
    PROCESS_MEMORY_COUNTERS pmc {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
        return (std::size_t) (pmc.WorkingSetSize / (1024 * 1024));
   #elif defined (__APPLE__)
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (std::size_t) (info.phys_footprint / (1024 * 1024));
   #endif
    return 0;
}

// Make `child` an owned window of `owner` (Windows): it then always stays above the owner,
// minimises with it, and doesn't float over other apps. Returns false where unsupported.
bool amethystSetWindowOwner (void* child, void* owner)
{
   #if defined (_WIN32)
    if (child == nullptr || owner == nullptr)
        return false;
    SetWindowLongPtrW ((HWND) child, GWLP_HWNDPARENT, (LONG_PTR) owner);
    return GetWindow ((HWND) child, GW_OWNER) == (HWND) owner;
   #else
    (void) child; (void) owner;
    return false;
   #endif
}

#if defined (__APPLE__)
namespace
{
    // Keychain item that holds the session. The "encrypted blob" handed back to the
    // app is just a marker; the secret itself never leaves the Keychain.
    const unsigned char kKeychainMarker[] = { 'k', 'c', '1' };

    CFMutableDictionaryRef keychainQuery()
    {
        auto q = CFDictionaryCreateMutable (nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue (q, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue (q, kSecAttrService, CFSTR ("com.amethyst.daw"));
        CFDictionarySetValue (q, kSecAttrAccount, CFSTR ("session"));
        return q;
    }
}
#endif

// Protect the saved sign-in session for the current OS user:
//   Windows: DPAPI encryption.   macOS: stored in the login Keychain.
bool amethystProtect (const void* data, std::size_t size, std::vector<unsigned char>& out, bool encrypt)
{
   #if defined (_WIN32)
    DATA_BLOB in { (DWORD) size, (BYTE*) data };
    DATA_BLOB result {};
    static const char entropyText[] = "AMETHYST-session-v1";
    DATA_BLOB entropy { (DWORD) sizeof (entropyText), (BYTE*) entropyText };

    const BOOL ok = encrypt ? CryptProtectData (&in, L"AMETHYST", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result)
                            : CryptUnprotectData (&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (! ok)
        return false;

    out.assign (result.pbData, result.pbData + result.cbData);
    LocalFree (result.pbData);
    return true;

   #elif defined (__APPLE__)
    if (encrypt)
    {
        auto query = keychainQuery();
        SecItemDelete (query); // replace any previous session

        auto value = CFDataCreate (nullptr, (const UInt8*) data, (CFIndex) size);
        CFDictionarySetValue (query, kSecValueData, value);
        CFDictionarySetValue (query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        const OSStatus status = SecItemAdd (query, nullptr);
        CFRelease (value);
        CFRelease (query);

        if (status != errSecSuccess)
            return false;
        out.assign (kKeychainMarker, kKeychainMarker + sizeof (kKeychainMarker));
        return true;
    }

    if (size != sizeof (kKeychainMarker) || std::memcmp (data, kKeychainMarker, size) != 0)
        return false;

    auto query = keychainQuery();
    CFDictionarySetValue (query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue (query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching (query, &result);
    CFRelease (query);

    if (status != errSecSuccess || result == nullptr)
        return false;

    auto bytes = (CFDataRef) result;
    const auto* p = CFDataGetBytePtr (bytes);
    out.assign (p, p + CFDataGetLength (bytes));
    CFRelease (result);
    return true;

   #else
    (void) data; (void) size; (void) out; (void) encrypt;
    return false;
   #endif
}
