// ACZPropNav - size-capped diagnostic logger (mirrors AC8PropNav's log behaviour:
// main log capped at 500 KiB, key log at 250 KiB with the recent half kept).
#pragma once

#include <string>

namespace acz {

class Log {
public:
    // dir: directory for the log files (usually the game's ACZero folder).
    // Opens <dir>/ACZPropNav.log and <dir>/ACZPropNav.key.log.
    bool Init(const std::wstring& dir, int level);
    void Shutdown();

    bool IsOpen() const;
    int Level() const;

    // Main log:   "[HH:MM:SS] [CORE] 文本"
    void Info(const char* fmt, ...);
    // Verbose main log (only when level >= 2).
    void Debug(const char* fmt, ...);
    // Key log: main log + ACZPropNav.key.log (for calibration hand-off).
    void Key(const char* fmt, ...);
    // Always printed, both logs, prefixed with "!! ".
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);

    // Container-agnostic helpers.
    void InfoStr(const std::string& s);
    void KeyStr(const std::string& s);
    void WarnStr(const std::string& s);
    void ErrorStr(const std::string& s);
    void DebugStr(const std::string& s);

    // Force both files open with a header banner.
    void Banner(const char* version, const char* buildInfo);

    // Push both files to the volume. Diagnostics are written with WriteFile (no CRT
    // buffering), so this is only needed before a long stretch where the process may
    // die (heavy scans) and the log has to be readable from another process/console.
    void Flush();
};

Log& L();

// UTF-8 <-> UTF-16 helpers (no exceptions).
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

} // namespace acz
