// ACZPropNav - XINPUT1_4.dll proxy loader (the recommended injection vector).
//
// acz.exe statically imports XINPUT1_4.dll by ORDINAL, and only ordinals 2
// (XInputGetState) and 3 (XInputSetState). Windows resolves those imports from
// the application directory first (XINPUT1_4 is not a KnownDLL), so this file
// -- built as build\XINPUT1_4.dll and copied next to acz.exe -- gets loaded
// into the game. It then:
//   * loads the genuine C:\Windows\System32\XInput1_4.dll by absolute path and
//     forwards every export to it (lazily, cached), and
//   * loads ACZPropNav.dll from its own directory and calls AczCoreRun().
//
// Every exported function is a pure forwarder: no game-visible behaviour is
// changed if the mod is absent, broken or still loading.
#include <windows.h>

#include "proxy_common.h"

// ---------------------------------------------------------------------------
// Lazy export caches. One slot per forwarded function; filled on first use.
// ---------------------------------------------------------------------------
static void* g_slotGetState = nullptr;
static void* g_slotSetState = nullptr;
static void* g_slotGetCapabilities = nullptr;
static void* g_slotEnable = nullptr;
static void* g_slotGetBatteryInformation = nullptr;
static void* g_slotGetKeystroke = nullptr;
static void* g_slotGetAudioDeviceIds = nullptr;

// Slots for the ordinal-only exports of the real DLL (100..104, 108, 109).
// They are primed eagerly by the worker thread, but the thunk below tolerates
// an unfilled slot (never jumps to null) so a very early call cannot crash.
extern "C" void* g_slotO100 = nullptr;
extern "C" void* g_slotO101 = nullptr;
extern "C" void* g_slotO102 = nullptr;
extern "C" void* g_slotO103 = nullptr;
extern "C" void* g_slotO104 = nullptr;
extern "C" void* g_slotO108 = nullptr;
extern "C" void* g_slotO109 = nullptr;

// ---------------------------------------------------------------------------
// Ordinal-only forwarder thunk.
//
// `jmp [rip+slot]` would clobber nothing, but a null slot would crash, so the
// thunk tests the slot first. rax is caller-saved and never an argument
// register on Win64, therefore the argument registers (rcx/rdx/r8/r9, xmm0-3)
// and the shadow space reach the real function untouched. If the real export
// cannot be resolved we return ERROR_DEVICE_NOT_CONNECTED instead of exploding.
// ---------------------------------------------------------------------------
#define ACZ_XINPUT_ORDINAL_THUNK(fnname, slot)                                       \
    extern "C" __attribute__((naked, noinline)) void fnname(void) {                  \
        __asm__ __volatile__(                                                        \
            "mov " #slot "(%rip), %rax\n\t"                                          \
            "test %rax, %rax\n\t"                                                    \
            "je 1f\n\t"                                                              \
            "jmp *%rax\n\t"                                                          \
            "1:\n\t"                                                                 \
            "mov $1167, %eax\n\t"                                                    \
            "ret\n\t");                                                              \
    }

ACZ_XINPUT_ORDINAL_THUNK(Fwd_100, g_slotO100)
ACZ_XINPUT_ORDINAL_THUNK(Fwd_101, g_slotO101)
ACZ_XINPUT_ORDINAL_THUNK(Fwd_102, g_slotO102)
ACZ_XINPUT_ORDINAL_THUNK(Fwd_103, g_slotO103)
ACZ_XINPUT_ORDINAL_THUNK(Fwd_104, g_slotO104)
ACZ_XINPUT_ORDINAL_THUNK(Fwd_108, g_slotO108)
ACZ_XINPUT_ORDINAL_THUNK(Fwd_109, g_slotO109)

// ---------------------------------------------------------------------------
// Named exports: plain forwarders. Struct pointers are `void*` on purpose --
// we must never interpret game data, only pass it through unchanged.
// ---------------------------------------------------------------------------
extern "C" DWORD WINAPI XInputGetState(DWORD dwUserIndex, void* pState) {
    typedef DWORD(WINAPI * Fn)(DWORD, void*);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotGetState, 2, "XInputGetState"));
    if (fn == nullptr) return ERROR_DEVICE_NOT_CONNECTED;
    return fn(dwUserIndex, pState);
}

extern "C" DWORD WINAPI XInputSetState(DWORD dwUserIndex, void* pVibration) {
    typedef DWORD(WINAPI * Fn)(DWORD, void*);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotSetState, 3, "XInputSetState"));
    if (fn == nullptr) return ERROR_DEVICE_NOT_CONNECTED;
    return fn(dwUserIndex, pVibration);
}

extern "C" DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                                              void* pCapabilities) {
    typedef DWORD(WINAPI * Fn)(DWORD, DWORD, void*);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotGetCapabilities, 4, "XInputGetCapabilities"));
    if (fn == nullptr) return ERROR_DEVICE_NOT_CONNECTED;
    return fn(dwUserIndex, dwFlags, pCapabilities);
}

extern "C" void WINAPI XInputEnable(BOOL enable) {
    typedef void(WINAPI * Fn)(BOOL);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotEnable, 5, "XInputEnable"));
    if (fn != nullptr) fn(enable);
}

extern "C" DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
                                                    void* pBatteryInformation) {
    typedef DWORD(WINAPI * Fn)(DWORD, BYTE, void*);
    Fn fn = reinterpret_cast<Fn>(aczproxy::ExportCached(
        &g_slotGetBatteryInformation, 7, "XInputGetBatteryInformation"));
    if (fn == nullptr) return ERROR_DEVICE_NOT_CONNECTED;
    return fn(dwUserIndex, devType, pBatteryInformation);
}

extern "C" DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
                                           void* pKeystroke) {
    typedef DWORD(WINAPI * Fn)(DWORD, DWORD, void*);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotGetKeystroke, 8, "XInputGetKeystroke"));
    if (fn == nullptr) return ERROR_DEVICE_NOT_CONNECTED;
    return fn(dwUserIndex, dwReserved, pKeystroke);
}

extern "C" DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId,
                                                UINT* pRenderCount, LPWSTR pCaptureDeviceId,
                                                UINT* pCaptureCount) {
    typedef DWORD(WINAPI * Fn)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
    Fn fn = reinterpret_cast<Fn>(
        aczproxy::ExportCached(&g_slotGetAudioDeviceIds, 10, "XInputGetAudioDeviceIds"));
    if (fn == nullptr) return ERROR_DEVICE_NOT_CONNECTED;
    return fn(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId, pCaptureCount);
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

// Fill the ordinal-only slots. Called from the worker thread only.
static void PrimeOrdinalSlots() {
    struct Item {
        WORD ordinal;
        void* volatile* slot;
    };
    const Item items[] = {
        {100, &g_slotO100}, {101, &g_slotO101}, {102, &g_slotO102}, {103, &g_slotO103},
        {104, &g_slotO104}, {108, &g_slotO108}, {109, &g_slotO109},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        void* p = aczproxy::Export(items[i].ordinal, nullptr);
        if (p != nullptr) InterlockedExchangePointer(items[i].slot, p);
    }
}

static DWORD WINAPI LoaderThread(LPVOID) {
    aczproxy::RunWorker(L"ACZPropNav.dll");
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return aczproxy::HandleDllMain(hinstDLL, fdwReason, lpvReserved, &LoaderThread,
                                   L"XInput1_4.dll", &PrimeOrdinalSlots);
}
