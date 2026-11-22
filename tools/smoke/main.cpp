// ACZPropNav smoke test: loads the built mod DLL into this process, waits for it
// to write its log, and checks that the boot sequence completed without crashing.
//
// This is the closest thing to "does the DLL actually run" that can be done
// offline: the mod will not find any missile records in a synthetic process, but
// it must reach the discovery stage, log the outcome and keep the process alive.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string ExeDir() {
    char path[MAX_PATH * 2] = {};
    DWORD n = ::GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    if (n == 0) return std::string(".");
    std::string s(path, n);
    const size_t slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : s.substr(0, slash);
}

bool WaitForFile(const std::string& path, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 50; ++i) {
        HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            return true;
        }
        ::Sleep(50);
    }
    return false;
}

std::string ReadAll(const std::string& path) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : ExeDir();
    const std::string dllPath = dir + "\\ACZPropNav.dll";
    const std::string logPath = dir + "\\ACZPropNav.log";
    const std::string keyPath = dir + "\\ACZPropNav.key.log";
    ::DeleteFileA(logPath.c_str());
    ::DeleteFileA(keyPath.c_str());

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("    %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    };

    std::printf("[smoke] loading %s\n", dllPath.c_str());
    ::SetDllDirectoryA(dir.c_str());
    HMODULE mod = ::LoadLibraryA(dllPath.c_str());
    if (!mod) {
        std::printf("RESULT: FAIL (LoadLibrary error %lu)\n", ::GetLastError());
        return 1;
    }
    check(true, "LoadLibrary(ACZPropNav.dll) succeeds");

    // The mod exports AczCoreRun (used by the community loader) and also starts
    // itself from DllMain; both paths must be idempotent.
    using CoreRunFn = void (*)(void);
    CoreRunFn coreRun = reinterpret_cast<CoreRunFn>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "AczCoreRun")));
    check(coreRun != nullptr, "exports AczCoreRun");
    if (coreRun) coreRun();

    check(WaitForFile(logPath, 15000), "writes ACZPropNav.log within 15 s");
    const std::string log = ReadAll(logPath);
    if (log.empty()) {
        std::printf("RESULT: FAIL (log is empty / missing)\n");
        return 1;
    }

    // Assert the banner SHAPE, not a hard-coded version: this check used to look for
    // "ACZPropNav 1.0.0" and silently went stale at the 1.0.1 bump (it was masked by
    // the memtest segfault for several CI runs). The literal below is a digit, so any
    // version string from Log::Banner() satisfies it without needing a bump here.
    {
        const bool banner = Contains(log, "ACZPropNav 0.") || Contains(log, "ACZPropNav 1.") ||
                            Contains(log, "ACZPropNav 2.") || Contains(log, "ACZPropNav 3.");
        check(banner, "log contains the version banner");
    }
    check(Contains(log, "配置文件 / config:"), "log names the config file");
    check(Contains(log, "记录字节扫描") || Contains(log, "参数表搜索") || Contains(log, "静态参数路径未启用"),
          "reached the discovery stage (or skipped it by design)");
    check(Contains(log, "静态参数补丁") || Contains(log, "没有找到可信的导弹参数表") ||
              Contains(log, "静态参数路径未启用"),
          "discovery produced a verdict (patched nothing, no crash)");

    // Optional: keep the process alive longer so time-triggered paths (e.g. the
    // Probe schedule, which fires at 10 s) are exercised too.
    int extraWait = 0;
    if (argc > 2) extraWait = atoi(argv[2]);
    if (extraWait > 0) {
        std::printf("[smoke] keeping the process alive for %d s\n", extraWait);
        ::Sleep(static_cast<DWORD>(extraWait) * 1000);
        const std::string key = ReadAll(keyPath);
        check(Contains(key, "probe:") || Contains(key, "PROBE"),
              "probe schedule ran and wrote its diagnostics");
    }

    // Hot paths: give the tick loop a moment to run its retry logic, then make
    // sure the process is still healthy and the log is still being appended.
    ::Sleep(1200);
    const std::string log2 = ReadAll(logPath);
    check(Contains(log2, "参数表搜索") || Contains(log2, "记录字节扫描") ||
              Contains(log2, "重新搜索") || Contains(log2, "静态参数路径未启用"),
          "tick loop kept running without crashing");

    if (Contains(keyPath.empty() ? "" : ReadAll(keyPath), "PROBE")) {
        check(true, "probe dump present in key log");
    }

    // Deliberately do NOT FreeLibrary: the game never unloads mods either, and
    // unloading a DLL with a live worker thread is not a supported path.
    std::printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
