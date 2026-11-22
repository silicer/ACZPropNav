// ACZPropNav - size-capped diagnostic logger.
//
// Deliberately built on Win32 file APIs only (no iostream, no CRT locale) so it is
// safe and predictable inside a game process, and works before the CRT is fully up.
#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace acz {

namespace {

constexpr DWORD kMainCap = 500u * 1024u;  // AC8PropNav uses 500 KiB for the main log
// The key log carries the probe dumps.  A Run A probe (docs/ARCHITECTURE.md) writes
// roughly 250 KiB of intentional diagnostics, so the cap is 512 KiB: without the
// headroom the "keep the recent half" rotation would throw away the hook/alloc
// events recorded at the start of the run.
constexpr DWORD kKeyCap = 512u * 1024u;


struct FileLog {
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD size = 0;
    DWORD cap = 0;
    std::wstring path;

    void Close() {
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
        size = 0;
    }

    bool Open(const std::wstring& p, DWORD capBytes, bool truncate) {
        Close();
        path = p;
        cap = capBytes;
        h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (::GetFileSizeEx(h, &sz)) size = static_cast<DWORD>(sz.QuadPart);
        LARGE_INTEGER pos{};
        pos.QuadPart = size;
        ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        return true;
    }

    // Writes raw bytes at the end of the file.
    bool Append(const char* data, size_t len) {
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!::WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr)) return false;
        size += written;
        return true;
    }

    // Keep the most recent `keep` bytes and start over with `header` in front.
    bool RotateKeepTail(DWORD keep, const char* header) {
        if (h == INVALID_HANDLE_VALUE) return false;
        if (size <= cap) return true;
        const DWORD tail = (size > keep) ? keep : size;
        std::string buf(tail, '\0');
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(size) - static_cast<LONGLONG>(tail);
        if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        if (!::ReadFile(h, &buf[0], tail, &got, nullptr)) return false;
        buf.resize(got);
        ::CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
        h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        size = 0;
        if (header) Append(header, std::strlen(header));
        Append(buf.data(), buf.size());
        LARGE_INTEGER endPos{};
        endPos.QuadPart = size;
        ::SetFilePointerEx(h, endPos, nullptr, FILE_BEGIN);
        return true;
    }
};

struct State {
    bool inited = false;
    int level = 2;
    CRITICAL_SECTION cs;
    bool csOk = false;
    FileLog mainLog;
    FileLog keyLog;
    std::wstring dir;
};

State& S() {
    static State s;
    return s;
}

void Timestamp(char* out, size_t n) {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::snprintf(out, n, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
}

void AppendLine(FileLog& f, const std::string& line) {
    f.Append(line.data(), line.size());
    f.Append("\r\n", 2);
}

void RotateIfNeeded(FileLog& f, DWORD keep) {
    if (f.size <= f.cap) return;
    const char* header = nullptr;
    std::string h;
    if (f.cap == kMainCap) {
        h = "[CORE] ---- 日志已超过上限，清空重写 / log exceeded cap, truncated ----\r\n";
    } else {
        h = "[CORE] ---- 日志已超过上限，保留最近一半 / log exceeded cap, kept recent half ----\r\n";
    }
    header = h.c_str();
    f.RotateKeepTail(keep, header);
}

void VWrite(int dest, const char* prefix, const char* fmt, va_list ap) {
    State& st = S();
    if (!st.inited) return;
    if (!st.csOk) return;
    char body[4096];
    va_list cp;
    va_copy(cp, ap);
    int n = std::vsnprintf(body, sizeof(body), fmt, cp);
    va_end(cp);
    if (n < 0) body[0] = '\0';
    if (n >= (int)sizeof(body)) body[sizeof(body) - 1] = '\0';

    char ts[32];
    Timestamp(ts, sizeof(ts));
    std::string line;
    line.reserve(std::strlen(ts) + std::strlen(prefix) + std::strlen(body) + 16);
    line += ts;
    line += prefix;
    line += body;

    ::EnterCriticalSection(&st.cs);
    if (dest & 1) {
        AppendLine(st.mainLog, line);
        RotateIfNeeded(st.mainLog, kMainCap);
    }
    if (dest & 2) {
        AppendLine(st.keyLog, line);
        RotateIfNeeded(st.keyLog, kKeyCap / 2);
    }
    ::LeaveCriticalSection(&st.cs);
}

} // namespace

Log& L() {
    static Log instance;
    return instance;
}

bool Log::Init(const std::wstring& dir, int level) {
    State& st = S();
    if (st.inited) return true;
    if (!st.csOk) {
        ::InitializeCriticalSection(&st.cs);
        st.csOk = true;
    }
    st.level = level;
    st.dir = dir;
    // LogLevel 0 means "write no log files at all" (documented in the INI and README).
    // Init still stores the level first so the rest of the module behaves, but it must
    // not create the files - otherwise "0 = off" leaves two empty files in the game
    // folder, which is exactly the kind of thing that makes users think it is broken.
    if (level < 1) {
        st.inited = false;
        return false;
    }
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
    std::wstring mainPath = base + L"ACZPropNav.log";
    std::wstring keyPath = base + L"ACZPropNav.key.log";

    ::EnterCriticalSection(&st.cs);
    st.mainLog.Open(mainPath, kMainCap, false);
    st.keyLog.Open(keyPath, kKeyCap, false);
    st.inited = (st.mainLog.h != INVALID_HANDLE_VALUE);
    ::LeaveCriticalSection(&st.cs);
    return st.inited;
}

void Log::Shutdown() {
    State& st = S();
    if (!st.csOk) return;
    ::EnterCriticalSection(&st.cs);
    st.mainLog.Close();
    st.keyLog.Close();
    st.inited = false;
    ::LeaveCriticalSection(&st.cs);
}

bool Log::IsOpen() const { return S().inited; }
int Log::Level() const { return S().level; }

void Log::Flush() {
    State& st = S();
    if (!st.csOk) return;
    ::EnterCriticalSection(&st.cs);
    if (st.mainLog.h != INVALID_HANDLE_VALUE) ::FlushFileBuffers(st.mainLog.h);
    if (st.keyLog.h != INVALID_HANDLE_VALUE) ::FlushFileBuffers(st.keyLog.h);
    ::LeaveCriticalSection(&st.cs);
}

void Log::Banner(const char* version, const char* buildInfo) {
    Info("============================================================");
    Info("ACZPropNav %s", version ? version : "");
    Info("%s", buildInfo ? buildInfo : "");
    Info("target: ACE COMBAT ZERO: THE BELKAN WAR (Steam) / ACZero\\acz.exe");
    Info("============================================================");
}

void Log::Info(const char* fmt, ...) {
    // LogLevel = 0 means "no log files at all" (documented in the INI). Previously
    // Info() had NO level check at all, so LogLevel was a no-op for everything except
    // the rarely used Debug() - a documented switch that silently did nothing.
    if (S().level < 1) return;
    va_list ap;
    va_start(ap, fmt);
    VWrite(1, "[CORE] ", fmt, ap);
    va_end(ap);
}

void Log::Debug(const char* fmt, ...) {
    if (S().level < 2) return;
    va_list ap;
    va_start(ap, fmt);
    VWrite(1, "[CORE] ", fmt, ap);
    va_end(ap);
}

void Log::Key(const char* fmt, ...) {
    // Key() is the verbose channel: it writes BOTH logs, so it is gated on level >= 2.
    // IMPORTANT: a handful of startup/one-shot lines use Key() and are genuinely useful
    // at level 1 too - those were converted to Info() at their call sites, not here.
    if (S().level < 2) return;
    va_list ap;
    va_start(ap, fmt);
    VWrite(3, "[CORE] ", fmt, ap);
    va_end(ap);
}

void Log::Warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VWrite(3, "[CORE] !! ", fmt, ap);
    va_end(ap);
}

void Log::Error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VWrite(3, "[CORE] !!! ", fmt, ap);
    va_end(ap);
}

void Log::InfoStr(const std::string& s) { Info("%s", s.c_str()); }
void Log::KeyStr(const std::string& s) { Key("%s", s.c_str()); }
void Log::WarnStr(const std::string& s) { Warn("%s", s.c_str()); }
void Log::ErrorStr(const std::string& s) { Error("%s", s.c_str()); }
void Log::DebugStr(const std::string& s) { Debug("%s", s.c_str()); }

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

} // namespace acz
