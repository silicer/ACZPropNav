// ACZPropNav - SEH-safe process memory primitives, region enumeration, scanners.
//
// Everything here is designed to be safe inside a live, Steam-DRM-wrapped game:
//  - every raw dereference goes through a fault guard (see "fault guard" below),
//  - scans only touch committed, readable memory that VirtualQueryEx reports,
//  - writes re-protect the page, write, restore, flush icache when exec,
//  - writes are alignment/size checked and hard-capped.
//
// ---------------------------------------------------------------------------
// Why there is no `__try/__except` in this file
// ---------------------------------------------------------------------------
// The repository toolchain is `x86_64-w64-mingw32-g++` which on this machine is
// clang 23 targeting x86_64-w64-windows-gnu.  That target *parses*
// `__try`/`__except` when -fms-extensions is given, but it does NOT emit a
// Microsoft exception handler for it: the generated module contains no
// `__C_specific_handler` reference, no .xdata funclet and no `.seh_handler`
// directive, and an actual access violation is not caught (verified with a
// runtime probe; the crash propagates as an unhandled AV).  Silently dropping a
// safety construct is the worst possible outcome, so this file uses the
// sanctioned fallback instead:
//
//   one process-wide AddVectoredExceptionHandler() + a thread-local
//   setjmp/longjmp frame around each copy.
//
// The frame is only "armed" while we are inside our own memcpy, on our own
// thread, so faults in game code (or in other threads) are always passed
// through with EXCEPTION_CONTINUE_SEARCH: the mod never swallows a game crash.
// The handler is installed lazily on first use and never removed (process
// lifetime).  If the handler cannot be installed (exotic environment) every
// primitive falls back to ReadProcessMemory/WriteProcessMemory, which are
// kernel-mediated and cannot crash the caller either.
//
// Tests: tools/memtest/main.cpp exercises null/invalid/guard-page/boundary
// faults through these primitives, plus a 200-fault stress loop.
#include "mem/mem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>

// MinGW headers call this GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT; some SDK
// revisions / docs use ..._UNCHANGED_REFS.  Define it defensively.
#ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#endif

// ---------------------------------------------------------------------------
// noinline helper: keep the setjmp frame in its own function so the compiler
// cannot fold the guarded copy into a caller that longjmps across objects.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define ACZ_NOINLINE __declspec(noinline)
#else
#define ACZ_NOINLINE __attribute__((noinline))
#endif

namespace acz::mem {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Hard cap for a single ReadBytes/WriteBytes request.  Anything bigger is a
// caller bug (or an attempt to copy a whole region) and is rejected.
constexpr size_t kMaxTransfer = 64u * 1024u;

// Bulk-copy chunk used by the scanners.  256 KiB keeps the scratch buffer in
// cache while letting each region be traversed with one VirtualQuery-validated
// copy per chunk instead of one guarded call per byte.
constexpr size_t kScanChunk = 256u * 1024u;

// KUSER_SHARED_DATA / PAGE_GUARD violations arrive as this code (ntstatus.h is
// not included on purpose: MinGW's windows.h does not pull it in cleanly).
constexpr DWORD kGuardPageViolation = 0x80000001u;

// ---------------------------------------------------------------------------
// Process / module state
// ---------------------------------------------------------------------------

void* g_hProcess = nullptr;   // target process handle (current process by default)
bool g_isSelf = true;         // true when g_hProcess == GetCurrentProcess()

bool g_mainInit = false;
ModuleInfo g_main;

char g_moduleAnchor = 0;      // address inside our own module (fallback lookup)

void EnsureProcess() {
    if (!g_hProcess) {
        g_hProcess = GetCurrentProcess();
        g_isSelf = true;
    }
}

void ResetModuleCache() {
    g_mainInit = false;
    g_main = ModuleInfo{};
}

// ---------------------------------------------------------------------------
// Fault guard
// ---------------------------------------------------------------------------
//
// A frame holds the page-rounded address window of the copies that are allowed
// to fault for it.  Rounding to pages (instead of the exact byte range) is
// required for correctness: the CRT's memcpy is free to move data in machine
// words inside the pages it copies from, and a page-aligned window covers those
// accesses.  Anything outside the window (different thread, game code, a bug in
// our own buffer handling) is passed through untouched, so the mod can never
// swallow a crash that belongs to the game.

struct GuardFrame {
    jmp_buf jb;
    GuardFrame* prev;
    uintptr_t lo;   // page-aligned start of the only addresses this frame may recover
    uintptr_t hi;   // page-aligned end (exclusive)
};

thread_local GuardFrame* t_frame = nullptr;
volatile LONG g_guardState = 0;  // 0 = uninitialised, 1 = installing, 2 = ready, 3 = unavailable

uintptr_t PageSize() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 0x1000u;
}

// Page window covering every page touched by [addr, addr+n).  n must be > 0.
void ArmWindow(GuardFrame& f, uintptr_t addr, size_t n) {
    const uintptr_t ps = PageSize();
    f.lo = addr & ~(ps - 1);
    f.hi = ((addr + (n - 1)) & ~(ps - 1)) + ps;   // last touched page, inclusive
}

extern "C" LONG CALLBACK AczGuardHandler(EXCEPTION_POINTERS* ep) {
    GuardFrame* frame = t_frame;
    // No active guard on this thread -> not our fault: let the game handle it.
    if (!frame) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != kGuardPageViolation) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The fault address is parameter 1 of both AV and in-page error records.
    const uintptr_t faultAddr = (ep->ExceptionRecord->NumberParameters >= 2)
                                    ? static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1])
                                    : 0;
    if (faultAddr < frame->lo || faultAddr >= frame->hi) {
        // A fault outside the copy we armed: not ours, never absorb it.
        return EXCEPTION_CONTINUE_SEARCH;
    }
    longjmp(frame->jb, 1);  // never returns
    return EXCEPTION_CONTINUE_SEARCH;  // unreachable; keeps the compiler happy
}

bool EnsureGuard() {
    const LONG state = g_guardState;
    if (state == 2) return true;
    if (state == 3) return false;
    if (InterlockedCompareExchange(&g_guardState, 1, 0) == 0) {
        PVOID h = AddVectoredExceptionHandler(1 /* first handler */, &AczGuardHandler);
        InterlockedExchange(&g_guardState, h ? 2 : 3);
    } else {
        // Another thread is installing; give it a moment (this is a one-shot path).
        for (int i = 0; i < 200 && g_guardState == 1; ++i) Sleep(0);
    }
    return g_guardState == 2;
}

// Copy `n` bytes from a target address we do not trust into `dst`.  Only the
// memcpy is inside the guard: nothing else can be masked by the handler.
ACZ_NOINLINE bool GuardedRead(void* dst, uintptr_t src, size_t n) {
    GuardFrame frame;
    frame.prev = t_frame;
    ArmWindow(frame, src, n);
    if (setjmp(frame.jb) != 0) {   // faulted: frame is still ours, unwind it
        t_frame = frame.prev;
        return false;
    }
    t_frame = &frame;
    std::memcpy(dst, reinterpret_cast<const void*>(src), n);
    t_frame = frame.prev;
    return true;
}

// Copy `n` bytes from `src` into a target address we do not trust.
ACZ_NOINLINE bool GuardedWrite(uintptr_t dst, const void* src, size_t n) {
    GuardFrame frame;
    frame.prev = t_frame;
    ArmWindow(frame, dst, n);
    if (setjmp(frame.jb) != 0) {
        t_frame = frame.prev;
        return false;
    }
    t_frame = &frame;
    std::memcpy(reinterpret_cast<void*>(dst), src, n);
    t_frame = frame.prev;
    return true;
}

// Kernel-mediated fallbacks: ReadProcessMemory/WriteProcessMemory validate the
// range in the kernel and return FALSE instead of raising in our thread.
bool CopyFromTarget(void* dst, uintptr_t src, size_t n) {
    EnsureProcess();
    if (g_isSelf && EnsureGuard()) return GuardedRead(dst, src, n);
    SIZE_T got = 0;
    const BOOL ok = ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(src), dst, n, &got);
    return ok != FALSE && got == n;
}

bool CopyToTarget(uintptr_t dst, const void* src, size_t n) {
    EnsureProcess();
    if (g_isSelf && EnsureGuard()) return GuardedWrite(dst, src, n);
    SIZE_T put = 0;
    const BOOL ok = WriteProcessMemory(g_hProcess, reinterpret_cast<LPVOID>(dst), src, n, &put);
    return ok != FALSE && put == n;
}

// ---------------------------------------------------------------------------
// Protection classification
// ---------------------------------------------------------------------------

bool ProtectReadable(DWORD p) {
    switch (p & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool ProtectWritable(DWORD p) {
    switch (p & 0xFFu) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool ProtectExecutable(DWORD p) {
    switch (p & 0xFFu) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// PAGE_GUARD / PAGE_NOACCESS regions are reported as non-readable: touching a
// guard page raises STATUS_GUARD_PAGE_VIOLATION and consumes the guard, which
// is not something a scanner should ever do to a live process.
bool IsRegionReadable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    return ProtectReadable(mbi.Protect);
}

Region MakeRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    Region r;
    r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    r.size = static_cast<size_t>(mbi.RegionSize);
    r.protect = static_cast<uint32_t>(mbi.Protect);
    r.state = static_cast<uint32_t>(mbi.State);
    r.type = static_cast<uint32_t>(mbi.Type);
    const bool guard = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
    r.readable = !guard && ProtectReadable(mbi.Protect);
    r.writable = !guard && ProtectWritable(mbi.Protect);
    r.executable = !guard && ProtectExecutable(mbi.Protect);
    r.image = (mbi.Type == MEM_IMAGE);
    return r;
}

// ---------------------------------------------------------------------------
// Module enumeration helpers
// ---------------------------------------------------------------------------

bool EnumModuleHandles(std::vector<HMODULE>& out) {
    EnsureProcess();
    out.clear();
    HMODULE buf[1024];
    DWORD needed = 0;
    BOOL ok = EnumProcessModulesEx(g_hProcess, buf, static_cast<DWORD>(sizeof(buf)), &needed,
                                   LIST_MODULES_ALL);
    if (!ok) {
        needed = 0;
        ok = EnumProcessModules(g_hProcess, buf, static_cast<DWORD>(sizeof(buf)), &needed);
    }
    if (!ok) return false;
    size_t count = static_cast<size_t>(needed) / sizeof(HMODULE);
    if (count > (sizeof(buf) / sizeof(buf[0]))) count = sizeof(buf) / sizeof(buf[0]);
    out.assign(buf, buf + count);
    return !out.empty();
}

void FillModuleInfo(HMODULE h, ModuleInfo& out) {
    out = ModuleInfo{};
    if (!h) return;
    MODULEINFO mi{};
    if (GetModuleInformation(g_hProcess, h, &mi, sizeof(mi))) {
        out.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        out.size = static_cast<size_t>(mi.SizeOfImage);
    }
    wchar_t path[MAX_PATH * 2] = {};
    if (GetModuleFileNameExW(g_hProcess, h, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])))) {
        out.path.assign(path);
    }
}

// ---------------------------------------------------------------------------
// x86-64 RIP-relative instruction matcher
// ---------------------------------------------------------------------------

bool IsLegacyPrefix(uint8_t b) {
    switch (b) {
        case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3:
        case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65:
            return true;
        default:
            return false;
    }
}

bool IsRexPrefix(uint8_t b) { return (b & 0xF0u) == 0x40u; }

// One-byte opcodes that take a ModRM byte and NO immediate.  Only these can be
// decoded safely without a full length decoder: `48 8B 05`, `48 8D 0D`, `4C 8B 35`,
// `48 89 05`, `48 3B 05`, ...  (0x8D = lea, 0x8B = mov r,r/m, ...)
bool IsRipOpcode(uint8_t op) {
    switch (op) {
        case 0x01: case 0x03: case 0x09: case 0x0B: case 0x11: case 0x13:
        case 0x19: case 0x1B: case 0x21: case 0x23: case 0x29: case 0x2B:
        case 0x31: case 0x33: case 0x39: case 0x3B: case 0x63: case 0x85:
        case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
        case 0x8F: case 0xFF:
            return true;
        default:
            return false;
    }
}

// SSE/SSE2 opcodes after 0x0F that take a ModRM and no immediate.
bool IsSseOpcode(uint8_t op) {
    switch (op) {
        case 0x10: case 0x11: case 0x12: case 0x13:  // movss/movsd/movlps/...
        case 0x28: case 0x29:                        // movaps/movapd
        case 0x2E: case 0x2F:                        // ucomiss/comiss
        case 0x51: case 0x54: case 0x55: case 0x56: case 0x57:  // sqrt/and/or/xor
        case 0x58: case 0x59: case 0x5A: case 0x5C: case 0x5D: case 0x5E: case 0x5F:  // arith
        case 0x6E: case 0x6F: case 0x7E: case 0x7F:  // movd/movq/movdqa
        case 0xD6:
            return true;
        default:
            return false;
    }
}

// If `p` (avail bytes available) starts an instruction with a RIP-relative
// disp32 memory operand, fill `dispOff` (offset of the disp32 inside the
// instruction) and `insnLen`; otherwise return false.
bool DecodeRipMem(const uint8_t* p, size_t avail, size_t& dispOff, size_t& insnLen) {
    dispOff = 0;
    insnLen = 0;
    size_t i = 0;
    // Skip legacy prefixes (limit the scan to keep this cheap on garbage data).
    for (int guard = 0; i < avail && i < 6 && IsLegacyPrefix(p[i]); ++guard, ++i) {
        if (guard > 5) return false;
    }
    if (i < avail && IsRexPrefix(p[i])) ++i;
    if (i >= avail) return false;

    if (p[i] == 0x0F) {
        if (i + 3 > avail) return false;
        if (!IsSseOpcode(p[i + 1])) return false;
        const uint8_t modrm = p[i + 2];
        if ((modrm & 0xC7u) != 0x05u) return false;   // mod=00, rm=101 -> RIP+disp32
        dispOff = i + 3;
        insnLen = dispOff + 4;
        return avail >= insnLen;
    }

    if (!IsRipOpcode(p[i])) return false;
    if (i + 2 > avail) return false;
    const uint8_t modrm = p[i + 1];
    if ((modrm & 0xC7u) != 0x05u) return false;
    dispOff = i + 2;
    insnLen = dispOff + 4;
    return avail >= insnLen;
}

// ---------------------------------------------------------------------------
// Chunked traversal helper
// ---------------------------------------------------------------------------

// Thread-local scratch so repeated scans do not churn the heap.
std::vector<uint8_t>& ScratchBuffer() {
    static thread_local std::vector<uint8_t> buf;
    return buf;
}

// Copies `r` into a scratch buffer in overlapping chunks and invokes
// fn(chunkBytes, chunkLen, isLast).  The chunk start is always 4-byte aligned
// (regions are page aligned and `step` is a multiple of 4 for the float scanner
// because its overlap is), which the float scanner relies on.
template <typename Fn>
void ForEachChunk(const Region& r, size_t overlap, Fn&& fn) {
    if (r.size == 0) return;
    size_t step = (kScanChunk > overlap + 4) ? (kScanChunk - overlap) : (overlap + 4);
    std::vector<uint8_t>& buf = ScratchBuffer();
    buf.resize(kScanChunk);
    size_t off = 0;
    while (off < r.size) {
        const size_t chunkLen = (r.size - off < kScanChunk) ? (r.size - off) : kScanChunk;
        const bool last = (off + chunkLen >= r.size);
        if (!CopyFromTarget(buf.data(), r.base + off, chunkLen)) {
            // Region turned unreadable / partially unmapped: skip this chunk.
            if (last) return;
            off += chunkLen;
            off = (off + 3u) & ~static_cast<size_t>(3u);  // keep chunk starts aligned
            continue;
        }
        fn(buf.data(), chunkLen, last);
        if (last) return;
        off += (step < chunkLen) ? step : chunkLen;
    }
}

}  // namespace

// ===========================================================================
// Public API
// ===========================================================================

bool SetProcess(void* hProcess) {
    if (!hProcess) return false;
    g_hProcess = hProcess;
    g_isSelf = (hProcess == GetCurrentProcess());
    ResetModuleCache();
    return true;
}

void* Process() {
    EnsureProcess();
    return g_hProcess;
}

bool MainModule(ModuleInfo& out) {
    EnsureProcess();
    if (!g_mainInit) {
        g_mainInit = true;
        g_main = ModuleInfo{};

        std::vector<HMODULE> mods;
        if (EnumModuleHandles(mods) && !mods.empty()) {
            // EnumProcessModulesEx documents the main (exe) module first.
            FillModuleInfo(mods[0], g_main);
        }
        if (g_main.base == 0) {
            // Fallback for exotic cases: resolves our own module (for the DLL this
            // is ACZPropNav.dll, i.e. better than nothing for logging purposes).
            HMODULE h = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&g_moduleAnchor), &h) &&
                h) {
                FillModuleInfo(h, g_main);
            }
        }
    }
    out = g_main;
    return g_main.base != 0;
}

void EnumRegions(std::vector<Region>& out, size_t minSize) {
    out.clear();
    EnsureProcess();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t pageSize = si.dwPageSize ? si.dwPageSize : 0x1000u;
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uint64_t guard = 0;
    while (addr < maxAddr) {
        if (++guard > 2000000ull) break;  // paranoia against a pathological VAD
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(g_hProcess, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
            addr += pageSize;
            continue;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t rsize = static_cast<size_t>(mbi.RegionSize);
        if (rsize == 0) break;
        const uintptr_t next = base + rsize;
        if (mbi.State == MEM_COMMIT && rsize >= minSize) {
            out.push_back(MakeRegion(mbi));
        }
        if (next <= addr) break;  // no forward progress
        addr = next;
    }
}

std::vector<Region> WritableRegions(size_t minSize) {
    std::vector<Region> all, out;
    EnumRegions(all, minSize);
    out.reserve(all.size());
    for (const Region& r : all) {
        if (r.readable && r.writable) out.push_back(r);
    }
    return out;
}

std::vector<Region> ExecutableRegions(size_t minSize) {
    std::vector<Region> all, out;
    EnumRegions(all, minSize);
    out.reserve(all.size());
    for (const Region& r : all) {
        if (r.readable && r.executable) out.push_back(r);
    }
    return out;
}

// ---- SEH-safe primitive access -------------------------------------------

bool IsReadable(uintptr_t addr, size_t size) {
    if (!addr || !size) return false;
    EnsureProcess();
    const uintptr_t end = addr + size;
    if (end < addr) return false;  // overflow
    uintptr_t cur = addr;
    uint64_t guard = 0;
    while (cur < end) {
        if (++guard > 100000ull) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(g_hProcess, reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (!IsRegionReadable(mbi)) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = base + static_cast<size_t>(mbi.RegionSize);
        if (next <= cur) return false;
        cur = next;
    }
    return true;
}

bool ReadBytes(uintptr_t addr, void* dst, size_t size) {
    if (!addr || !dst || !size) return false;
    if (size > kMaxTransfer) return false;
    return CopyFromTarget(dst, addr, size);
}

bool WriteBytes(uintptr_t addr, const void* src, size_t size) {
    if (!addr || !src || !size) return false;
    if (size > kMaxTransfer) return false;
    if ((addr & 0x3u) != 0) return false;  // only float/pointer-aligned writes are allowed
    EnsureProcess();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t pageSize = si.dwPageSize ? si.dwPageSize : 0x1000u;
    const uintptr_t end = addr + size;
    if (end < addr) return false;

    // Walk the write page by page. VirtualProtectEx reports ONE protection value for
    // a multi-page range (Windows documents the first page's), so raising a range to
    // RWX and restoring that single value would leave the trailing page(s) at the
    // FIRST page's protection. A 12-byte WriteVec3 straddling a 4 KiB boundary could
    // therefore leave the second page PAGE_READONLY (or PAGE_EXECUTE_READ) instead of
    // its original PAGE_READWRITE, and the game's own next write there would fault.
    // Save each page's own protection, do the copy once, then restore each page.
    constexpr int kMaxPages = 8;
    DWORD saved[kMaxPages] = {};
    int pages = 0;
    bool wasExec = false;
    bool unlocked = true;
    const uintptr_t firstPage = addr & ~(pageSize - 1);
    const uintptr_t lastPage = (end - 1) & ~(pageSize - 1);
    for (uintptr_t page = firstPage; page <= lastPage; page += pageSize) {
        if (pages >= kMaxPages) {  // cannot happen for kMaxTransfer, but never overrun
            unlocked = false;
            break;
        }
        DWORD old = 0;
        if (VirtualProtectEx(g_hProcess, reinterpret_cast<LPVOID>(page),
                             static_cast<SIZE_T>(pageSize), PAGE_EXECUTE_READWRITE, &old) == 0) {
            unlocked = false;
            break;
        }
        saved[pages++] = old;
        if ((old & 0xF0u) != 0) wasExec = true;
    }

    bool ok = false;
    if (unlocked) {
        ok = CopyToTarget(addr, src, size);
        if (ok && wasExec) {
            // We patched code (or a page mapped as code): drop stale icache lines.
            FlushInstructionCache(g_hProcess, reinterpret_cast<LPCVOID>(addr), size);
        }
    }

    // Restore every page we unlocked, whether or not the copy succeeded.
    for (int i = 0; i < pages; ++i) {
        const uintptr_t page = firstPage + static_cast<uintptr_t>(i) * pageSize;
        DWORD ignored = 0;
        VirtualProtectEx(g_hProcess, reinterpret_cast<LPVOID>(page),
                         static_cast<SIZE_T>(pageSize), saved[i], &ignored);
    }
    return ok;
}

// NOTE: these two are page-value-lossy for ranges spanning more than one page:
// VirtualProtectEx returns a single protection value (the first page's) for a
// multi-page range, so RestoreProtect cannot put the trailing pages back correctly.
// WriteBytes therefore does NOT use them - it saves/restores per page itself.
// They are kept for single-page callers only (and to preserve the public API).
bool MakeWritable(uintptr_t addr, size_t size, uint32_t& oldProtect) {
    if (!addr || !size) return false;
    EnsureProcess();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t pageSize = si.dwPageSize ? si.dwPageSize : 0x1000u;
    const uintptr_t end = addr + size;
    if (end < addr) return false;
    const uintptr_t start = addr & ~(pageSize - 1);
    const uintptr_t stop = (end + pageSize - 1) & ~(pageSize - 1);
    if (stop <= start) return false;
    DWORD old = 0;
    if (!VirtualProtectEx(g_hProcess, reinterpret_cast<LPVOID>(start),
                          static_cast<SIZE_T>(stop - start), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    oldProtect = old;
    return true;
}

bool RestoreProtect(uintptr_t addr, size_t size, uint32_t oldProtect) {
    if (!addr || !size) return false;
    EnsureProcess();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t pageSize = si.dwPageSize ? si.dwPageSize : 0x1000u;
    const uintptr_t end = addr + size;
    if (end < addr) return false;
    const uintptr_t start = addr & ~(pageSize - 1);
    const uintptr_t stop = (end + pageSize - 1) & ~(pageSize - 1);
    if (stop <= start) return false;
    DWORD old = 0;
    return VirtualProtectEx(g_hProcess, reinterpret_cast<LPVOID>(start),
                            static_cast<SIZE_T>(stop - start), oldProtect, &old) != FALSE;
}

// ---- scanning -------------------------------------------------------------

void ScanBytes(const Region& r, const uint8_t* pattern, const char* mask, size_t len,
               std::vector<uintptr_t>& hits, size_t maxHits) {
    if (!pattern || !mask || len == 0 || r.size < len || !r.readable) return;
    // Overlap of len-1 so a match crossing a chunk boundary is still found.
    const size_t overlap = len - 1;
    ForEachChunk(r, overlap, [&](const uint8_t* buf, size_t chunkLen, bool last) {
        if (hits.size() >= maxHits) return;
        if (chunkLen < len) return;
        size_t scanCount = chunkLen - len + 1;
        size_t limit = scanCount;
        if (!last) {
            const size_t step = kScanChunk - overlap;
            if (step < scanCount) limit = step;
        }
        for (size_t i = 0; i < limit; ++i) {
            if (hits.size() >= maxHits) return;
            const uint8_t* p = buf + i;
            // Fast first-byte rejection, then the full mask compare.
            if (mask[0] == 'x' && p[0] != pattern[0]) continue;
            bool ok = true;
            for (size_t k = 1; k < len; ++k) {
                if (mask[k] == 'x' && p[k] != pattern[k]) {
                    ok = false;
                    break;
                }
            }
            if (ok) hits.push_back(r.base + i);
        }
    });
}

void ScanAll(bool writableOnly, bool executableOnly, const uint8_t* pattern, const char* mask,
             size_t len, std::vector<uintptr_t>& hits, size_t maxHitsPerRange) {
    std::vector<Region> regions;
    EnumRegions(regions);
    for (const Region& r : regions) {
        if (!r.readable) continue;
        if (writableOnly && !r.writable) continue;
        if (executableOnly && !r.executable) continue;
        const size_t startHits = hits.size();
        ScanBytes(r, pattern, mask, len, hits, startHits + maxHitsPerRange);
    }
}

void FindFloatRuns(const Region& r, const float* values, const float* tols, int count,
                   std::vector<uintptr_t>& hits, size_t maxHits) {
    if (!values || !tols || count <= 0 || !r.readable) return;
    const size_t runBytes = static_cast<size_t>(count) * sizeof(float);
    if (r.size < runBytes) return;
    // Starts are 4-byte aligned; overlap keeps runs crossing a chunk boundary.
    const size_t overlap = runBytes - sizeof(float);
    ForEachChunk(r, overlap, [&](const uint8_t* buf, size_t chunkLen, bool last) {
        if (hits.size() >= maxHits) return;
        if (chunkLen < runBytes) return;
        const size_t scanCount = chunkLen - runBytes + 1;  // inclusive of the last start
        size_t limit = scanCount;
        if (!last) {
            const size_t step = kScanChunk - overlap;
            if (step < scanCount) limit = step;
        }
        // Only 4-aligned starts can be the first element of a float array.
        for (size_t i = 0; i < limit; i += sizeof(float)) {
            if (hits.size() >= maxHits) return;
            const float* f = reinterpret_cast<const float*>(buf + i);
            // Cheap first-element test; also rejects NaN (both comparisons false).
            const float d0 = f[0] - values[0];
            if (!(d0 <= tols[0] && d0 >= -tols[0])) continue;
            bool ok = true;
            for (int k = 1; k < count; ++k) {
                const float dk = f[k] - values[k];
                if (!(dk <= tols[k] && dk >= -tols[k])) {
                    ok = false;
                    break;
                }
            }
            if (ok) hits.push_back(r.base + i);
        }
    });
}

void CollectRipRelativeRefs(const Region& code, uintptr_t tableLo, uintptr_t tableHi,
                            std::vector<uintptr_t>& refs, size_t maxHits) {
    if (!code.readable || !code.executable || tableHi <= tableLo) return;
    constexpr size_t kMaxInsn = 16;
    const size_t overlap = kMaxInsn - 1;
    ForEachChunk(code, overlap, [&](const uint8_t* buf, size_t chunkLen, bool last) {
        if (refs.size() >= maxHits) return;
        size_t limit = chunkLen;
        if (!last) {
            const size_t step = kScanChunk - overlap;
            if (step < limit) limit = step;
        }
        for (size_t i = 0; i < limit; ++i) {
            if (refs.size() >= maxHits) return;
            const size_t avail = chunkLen - i;
            size_t dispOff = 0, insnLen = 0;
            if (!DecodeRipMem(buf + i, avail, dispOff, insnLen)) continue;
            int32_t disp = 0;
            std::memcpy(&disp, buf + i + dispOff, sizeof(disp));
            const uintptr_t insnAddr = code.base + i;
            const int64_t t = static_cast<int64_t>(insnAddr) + static_cast<int64_t>(insnLen) +
                              static_cast<int64_t>(disp);
            if (t <= 0) continue;
            const uintptr_t target = static_cast<uintptr_t>(t);
            if (target >= tableLo && target < tableHi) {
                // NOTE: the header comment says "target addresses"; we return the
                // *instruction* address instead because every consumer needs it
                // (CodeRefSite carries both, and the hook engine patches code).
                // The target is guaranteed to be inside [tableLo, tableHi).
                refs.push_back(insnAddr);
            }
        }
    });
}

bool ModuleForAddress(uintptr_t addr, ModuleInfo& out) {
    out = ModuleInfo{};
    EnsureProcess();
    std::vector<HMODULE> mods;
    if (!EnumModuleHandles(mods)) return false;
    for (HMODULE h : mods) {
        ModuleInfo mi;
        FillModuleInfo(h, mi);
        if (mi.base == 0) continue;
        if (addr >= mi.base && addr < mi.base + mi.size) {
            out = mi;
            return true;
        }
    }
    return false;
}

std::string Hex(uintptr_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return std::string(buf);
}

}  // namespace acz::mem
