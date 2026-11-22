// ACZPropNav - DLL bootstrap.
//
// Loaded by the XINPUT1_4.dll (or VERSION.dll) proxy that sits next to
// ACZero\acz.exe. Everything happens on a private thread so we never block or
// delay the game's own startup, and nothing here can take the game down: the
// patcher only writes to memory it has verified, and every access is SEH guarded.
#include <windows.h>
#include <cstdio>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/patcher.h"
#include "mem/mem.h"

namespace {

constexpr const char* kVersion = "1.0.0";

HANDLE g_thread = nullptr;
volatile LONG g_started = 0;
volatile LONG g_stop = 0;
acz::Patcher* g_patcher = nullptr;

// Host context handed to us by the community mod loader (version.dll) through
// AczAddonInit. Layout proven against more_bullets.dll / maneuvers.dll:
//   +0x08 const wchar_t* game dir (the ACZero folder)
//   +0x10 HMODULE        acz.exe base
//   +0x28 void (*log)(const char* fmt, ...)
// All of it is optional: when we are loaded by our own proxy instead, the ini is
// simply read from the folder the DLL lives in.
struct HostInfo {
    std::wstring gameDir;
    uintptr_t exeBase = 0;
    void (*logFn)(const char*, ...) = nullptr;
};

HostInfo g_host;

void HostLog(const char* text) {
    if (g_host.logFn) g_host.logFn("[ACZPropNav] %s", text);
}

std::wstring ModuleDirectory() {
    HMODULE self = nullptr;
    // 0x4 = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
    // 0x2 = GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFS
    if (!::GetModuleHandleExW(0x00000004u | 0x00000002u,
                              reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self)) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if (n == 0 || n >= std::size(path)) return std::wstring();
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash);
}

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return std::wstring(name);
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += name;
    return out;
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Where is ACZPropNav.ini?
//   1. <game dir>\mods\ACZPropNav.ini   (community loader convention)
//   2. <folder of ACZPropNav.dll>\ACZPropNav.ini  (our own proxy)
//   3. <game dir>\ACZPropNav.ini
std::wstring PickIniPath(const std::wstring& dllDir) {
    if (!g_host.gameDir.empty()) {
        const std::wstring mods = Join(Join(g_host.gameDir, L"mods"), L"ACZPropNav.ini");
        if (FileExists(mods)) return mods;
    }
    const std::wstring besideDll = Join(dllDir, L"ACZPropNav.ini");
    if (FileExists(besideDll)) return besideDll;
    if (!g_host.gameDir.empty()) {
        const std::wstring inGameDir = Join(g_host.gameDir, L"ACZPropNav.ini");
        if (FileExists(inGameDir)) return inGameDir;
    }
    return besideDll;
}

DWORD WINAPI ModThread(LPVOID) {
    // Discovery can scan a lot of memory; never compete with the game's threads.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    const std::wstring dir = ModuleDirectory();
    if (dir.empty()) return 0;

    // Parse the config first so LogLevel can be honoured from the start.
    acz::Config cfg;
    const std::wstring iniPath = PickIniPath(dir);
    cfg.LoadFromFile(iniPath);

    // Config missing/unreadable: keep the player-facing default (1 = quiet) rather than
    // dumping verbose telemetry, since in that case nothing will be patched anyway.
    acz::L().Init(dir, cfg.loaded ? cfg.runtime.logLevel : 1);
    acz::L().Banner(kVersion, "proportional-navigation guidance for ACZ:TBW (Steam)");
    acz::L().Info("安装目录 / install dir: %s", acz::WideToUtf8(dir).c_str());
    acz::L().Info("配置文件 / config: %s", acz::WideToUtf8(iniPath).c_str());

    if (!cfg.loaded) {
        acz::L().Error("找不到或无法读取 ACZPropNav.ini，什么也不做 / config missing or unreadable, "
                       "doing nothing");
        return 0;
    }
    {
        int playerModels = 0, enemyModels = 0;
        for (const auto& m : cfg.models) {
            if (m.player.Any()) ++playerModels;
            if (m.enemy.Any()) ++enemyModels;
        }
        acz::L().Info("INI: [player] %d 条，[enemy] %d 条，[vanilla] %d 条", playerModels,
                      enemyModels, static_cast<int>(cfg.vanilla.size()));
    }
    if (!cfg.warnings.empty()) {
        for (const auto& w : cfg.warnings) acz::L().Warn("INI: %s", w.c_str());
    }
    cfg.Sanitize();
    if (cfg.runtime.dryRun) acz::L().Warn("DryRun=1: 不会写入任何游戏内存 / no memory writes");
    if (cfg.runtime.probe) acz::L().Warn("Probe=1: 会把诊断写入 ACZPropNav.key.log");

    acz::Patcher patcher;
    g_patcher = &patcher;
    if (!patcher.Start(cfg, dir)) {
        acz::L().Error("Patcher 启动失败 / patcher failed to start");
        g_patcher = nullptr;
        return 0;
    }

    // Probe schedule: the probe installs a *single* observation hook on the missile
    // dispatcher (read-only, self-unhooking after 20 updates), so one trigger after
    // the level has loaded is enough. Repeating it is pointless and only adds risk.
    const double probeTimes[] = {15.0};
    int probesDone = 0;
    ULONGLONG startTick = ::GetTickCount64();
    const DWORD period = static_cast<DWORD>(1000 / (cfg.runtime.tickHz > 0 ? cfg.runtime.tickHz : 120));

    while (!g_stop) {
        patcher.Tick();
        if (cfg.runtime.probe && probesDone < static_cast<int>(std::size(probeTimes))) {
            const double elapsed = static_cast<double>(::GetTickCount64() - startTick) / 1000.0;
            if (elapsed >= probeTimes[probesDone]) {
                acz::L().Info("Probe #%d @%.0fs", probesDone + 1, elapsed);
                patcher.Probe();
                ++probesDone;
            }
        }
        ::Sleep(period == 0 ? 8 : period);
    }

    patcher.Shutdown();
    g_patcher = nullptr;
    acz::L().Info("ACZPropNav 已停止 / stopped");
    acz::L().Shutdown();
    return 0;
}

void StartOnce() {
    if (::InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    g_thread = ::CreateThread(nullptr, 0, &ModThread, nullptr, 0, nullptr);
    if (g_thread) ::CloseHandle(g_thread);
}

} // namespace

extern "C" __declspec(dllexport) void AczCoreRun(void) { StartOnce(); }

// Community mod-loader entry point (version.dll preloader -> ACZero\mods\*.dll).
// Proven ABI: int AczAddonInit(void* hostCtx), one RCX argument.
// hostCtx: +0x08 wchar_t* game dir, +0x10 HMODULE acz.exe, +0x28 printf logger.
extern "C" __declspec(dllexport) int AczAddonInit(void* hostCtx) {
    if (hostCtx) {
        const uintptr_t ctx = reinterpret_cast<uintptr_t>(hostCtx);
        wchar_t* dirPtr = nullptr;
        if (acz::mem::IsReadable(ctx + 0x08, sizeof(dirPtr)) &&
            acz::mem::Read<wchar_t*>(ctx + 0x08, dirPtr) && dirPtr &&
            acz::mem::IsReadable(reinterpret_cast<uintptr_t>(dirPtr), sizeof(wchar_t))) {
            // Bounded copy: a mis-reported pointer must not walk off the map.
            wchar_t buf[512] = {};
            for (int i = 0; i < 511; ++i) {
                wchar_t ch = 0;
                if (!acz::mem::Read<wchar_t>(reinterpret_cast<uintptr_t>(dirPtr) + i * sizeof(wchar_t), ch))
                    break;
                if (ch == 0) break;
                buf[i] = ch;
            }
            if (buf[0]) g_host.gameDir = buf;
        }
        uintptr_t base = 0;
        if (acz::mem::Read<uintptr_t>(ctx + 0x10, base)) g_host.exeBase = base;
        void* logPtr = nullptr;
        if (acz::mem::IsReadable(ctx + 0x28, sizeof(logPtr)) &&
            acz::mem::Read<void*>(ctx + 0x28, logPtr) && logPtr) {
            g_host.logFn = reinterpret_cast<void (*)(const char*, ...)>(logPtr);
        }
        char line[512];
        std::snprintf(line, sizeof(line),
                      "addon init: gameDir=%s exeBase=0x%llX hostLog=%s",
                      acz::WideToUtf8(g_host.gameDir).c_str(),
                      static_cast<unsigned long long>(g_host.exeBase),
                      g_host.logFn ? "yes" : "no");
        HostLog(line);
    }
    StartOnce();
    return 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            ::DisableThreadLibraryCalls(module);
            StartOnce();
            break;
        case DLL_PROCESS_DETACH:
            g_stop = 1;
            // Give the worker a bounded chance to unwind before the image goes
            // away (only reachable if someone FreeLibrary's us; the game does not).
            if (g_thread) ::WaitForSingleObject(g_thread, 3000);
            if (g_thread) { ::CloseHandle(g_thread); g_thread = nullptr; }
            break;
        default:
            break;
    }
    return TRUE;
}
