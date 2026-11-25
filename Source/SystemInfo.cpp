// Platform-specific helpers, kept separate from JUCE headers so system headers can't clash with JUCE names.
#include <cstddef>
#include <cstring>
#include <cwchar>
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
 #include <dwmapi.h>
 #pragma comment (lib, "dwmapi.lib")
#elif defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
 #include <Security/Security.h>
 #include <mach/mach.h>
 #include <sys/socket.h>
 #include <sys/un.h>
 #include <unistd.h>
 #include <fcntl.h>
 #include <cstdlib>
 #include <string>
 #include <cerrno>
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

// Space pressed while a plug-in editor (a window owned by AMETHYST's main window) has keyboard focus
// goes to the plug-in's own native window, which ignores it. This thread message hook spots that key
// before it's dispatched, swallows it, and calls `handler` instead, so play/stop/record-stop always work.
// Text boxes that are real Win32 edit controls keep their spaces.
#if defined (_WIN32)
namespace
{
    HHOOK gKeyHook = nullptr;
    HWND gMainWindow = nullptr;
    void (*gKeyHandler) (void*) = nullptr;
    void* gKeyContext = nullptr;

    bool isPluginWindowKey (const MSG& msg)
    {
        if (msg.wParam != VK_SPACE || msg.hwnd == nullptr || gMainWindow == nullptr)
            return false;
        if ((GetKeyState (VK_CONTROL) | GetKeyState (VK_MENU) | GetKeyState (VK_SHIFT)) & 0x8000)
            return false;
        const HWND root = GetAncestor (msg.hwnd, GA_ROOT);
        if (root == nullptr || root == gMainWindow || GetWindow (root, GW_OWNER) != gMainWindow)
            return false;
        wchar_t cls[64] = {};
        GetClassNameW (msg.hwnd, cls, 63);
        return std::wcsstr (cls, L"Edit") == nullptr && std::wcsstr (cls, L"EDIT") == nullptr;
    }

    LRESULT CALLBACK keyHookProc (int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && wParam == PM_REMOVE)
        {
            auto* msg = (MSG*) lParam;
            if ((msg->message == WM_KEYDOWN || msg->message == WM_KEYUP) && isPluginWindowKey (*msg))
            {
                const bool firstPress = msg->message == WM_KEYDOWN && (msg->lParam & (1 << 30)) == 0;
                msg->message = WM_NULL;   // the plug-in never sees it
                if (firstPress && gKeyHandler != nullptr)
                    gKeyHandler (gKeyContext);
            }
        }
        return CallNextHookEx (gKeyHook, code, wParam, lParam);
    }
}
#endif

bool amethystInstallPluginKeyHook (void* mainWindow, void (*handler) (void*), void* context)
{
   #if defined (_WIN32)
    gMainWindow = (HWND) mainWindow;
    gKeyHandler = handler;
    gKeyContext = context;
    if (gKeyHook == nullptr)
        gKeyHook = SetWindowsHookExW (WH_GETMESSAGE, keyHookProc, nullptr, GetCurrentThreadId());
    return gKeyHook != nullptr;
   #else
    (void) mainWindow; (void) handler; (void) context;
    return false;
   #endif
}

void amethystRemovePluginKeyHook()
{
   #if defined (_WIN32)
    if (gKeyHook != nullptr)
        UnhookWindowsHookEx (gKeyHook);
    gKeyHook = nullptr;
    gKeyHandler = nullptr;
    gKeyContext = nullptr;
   #endif
}

// Windows 10/11: dark title bar, and on Windows 11 a custom caption / text / border colour.
// Colours are 0xRRGGBB. Harmless no-op elsewhere or on older systems.
void amethystStyleTitleBar (void* window, unsigned int caption, unsigned int text, unsigned int border)
{
   #if defined (_WIN32)
    if (window == nullptr)
        return;
    auto hwnd = (HWND) window;
    auto bgr = [] (unsigned int rgb) { return (COLORREF) (((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff)); };
    const BOOL dark = TRUE;
    DwmSetWindowAttribute (hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof (dark));
    const COLORREF c = bgr (caption), t = bgr (text), b = bgr (border);
    DwmSetWindowAttribute (hwnd, 35 /* DWMWA_CAPTION_COLOR */, &c, sizeof (c));
    DwmSetWindowAttribute (hwnd, 36 /* DWMWA_TEXT_COLOR */, &t, sizeof (t));
    DwmSetWindowAttribute (hwnd, 34 /* DWMWA_BORDER_COLOR */, &b, sizeof (b));
   #else
    (void) window; (void) caption; (void) text; (void) border;
   #endif
}

//==============================================================================
// Local IPC to the Discord desktop app (Rich Presence): Windows named pipe "discord-ipc-N",
// macOS unix socket "<tmp>/discord-ipc-N". Returns an opaque handle or nullptr.
void* amethystIpcConnect()
{
   #if defined (_WIN32)
    for (int i = 0; i < 10; ++i)
    {
        wchar_t name[64];
        std::swprintf (name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW (name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            return (void*) h;
    }
    return nullptr;
   #elif defined (__APPLE__)
    const char* base = std::getenv ("TMPDIR");
    std::string dir = base != nullptr ? base : "/tmp/";
    if (! dir.empty() && dir.back() != '/') dir += "/";
    for (int i = 0; i < 10; ++i)
    {
        const std::string path = dir + "discord-ipc-" + std::to_string (i);
        int fd = socket (AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy (addr.sun_path, path.c_str(), sizeof (addr.sun_path) - 1);
        if (connect (fd, (sockaddr*) &addr, sizeof (addr)) == 0)
        {
            fcntl (fd, F_SETFL, fcntl (fd, F_GETFL, 0) | O_NONBLOCK);
            return (void*) (intptr_t) (fd + 1);   // +1 so fd 0 is never "null"
        }
        close (fd);
    }
    return nullptr;
   #else
    return nullptr;
   #endif
}

bool amethystIpcWrite (void* handle, const void* data, std::size_t size)
{
   #if defined (_WIN32)
    DWORD written = 0;
    return handle != nullptr && WriteFile ((HANDLE) handle, data, (DWORD) size, &written, nullptr) && written == (DWORD) size;
   #elif defined (__APPLE__)
    if (handle == nullptr) return false;
    const int fd = (int) (intptr_t) handle - 1;
    std::size_t done = 0;
    while (done < size)
    {
        const auto n = send (fd, (const char*) data + done, size - done, 0);
        if (n <= 0) return false;
        done += (std::size_t) n;
    }
    return true;
   #else
    (void) handle; (void) data; (void) size;
    return false;
   #endif
}

// Reads whatever is waiting without blocking: bytes read, 0 if nothing, -1 if the connection is gone.
int amethystIpcRead (void* handle, void* buffer, std::size_t size)
{
   #if defined (_WIN32)
    if (handle == nullptr) return -1;
    DWORD avail = 0;
    if (! PeekNamedPipe ((HANDLE) handle, nullptr, 0, nullptr, &avail, nullptr)) return -1;
    if (avail == 0) return 0;
    DWORD got = 0;
    if (! ReadFile ((HANDLE) handle, buffer, (DWORD) (avail < size ? avail : size), &got, nullptr)) return -1;
    return (int) got;
   #elif defined (__APPLE__)
    if (handle == nullptr) return -1;
    const auto n = recv ((int) (intptr_t) handle - 1, buffer, size, 0);
    if (n > 0) return (int) n;
    if (n == 0) return -1;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
   #else
    (void) handle; (void) buffer; (void) size;
    return -1;
   #endif
}

void amethystIpcClose (void* handle)
{
   #if defined (_WIN32)
    if (handle != nullptr) CloseHandle ((HANDLE) handle);
   #elif defined (__APPLE__)
    if (handle != nullptr) close ((int) (intptr_t) handle - 1);
   #else
    (void) handle;
   #endif
}

int amethystProcessId()
{
   #if defined (_WIN32)
    return (int) GetCurrentProcessId();
   #elif defined (__APPLE__)
    return (int) getpid();
   #else
    return 0;
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
