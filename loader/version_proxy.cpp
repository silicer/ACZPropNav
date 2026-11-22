// ACZPropNav - VERSION.dll proxy loader (alternative injection vector).
//
// acz.exe imports three named exports from VERSION.dll (GetFileVersionInfoW,
// GetFileVersionInfoSizeW, VerQueryValueW). VERSION.dll is *not* a KnownDLL on
// current Windows, so a VERSION.dll placed next to acz.exe wins the load -- the
// same trick as the XINPUT1_4 proxy, with an even smaller call surface.
//
// It forwards the whole 17-entry export table of the real DLL: the three
// functions the game uses are plain lazy forwarders with checked signatures
// (from winver.h), the low-traffic/awkward ones (variadic-ish 8-argument
// VerFindFile/VerInstallFile, the Ex variants, the forwarded VerLanguageName
// pair) are argument-transparent `jmp real` thunks.
//
// The mod itself is injected exactly as in the XInput proxy: ACZPropNav.dll is
// loaded from this DLL's directory and AczCoreRun() is called.
#include <windows.h>

#include "proxy_common.h"

// ---------------------------------------------------------------------------
// Lazy caches for the functions acz.exe actually imports.
// ---------------------------------------------------------------------------
static void* g_slotGetFileVersionInfoW = nullptr;
static void* g_slotGetFileVersionInfoA = nullptr;
static void* g_slotGetFileVersionInfoSizeW = nullptr;
static void* g_slotGetFileVersionInfoSizeA = nullptr;
static void* g_slotVerQueryValueW = nullptr;
static void* g_slotVerQueryValueA = nullptr;

// ---------------------------------------------------------------------------
// Argument-transparent thunks for the remaining exports.
//
// The thunk only touches rax (caller-saved, never an argument register), so
// rcx/rdx/r8/r9 and xmm0-3 -- and the caller's stack arguments and shadow
// space -- reach the real function untouched. If the slot was never primed the
// thunk returns 0 (FALSE) rather than jumping to null.
// ---------------------------------------------------------------------------
#define ACZ_VERSION_THUNK(sym, slot)                                                 \
    extern "C" void* slot = nullptr;                                                 \
    extern "C" __attribute__((naked, noinline)) void sym(void) {                     \
        __asm__ __volatile__(                                                        \
            "mov " #slot "(%rip), %rax\n\t"                                          \
            "test %rax, %rax\n\t"                                                    \
            "je 1f\n\t"                                                              \
            "jmp *%rax\n\t"                                                          \
            "1:\n\t"                                                                 \
            "xor %eax, %eax\n\t"                                                     \
            "ret\n\t");                                                              \
    }

ACZ_VERSION_THUNK(VerFwd_GetFileVersionInfoByHandle, g_slotGetFileVersionInfoByHandle)
ACZ_VERSION_THUNK(VerFwd_GetFileVersionInfoExA, g_slotGetFileVersionInfoExA)
ACZ_VERSION_THUNK(VerFwd_GetFileVersionInfoExW, g_slotGetFileVersionInfoExW)
ACZ_VERSION_THUNK(VerFwd_GetFileVersionInfoSizeExA, g_slotGetFileVersionInfoSizeExA)
ACZ_VERSION_THUNK(VerFwd_GetFileVersionInfoSizeExW, g_slotGetFileVersionInfoSizeExW)
ACZ_VERSION_THUNK(VerFwd_VerFindFileA, g_slotVerFindFileA)
ACZ_VERSION_THUNK(VerFwd_VerFindFileW, g_slotVerFindFileW)
ACZ_VERSION_THUNK(VerFwd_VerInstallFileA, g_slotVerInstallFileA)
ACZ_VERSION_THUNK(VerFwd_VerInstallFileW, g_slotVerInstallFileW)
ACZ_VERSION_THUNK(VerFwd_VerLanguageNameA, g_slotVerLanguageNameA)
ACZ_VERSION_THUNK(VerFwd_VerLanguageNameW, g_slotVerLanguageNameW)

// ---------------------------------------------------------------------------
// Checked forwarders for the hot exports. Signatures intentionally match
// winver.h, so the compiler verifies them against the real declarations.
// ---------------------------------------------------------------------------
extern "C" BOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle,
                                           DWORD dwLen, LPVOID lpData) {
    typedef BOOL(WINAPI * Fn)(LPCWSTR, DWORD, DWORD, LPVOID);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotGetFileVersionInfoW, 9, "GetFileVersionInfoW"));
    if (fn == nullptr) return FALSE;
    return fn(lptstrFilename, dwHandle, dwLen, lpData);
}

extern "C" BOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle,
                                           DWORD dwLen, LPVOID lpData) {
    typedef BOOL(WINAPI * Fn)(LPCSTR, DWORD, DWORD, LPVOID);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotGetFileVersionInfoA, 1, "GetFileVersionInfoA"));
    if (fn == nullptr) return FALSE;
    return fn(lptstrFilename, dwHandle, dwLen, lpData);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
    typedef DWORD(WINAPI * Fn)(LPCWSTR, LPDWORD);
    Fn fn = reinterpret_cast<Fn>(aczproxy::ExportCached(&g_slotGetFileVersionInfoSizeW, 8,
                                                        "GetFileVersionInfoSizeW"));
    if (fn == nullptr) return 0;
    return fn(lptstrFilename, lpdwHandle);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
    typedef DWORD(WINAPI * Fn)(LPCSTR, LPDWORD);
    Fn fn = reinterpret_cast<Fn>(aczproxy::ExportCached(&g_slotGetFileVersionInfoSizeA, 5,
                                                        "GetFileVersionInfoSizeA"));
    if (fn == nullptr) return 0;
    return fn(lptstrFilename, lpdwHandle);
}

extern "C" BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock,
                                      LPVOID* lplpBuffer, PUINT puLen) {
    typedef BOOL(WINAPI * Fn)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotVerQueryValueW, 17, "VerQueryValueW"));
    if (fn == nullptr) return FALSE;
    return fn(pBlock, lpSubBlock, lplpBuffer, puLen);
}

extern "C" BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock,
                                      LPVOID* lplpBuffer, PUINT puLen) {
    typedef BOOL(WINAPI * Fn)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotVerQueryValueA, 16, "VerQueryValueA"));
    if (fn == nullptr) return FALSE;
    return fn(pBlock, lpSubBlock, lplpBuffer, puLen);
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

// Prime every thunk slot from the real DLL (by export name -- all 17 exports
// of VERSION.dll are named, so no ordinal lookups are needed here).
static void PrimeVersionSlots() {
    struct Item {
        const char* name;
        void* volatile* slot;
    };
    const Item items[] = {
        {"GetFileVersionInfoByHandle", &g_slotGetFileVersionInfoByHandle},
        {"GetFileVersionInfoExA", &g_slotGetFileVersionInfoExA},
        {"GetFileVersionInfoExW", &g_slotGetFileVersionInfoExW},
        {"GetFileVersionInfoSizeExA", &g_slotGetFileVersionInfoSizeExA},
        {"GetFileVersionInfoSizeExW", &g_slotGetFileVersionInfoSizeExW},
        {"VerFindFileA", &g_slotVerFindFileA},
        {"VerFindFileW", &g_slotVerFindFileW},
        {"VerInstallFileA", &g_slotVerInstallFileA},
        {"VerInstallFileW", &g_slotVerInstallFileW},
        {"VerLanguageNameA", &g_slotVerLanguageNameA},
        {"VerLanguageNameW", &g_slotVerLanguageNameW},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        void* p = aczproxy::Export(0, items[i].name);
        if (p != nullptr) InterlockedExchangePointer(items[i].slot, p);
    }
}

static DWORD WINAPI LoaderThread(LPVOID) {
    aczproxy::RunWorker(L"ACZPropNav.dll");
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return aczproxy::HandleDllMain(hinstDLL, fdwReason, lpvReserved, &LoaderThread,
                                   L"version.dll", &PrimeVersionSlots);
}
