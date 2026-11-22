// ACZPropNav - shared forwarding core for the DLL proxies (header-only).
//
// A proxy DLL is placed next to acz.exe, gets loaded by the Windows loader
// because the game imports the *real* DLL name, and must:
//   1. never answer a call itself -- it forwards every export to the genuine
//      system DLL (loaded by absolute path from %SystemRoot%\System32, so we can
//      never recurse into ourselves), and
//   2. inject our mod: ACZPropNav.dll from our own directory, then call its
//      single export AczCoreRun().
//
// Everything here has internal linkage (namespace + static), because only one
// proxy translation unit is compiled into each proxy DLL, and because it keeps
// the link free of CRT/STL dependencies.
//
// Threading model: DllMain(PROCESS_ATTACH) starts one worker thread and returns
// immediately (never block the loader lock). The worker resolves the real DLL
// and loads the mod. Exported functions additionally resolve *lazily* on first
// use, so a call that arrives before the worker finished is still forwarded
// correctly instead of failing.
#pragma once

#include <windows.h>

#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif

namespace aczproxy {

constexpr DWORD kMaxPath = 1024;

// Identity marker embedded in every proxy image. It lets a human (or a script)
// recognise "the proxy this project installed" with a plain `findstr`/`grep`,
// even after a rebuild changed the binary byte-for-byte. The newlines make it a
// stand-alone line, so line-oriented search tools match it inside the binary.
// A genuine Microsoft XInput1_4.dll / version.dll never contains it.
static const char kAczProxyIdentity[] __attribute__((used)) = "\nACZPropNav proxy loader\n";

// ---- state ---------------------------------------------------------------
// Zero-initialised .bss / .data: no runtime initialisers, no C++ guards.
static HMODULE g_realModule = nullptr;      // the genuine DLL from System32
static SRWLOCK g_initLock = SRWLOCK_INIT;   // guards g_realModule / path setup
static wchar_t g_realPath[kMaxPath] = {};   // %SystemRoot%\System32\<real>.dll
static wchar_t g_selfDir[kMaxPath] = {};    // directory of this proxy DLL
static const wchar_t* g_realDllName = nullptr;  // e.g. L"XInput1_4.dll"
static HMODULE g_selfModule = nullptr;
static volatile LONG g_shuttingDown = 0;

// Worker-thread plumbing, shared by every proxy.
static HANDLE g_workerHandle = nullptr;
static HANDLE g_workerDone = nullptr;
typedef void (*VoidFn)();
static VoidFn g_workerHook = nullptr;  // optional extra priming step

// ---- tiny string helpers (kernel32 only, no CRT) --------------------------
inline bool CopyW(wchar_t* dst, DWORD cch, const wchar_t* src) {
    if (dst == nullptr || src == nullptr || cch == 0) return false;
    const int len = lstrlenW(src);
    if (len < 0 || static_cast<DWORD>(len) >= cch) {
        dst[0] = L'\0';
        return false;
    }
    lstrcpynW(dst, src, static_cast<int>(cch));
    return true;
}

inline bool AppendW(wchar_t* dst, DWORD cch, const wchar_t* suffix) {
    if (dst == nullptr || suffix == nullptr) return false;
    const int have = lstrlenW(dst);
    const int add = lstrlenW(suffix);
    if (have < 0 || add < 0) return false;
    if (static_cast<DWORD>(have + add) >= cch) return false;  // would truncate
    lstrcpynW(dst + have, suffix, static_cast<int>(cch - static_cast<DWORD>(have)));
    return true;
}

// out = dir + "\" + file
inline bool JoinPath(wchar_t* out, DWORD cch, const wchar_t* dir, const wchar_t* file) {
    if (!CopyW(out, cch, dir)) return false;
    return AppendW(out, cch, L"\\") && AppendW(out, cch, file);
}

// Directory part of the module that contains `module`.
inline bool ComputeSelfDir(HMODULE module, wchar_t* out, DWORD cch) {
    wchar_t full[kMaxPath];
    const DWORD n = GetModuleFileNameW(module, full, kMaxPath);
    if (n == 0 || n >= kMaxPath) return false;
    wchar_t* lastSlash = nullptr;
    for (wchar_t* p = full; *p != L'\0'; ++p)
        if (*p == L'\\' || *p == L'/') lastSlash = p;
    if (lastSlash == nullptr) return false;
    *lastSlash = L'\0';
    return CopyW(out, cch, full);
}

// Build "%SystemRoot%\System32\<g_realDllName>" into g_realPath.
inline void BuildRealPathLocked() {
    if (g_realDllName == nullptr) return;
    wchar_t sysDir[kMaxPath];
    const UINT n = GetSystemDirectoryW(sysDir, kMaxPath);
    if (n == 0 || n >= kMaxPath) return;
    if (!JoinPath(g_realPath, kMaxPath, sysDir, g_realDllName)) g_realPath[0] = L'\0';
}

// ---- the real DLL --------------------------------------------------------
// Loads the genuine system DLL exactly once, by absolute path. Safe to call
// from any thread, including before the worker thread ran.
inline HMODULE Real() {
    HMODULE m = static_cast<HMODULE>(
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_realModule),
                                          nullptr, nullptr));
    if (m != nullptr) return m;
    AcquireSRWLockExclusive(&g_initLock);
    m = g_realModule;
    if (m == nullptr) {
        if (g_realPath[0] == L'\0') BuildRealPathLocked();
        if (g_realPath[0] != L'\0') m = LoadLibraryW(g_realPath);
        if (m != nullptr)
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_realModule), m);
    }
    ReleaseSRWLockExclusive(&g_initLock);
    return m;
}

// GetProcAddress on the real DLL: by name when `name` is given, else by ordinal.
inline void* Export(WORD ordinal, const char* name) {
    HMODULE m = Real();
    if (m == nullptr) return nullptr;
    if (name != nullptr) return reinterpret_cast<void*>(GetProcAddress(m, name));
    return reinterpret_cast<void*>(
        GetProcAddress(m, reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(ordinal))));
}

// Lazily resolve + cache one export in `*slot`.
inline void* ExportCached(void* volatile* slot, WORD ordinal, const char* name) {
    if (slot == nullptr) return nullptr;
    void* p = *slot;
    if (p != nullptr) return p;
    p = Export(ordinal, name);
    if (p != nullptr) InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), p);
    return p;
}

// ---- mod injection -------------------------------------------------------
// Loads ACZPropNav.dll from our own directory and calls its entry point
// AczCoreRun(HMODULE self). Silently does nothing when the mod is absent (that
// is a supported configuration: a bare proxy must not crash the game).
inline void InjectMod(const wchar_t* modDllName) {
    if (modDllName == nullptr || g_selfDir[0] == L'\0') return;
    wchar_t modPath[kMaxPath];
    if (!JoinPath(modPath, kMaxPath, g_selfDir, modDllName)) return;
    HMODULE mod = LoadLibraryW(modPath);
    if (mod == nullptr) return;  // mod not installed -> proxy keeps working
    typedef void (*AczCoreRunFn)(void*);
    AczCoreRunFn run = reinterpret_cast<AczCoreRunFn>(GetProcAddress(mod, "AczCoreRun"));
    if (run != nullptr) run(reinterpret_cast<void*>(mod));
}

// Worker-thread body, in this order:
//   1. load the genuine system DLL (absolute path, so we cannot recurse),
//   2. prime any ordinal-only export slots the proxy needs,
//   3. only then inject the mod, so the mod's code never observes a half
//      initialised proxy.
inline void RunWorker(const wchar_t* modDllName) {
    ComputeSelfDir(g_selfModule, g_selfDir, kMaxPath);
    Real();
    if (g_shuttingDown == 0 && g_workerHook != nullptr) g_workerHook();
    if (g_shuttingDown == 0) InjectMod(modDllName);
    if (g_workerDone != nullptr) SetEvent(g_workerDone);
}

// Common DllMain body for a proxy. `proc` is the proxy's worker thread entry
// point (also used as a "code address inside this module" for pinning), and
// `afterLoad` optionally primes ordinal-only export slots.
inline BOOL HandleDllMain(HMODULE self, DWORD reason, LPVOID reserved,
                          LPTHREAD_START_ROUTINE proc, const wchar_t* realDllName,
                          VoidFn afterLoad) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_selfModule = self;
            g_realDllName = realDllName;
            g_workerHook = afterLoad;
            DisableThreadLibraryCalls(self);
            // Pin ourselves into the process. A proxy that is unloaded while the
            // game (or another thread) is inside one of our forwarders would
            // crash; pinning also makes an accidental FreeLibrary harmless.
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(proc), &pinned);
            g_workerDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_workerHandle = CreateThread(nullptr, 0, proc, nullptr, 0, nullptr);
            return TRUE;
        }
        case DLL_PROCESS_DETACH: {
            // Only reachable via an explicit FreeLibrary (the pin normally makes
            // that impossible). Let the worker finish, then drop its handles.
            if (reserved == nullptr) {
                InterlockedExchange(&g_shuttingDown, 1);
                if (g_workerDone != nullptr) WaitForSingleObject(g_workerDone, 2000);
                if (g_workerHandle != nullptr) {
                    CloseHandle(g_workerHandle);
                    g_workerHandle = nullptr;
                }
                if (g_workerDone != nullptr) {
                    CloseHandle(g_workerDone);
                    g_workerDone = nullptr;
                }
            }
            return TRUE;
        }
        default:
            return TRUE;
    }
}

}  // namespace aczproxy
