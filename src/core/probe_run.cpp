// ACZPropNav - "Run A" live probe implementation (read-only).
//
// See core/probe.h for the design summary and docs/PROBE_RUN_A.md for what one
// run collects.  Safety rules obeyed here:
//   * the two detours never allocate, never take a lock, never log: fixed arrays
//     + Interlocked only;
//   * every read of game memory goes through mem::Read* / mem::ReadBytes (SEH
//     guarded by the mem engine) and every pointer is validated before use;
//   * no writes to game memory except the two code patches themselves; no
//     PAGE_GUARD, no pointer chasing, no new threads;
//   * every heavy pass is bounded (byte budget, hit caps, iteration caps) and the
//     log is flushed before it starts.
#include "core/probe.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "hook/hook.h"
#include "mem/mem.h"

namespace acz {
namespace {

// ---------------------------------------------------------------------------
// RVAs for acz.exe 4,046,008 B / SHA-256 3802B0DC... (docs/RUNTIME_PARAMS.md,
// docs/MSL_GUIDANCE.md).  RVA = VA - 0x140000000.
// ---------------------------------------------------------------------------
constexpr uint32_t kRvaAllocate = 0x201460;            // pool Allocate(pool,size,align)
constexpr uint32_t kRvaWeaponModelAllocRet = 0x0C767D;  // return addr of the WeaponModel alloc
constexpr uint32_t kRvaMissionSingletonRet = 0x16E14B;  // return addr of the 0xA8940 alloc
constexpr uint32_t kRvaNodePopEntry = 0x1B2C0;          // NOT hookable (jg rel8 at +3)
constexpr uint32_t kRvaNodePopHook = 0x1B2C8;           // the jg target: clean 14-byte prologue
constexpr uint32_t kRvaPoolSingleton = 0x3219A0;        // projectile pool singleton S
constexpr uint32_t kRvaGlobalList = 0x33EDB0;           // global time-sorted node list
constexpr uint32_t kRvaWeaponModelVtable = 0x2AE790;    // WeaponModel vftable
constexpr uint32_t kRvaWorldTick = 0x1B7B00;            // per-frame weapon/effect tick, rcx=world

// Expected prologues (verified offline against work/acz_copy.exe.unpacked.exe;
// also asserted by tools/hooktest/main.cpp Test7).
const uint8_t kAllocPrologue[17] = {0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x41, 0x55,
                                    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x10};
const uint8_t kPopPrologue[14] = {0x48, 0x8D, 0x91, 0x18, 0x02, 0x00, 0x00, 0x4C,
                                  0x8B, 0x42, 0x08, 0x4C, 0x3B, 0xC2};
// push rsi / sub rsp,0x80 / mov eax,[rcx+0xa8790]  (15 bytes, .pdata start at 0x1B7B00)
const uint8_t kWorldPrologue[15] = {0x40, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00,
                                    0x00, 0x8B, 0x81, 0x90, 0x87, 0x0A, 0x00};

// The six known node-allocation sites (function starts, from .pdata/xref); the
// captured return address always falls inside one of these ranges.
struct SpawnSite {
    uint32_t start;
    uint32_t size;
    const char* name;
};
const SpawnSite kSpawnSites[] = {
    {0x1254A2, 0x3D7, "spawn@0x1254A2"},
    {0x12B631, 0x795, "spawn@0x12B631"},
    {0x13B09B, 0x11A, "spawn@0x13B09B"},
    {0x13D665, 0x1E2, "spawn@0x13D665"},
    {0x1412B4, 0x2B4, "spawn@0x1412B4"},
    {0x1C7080, 0x16B, "spawn@0x1C7080"},
};
const char* SiteNameFor(uintptr_t rva) {
    for (const SpawnSite& s : kSpawnSites) {
        if (rva >= s.start && rva < s.start + s.size) return s.name;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
double Now() {
    LARGE_INTEGER f{}, c{};
    static double freq = 0.0;
    if (freq == 0.0) {
        ::QueryPerformanceFrequency(&f);
        freq = (f.QuadPart > 0) ? static_cast<double>(f.QuadPart) : 1.0;
    }
    ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

// Our own module: its static tables contain the very float patterns being
// scanned for, so it must never take part in a scan.
void OwnModuleAnchor() {}
struct OwnMod {
    uintptr_t base = 0;
    size_t size = 0;
};
const OwnMod& Own() {
    static OwnMod m;
    static bool init = false;
    if (!init) {
        init = true;
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(0x00000004u | 0x00000002u,
                                 reinterpret_cast<LPCWSTR>(&OwnModuleAnchor), &self) &&
            self) {
            MODULEINFO mi{};
            if (::GetModuleInformation(::GetCurrentProcess(), self, &mi, sizeof(mi))) {
                m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                m.size = mi.SizeOfImage;
            }
        }
    }
    return m;
}
bool InsideOwnModule(uintptr_t addr) {
    const OwnMod& m = Own();
    return m.base != 0 && addr >= m.base && addr < m.base + m.size;
}

void App(char* out, unsigned cap, unsigned& n, const char* fmt, ...) {
    if (out == nullptr || n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
    if (w > 0) {
        n += static_cast<unsigned>(w);
        if (n >= cap) n = cap - 1;
    }
}

// Value markers used by the parameter/WeaponModel dumps.
struct MarkFloat {
    float v;
    const char* tag;
};
const MarkFloat kMarkFloats[] = {
    {1600.f, "SCOPE1600"}, {3000.f, "RANGE3000"}, {2000.f, "V2000"},  {40.f, "LEAD40"},
    {80.f, "LEAD80"},      {28.f, "LEAD28"},      {10.f, "V10"},      {90.f, "ANG90"},
    {1000.f, "SPEED1000"},
};

bool NearVal(float a, float b) {
    float tol = std::fabs(b) * 0.01f;
    if (tol < 0.001f) tol = 0.001f;
    return std::fabs(a - b) <= tol;
}

// Marks the interesting values of one 16-byte row (f32 + u8/u16/i32 encodings).
void MarkRow(const uint8_t* row, char* out, unsigned cap, unsigned& n) {
    for (int i = 0; i < 4; ++i) {
        float v = 0.f;
        std::memcpy(&v, row + i * 4, 4);
        if (!std::isfinite(v)) continue;
        for (const MarkFloat& m : kMarkFloats) {
            if (NearVal(v, m.v)) {
                App(out, cap, n, " f+%02X=%s", i * 4, m.tag);
                break;
            }
        }
    }
    for (int off = 0; off + 1 < 16; off += 2) {
        uint16_t w = 0;
        std::memcpy(&w, row + off, 2);
        if (w == 1000) {
            App(out, cap, n, " u16+%02X=1000", off);
        } else if (w == 90) {
            App(out, cap, n, " u16+%02X=90", off);
        }
    }
    for (int off = 0; off + 3 < 16; off += 4) {
        int32_t w = 0;
        std::memcpy(&w, row + off, 4);
        if (w == 1000) App(out, cap, n, " i32+%02X=1000", off);
    }
    for (int off = 0; off < 16; ++off) {
        if (row[off] == 90) App(out, cap, n, " u8+%02X=90", off);
    }
}

// ---------------------------------------------------------------------------
// Shared state of the two detours.
//
// These arrays are written by game threads (inside the detours) and read by the
// mod thread (Tick); the writer publishes with InterlockedExchange of `ready`
// after the payload is stored, and the reader only consumes a slot it has not
// seen yet.  No allocation, no locks, no logging in the detour path.
// ---------------------------------------------------------------------------
uintptr_t g_exeBase = 0;

PVOID volatile g_allocTramp = nullptr;
PVOID volatile g_popTramp = nullptr;
PVOID volatile g_worldTramp = nullptr;
PVOID volatile g_world = nullptr;   // last rcx seen at exe+0x1B7B00 (= world)
volatile LONG64 g_worldFrames = 0;  // entries into the per-frame world tick
volatile LONG64 g_worldCalls = 0;

struct AllocCap {
    volatile LONG ready = 0;
    volatile LONG64 ptr = 0;
    volatile LONG64 size = 0;
    volatile LONG64 callerRva = 0;
};
struct PopCap {
    volatile LONG ready = 0;
    volatile LONG64 node = 0;
    volatile LONG64 callerRva = 0;
};
constexpr int kAllocCaps = 256;
constexpr int kPopCaps = 256;
AllocCap g_allocCaps[kAllocCaps];
PopCap g_popCaps[kPopCaps];
volatile LONG g_allocReserved = 0;
volatile LONG g_popReserved = 0;
volatile LONG g_allocOverflow = 0;
volatile LONG g_popOverflow = 0;
volatile LONG64 g_allocCalls = 0;
volatile LONG64 g_popCalls = 0;

// hook::Install makes the jump live before it returns the trampoline, so a
// detour can be entered a few microseconds early.  Waiting is the only safe
// answer: the original prologue is already overwritten, so calling a null
// trampoline would take the game down.
void* WaitTramp(PVOID volatile* slot) {
    void* t = *slot;
    for (int i = 0; i < 100 && t == nullptr; ++i) ::SwitchToThread();
    while (t == nullptr) {
        // The installer thread publishes within microseconds; it cannot be blocked
        // on us (it holds no lock the game needs for this).  Yield instead of
        // burning a core, and stay here rather than crash.
        ::SwitchToThread();
        t = *slot;
    }
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Detours (plain C++ functions: on x64 Windows there is one calling convention,
// and the stolen prologue is executed by the trampoline, so this is ABI-correct).
// ---------------------------------------------------------------------------

// exe+0x201460: rcx = pool, edx = size, r8d = align -> rax = new object.
// Hot: every pool allocation in the game.  Fast path = two RVA compares.
typedef void* (*AllocFn)(void*, uint32_t, uint32_t, void*);
extern "C" __attribute__((noinline)) void* AllocDetour(void* pool, uint32_t size,
                                                       uint32_t align, void* a4) {
    ::InterlockedIncrement64(&g_allocCalls);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    AllocFn f = reinterpret_cast<AllocFn>(WaitTramp(&g_allocTramp));
    void* r = f(pool, size, align, a4);
    const uintptr_t base = g_exeBase;
    const uintptr_t rva = (caller >= base) ? (caller - base) : ~static_cast<uintptr_t>(0);
    if (rva == kRvaWeaponModelAllocRet || rva == kRvaMissionSingletonRet) {
        const LONG slot = ::InterlockedIncrement(&g_allocReserved) - 1;
        if (slot < kAllocCaps) {
            AllocCap& c = g_allocCaps[slot];
            c.ptr = static_cast<LONG64>(reinterpret_cast<uintptr_t>(r));
            c.size = static_cast<LONG64>(size);
            c.callerRva = static_cast<LONG64>(rva);
            ::InterlockedExchange(&c.ready, 1);
        } else {
            ::InterlockedIncrement(&g_allocOverflow);
        }
    }
    return r;
}

// exe+0x1B2C8 (the jg target inside exe+0x1B2C0): rcx = pool singleton S,
// rax = popped node (or NULL).  Rare (a few per missile spawn).
typedef void* (*PopFn)(void*, void*, void*, void*);
extern "C" __attribute__((noinline)) void* PopDetour(void* s, void* a2, void* a3, void* a4) {
    ::InterlockedIncrement64(&g_popCalls);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    PopFn f = reinterpret_cast<PopFn>(WaitTramp(&g_popTramp));
    void* node = f(s, a2, a3, a4);
    if (node != nullptr) {
        const uintptr_t base = g_exeBase;
        const uintptr_t rva = (caller >= base) ? (caller - base) : ~static_cast<uintptr_t>(0);
        const LONG slot = ::InterlockedIncrement(&g_popReserved) - 1;
        if (slot < kPopCaps) {
            PopCap& c = g_popCaps[slot];
            c.node = static_cast<LONG64>(reinterpret_cast<uintptr_t>(node));
            c.callerRva = static_cast<LONG64>(rva);
            ::InterlockedExchange(&c.ready, 1);
        } else {
            ::InterlockedIncrement(&g_popOverflow);
        }
    }
    return node;
}

// ---------------------------------------------------------------------------
// exe+0x1B7B00: world = rcx; called once per frame as world-update phase 7
// (docs/MSL_ADVANCE.md §4.1).  The detour only records the world pointer and a
// frame counter; all validation and every read happen later, in Tick.
// ---------------------------------------------------------------------------
typedef uintptr_t (*WorldFn)(void*, void*, void*, void*);
extern "C" __attribute__((noinline)) uintptr_t WorldDetour(void* world, void* a2, void* a3,
                                                           void* a4) {
    ::InterlockedIncrement64(&g_worldCalls);
    ::InterlockedIncrement64(&g_worldFrames);
    if (world != nullptr) ::InterlockedExchangePointer(&g_world, world);
    WorldFn f = reinterpret_cast<WorldFn>(WaitTramp(&g_worldTramp));
    return f(world, a2, a3, a4);
}

namespace {

// ---------------------------------------------------------------------------
// Readable-region snapshot helpers shared by the two heavy scans.
// ---------------------------------------------------------------------------
struct ScanRegions {
    std::vector<mem::Region> regions;
    size_t totalBytes = 0;
    size_t count = 0;
};

// Only non-executable data pages matter for both scans (the game keeps these
// values in heap blocks / .data), and skipping code pages keeps the pass short
// and the u8/u16 hit sets meaningful.
ScanRegions CollectDataRegions() {
    ScanRegions out;
    std::vector<mem::Region> all;
    mem::EnumRegions(all, 0x1000);
    for (const mem::Region& r : all) {
        if (!r.readable || r.executable) continue;
        if (InsideOwnModule(r.base)) continue;
        out.regions.push_back(r);
        out.totalBytes += r.size;
    }
    out.count = out.regions.size();
    return out;
}

constexpr uint64_t kMaxScanBytes = 2ull * 1024 * 1024 * 1024;  // ~2 GB per pass

// ---------------------------------------------------------------------------
// R3 value sets (docs/RUNTIME_PARAMS.md §6 R3).
// ---------------------------------------------------------------------------
enum R3Kind {
    kR3_1600 = 0,
    kR3_2000,
    kR3_3000,
    kR3_40,
    kR3_80,
    kR3_1000f,
    kR3_1000i,
    kR3_90f,
    kR3_1000u16,
    kR3_90u16,
    kR3_90u8,
    kR3_Count
};
const char* kR3Name[kR3_Count] = {"1600.0f", "2000.0f", "3000.0f", "40.0f",   "80.0f",
                                  "1000.0f", "i32=1000", "90.0f",  "u16=1000", "u16=90",
                                  "u8=90"};
// The 4-byte patterns (1600.0f / 2000.0f / 3000.0f / 40.0f / 80.0f / 1000.0f /
// i32 1000 / 90.0f) are matched in a switch in the aligned loop below; the u16 and
// u8 forms are matched in the byte loop.
constexpr size_t kR3HitCap = 65536;

} // namespace

// ---------------------------------------------------------------------------
// Start / Shutdown
// ---------------------------------------------------------------------------
void ProbeModel::Start(uintptr_t exeBase) {
    if (active_) return;
    active_ = true;
    base_ = exeBase;
    start_ = Now();
    r3At_ = start_ + 60.0;  // fallback: if the allocator hook sees nothing

    ::InterlockedExchangePointer(&g_allocTramp, nullptr);
    ::InterlockedExchangePointer(&g_popTramp, nullptr);
    ::InterlockedExchangePointer(&g_worldTramp, nullptr);
    ::InterlockedExchangePointer(&g_world, nullptr);
    g_exeBase = exeBase;
    g_allocReserved = 0;
    g_popReserved = 0;
    g_allocOverflow = 0;
    g_popOverflow = 0;
    g_allocCalls = 0;
    g_popCalls = 0;
    g_worldFrames = 0;
    g_worldCalls = 0;

    L().Key("================ PROBE-A (Run A) ================");
    L().Key("PROBE-A: exe base %s; read-only live survey of the plain-MSL pipeline",
            mem::Hex(exeBase).c_str());
    L().Key("PROBE-A: hook 1 = exe+0x%X pool Allocate(pool,size,align); capture caller "
            "exe+0x%X (WeaponModel) / exe+0x%X (mission singleton 0xA8940)",
            kRvaAllocate, kRvaWeaponModelAllocRet, kRvaMissionSingletonRet);
    L().Key("PROBE-A: hook 2 = exe+0x%X node pop.  NOTE: the documented entry exe+0x%X is NOT "
            "hookable (byte +3 = 7F 03 jg rel8, a relative branch; no .pdata entry).  Its jg "
            "target exe+0x%X has a clean 14-byte prologue and is the only path that can return "
            "a node.",
            kRvaNodePopHook, kRvaNodePopEntry, kRvaNodePopHook);
    L().Key("PROBE-A: hook 3 = exe+0x%X per-frame world tick (rcx=world) -> 250 ms world/object/"
            "part snapshots (max %d)",
            kRvaWorldTick, kWorldSnaps);
    L().Key("PROBE-A: sampler: global list exe+0x%X (1/s x30), pool singleton S=exe+0x%X, "
            "8 nodes x 12 samples, 0x200-byte dumps at node+0x28 (max 8) incl. the flat .wpb "
            "record check, WeaponModel vtable exe+0x%X scan, R3 delta tally (one bounded pass)",
            kRvaGlobalList, kRvaPoolSingleton, kRvaWeaponModelVtable);
    L().Key("PROBE-A: no game memory is written; the only patches are the three code jumps above. "
            "Send ACZero\\ACZPropNav.key.log after the run (512 KiB cap, no rotation expected).");
    L().Flush();
}

void ProbeModel::Shutdown() {
    if (!active_) return;
    L().Key("PROBE-A: shutdown: allocHooked=%d popHooked=%d allocEvents=%d popEvents=%d "
            "wmVerified=%d globalWalks=%d",
            allocHooked_ ? 1 : 0, popHooked_ ? 1 : 0, allocEvents_, popEvents_, wmVerified_,
            globalWalks_);
    if (allocHooked_ && allocTarget_ != nullptr) {
        if (!hook::Remove(allocTarget_, allocInstall_)) {
            L().Key("PROBE-A: cannot restore the alloc prologue at %s",
                     mem::Hex(reinterpret_cast<uintptr_t>(allocTarget_)).c_str());
        }
        allocTarget_ = nullptr;
        allocHooked_ = false;
        ::InterlockedExchangePointer(&g_allocTramp, nullptr);
    }
    if (popHooked_ && popTarget_ != nullptr) {
        if (!hook::Remove(popTarget_, popInstall_)) {
            L().Key("PROBE-A: cannot restore the node-pop prologue at %s",
                     mem::Hex(reinterpret_cast<uintptr_t>(popTarget_)).c_str());
        }
        popTarget_ = nullptr;
        popHooked_ = false;
        ::InterlockedExchangePointer(&g_popTramp, nullptr);
    }
    if (worldHooked_ && worldTarget_ != nullptr) {
        if (!hook::Remove(worldTarget_, worldInstall_)) {
            L().Key("PROBE-A: cannot restore the world-tick prologue at %s",
                    mem::Hex(reinterpret_cast<uintptr_t>(worldTarget_)).c_str());
        }
        worldTarget_ = nullptr;
        worldHooked_ = false;
        ::InterlockedExchangePointer(&g_worldTramp, nullptr);
    }
    active_ = false;
}

// ---------------------------------------------------------------------------
// Deferred installation (acz.exe's .text is SteamStub-encrypted at DLL load;
// retry every 200 ms until the real bytes appear, give up after 60 s).
// ---------------------------------------------------------------------------
void ProbeModel::TickInstall(double now) {
    if ((now - lastInstallTry_) < 0.2) return;
    lastInstallTry_ = now;
    ++installTries_;

    if (!allocHooked_ && !allocGaveUp_) {
        if (InstallAllocHook()) {
            allocHooked_ = true;
        } else if (installTries_ >= 300) {
            allocGaveUp_ = true;
            L().Key("PROBE-A: alloc hook not installed after 60 s (bytes never matched / engine "
                    "refused).  Continuing read-only.");
        }
    }
    if (!popHooked_ && !popGaveUp_) {
        if (InstallPopHook()) {
            popHooked_ = true;
        } else if (installTries_ >= 300) {
            popGaveUp_ = true;
            L().Key("PROBE-A: node-pop hook not installed after 60 s.  Continuing read-only.");
        }
    }
    if (!worldHooked_ && !worldGaveUp_) {
        if (InstallWorldHook()) {
            worldHooked_ = true;
        } else if (installTries_ >= 300) {
            worldGaveUp_ = true;
            L().Key("PROBE-A: world-tick hook not installed after 60 s.  Continuing without the "
                    "world snapshots.");
        }
    }
    if ((!allocHooked_ || !popHooked_ || !worldHooked_) && installTries_ == 1) {
        L().Key("PROBE-A: waiting for the SteamStub-decrypted .text (retry every 200 ms, max 60 s) "
                "- this is expected");
    }
}
bool ProbeModel::InstallAllocHook() {
    const uintptr_t target = base_ + kRvaAllocate;
    uint8_t pro[sizeof(kAllocPrologue)] = {};
    if (!mem::ReadBytes(target, pro, sizeof(pro)) ||
        std::memcmp(pro, kAllocPrologue, sizeof(pro)) != 0) {
        return false;  // .text not decrypted yet (or a different game build)
    }
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&AllocDetour),
                       &tramp, ir, &why)) {
        if (!allocRefusalLogged_) {
            allocRefusalLogged_ = true;
            L().Key("PROBE-A: alloc hook refused by the engine: %s", why.c_str());
        }
        return false;
    }
    ::InterlockedExchangePointer(&g_allocTramp, tramp);
    allocInstall_ = ir;
    allocTarget_ = reinterpret_cast<void*>(target);
    L().Key("PROBE-A: HOOK exe+0x%X installed (stole %d byte(s), %s)", kRvaAllocate,
            static_cast<int>(ir.stolenBytes), ir.reason.c_str());
    return true;
}

bool ProbeModel::InstallPopHook() {
    const uintptr_t target = base_ + kRvaNodePopHook;
    uint8_t pro[sizeof(kPopPrologue)] = {};
    if (!mem::ReadBytes(target, pro, sizeof(pro)) ||
        std::memcmp(pro, kPopPrologue, sizeof(pro)) != 0) {
        return false;  // .text not decrypted yet
    }
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&PopDetour),
                       &tramp, ir, &why)) {
        if (!popRefusalLogged_) {
            popRefusalLogged_ = true;
            L().Key("PROBE-A: node-pop hook refused by the engine: %s", why.c_str());
        }
        return false;
    }
    ::InterlockedExchangePointer(&g_popTramp, tramp);
    popInstall_ = ir;
    popTarget_ = reinterpret_cast<void*>(target);
    L().Key("PROBE-A: HOOK exe+0x%X installed (the jg target of exe+0x%X; stole %d byte(s), %s)",
            kRvaNodePopHook, kRvaNodePopEntry, static_cast<int>(ir.stolenBytes),
            ir.reason.c_str());
    return true;
}

bool ProbeModel::InstallWorldHook() {
    const uintptr_t target = base_ + kRvaWorldTick;
    uint8_t pro[sizeof(kWorldPrologue)] = {};
    if (!mem::ReadBytes(target, pro, sizeof(pro)) ||
        std::memcmp(pro, kWorldPrologue, sizeof(pro)) != 0) {
        return false;  // .text not decrypted yet
    }
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&WorldDetour),
                       &tramp, ir, &why)) {
        if (!worldRefusalLogged_) {
            worldRefusalLogged_ = true;
            L().Key("PROBE-A: world-tick hook refused by the engine: %s", why.c_str());
        }
        return false;
    }
    ::InterlockedExchangePointer(&g_worldTramp, tramp);
    worldInstall_ = ir;
    worldTarget_ = reinterpret_cast<void*>(target);
    L().Key("PROBE-A: HOOK exe+0x%X installed (per-frame world tick, rcx=world; stole %d "
            "byte(s), %s)",
            kRvaWorldTick, static_cast<int>(ir.stolenBytes), ir.reason.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Drain the lock-free capture arrays (this is the only place they are logged).
// ---------------------------------------------------------------------------
void ProbeModel::DrainCaptures() {
    const double now = Now();

    // ---- pool Allocate events (only the two filtered callers are recorded) ----
    const LONG allocReserved = g_allocReserved;
    for (LONG i = 0; i < allocReserved && i < kAllocCaps; ++i) {
        if (g_allocCaps[i].ready == 0 || allocSeen_[i]) continue;
        allocSeen_[i] = true;
        ++allocEvents_;
        const uintptr_t rva = static_cast<uintptr_t>(g_allocCaps[i].callerRva);
        const uintptr_t ptr = static_cast<uintptr_t>(g_allocCaps[i].ptr);
        const uint32_t size = static_cast<uint32_t>(g_allocCaps[i].size);
        if (allocLogged_ < 64) {
            ++allocLogged_;
            L().Key("PROBE-A: ALLOC caller=exe+0x%llX size=0x%X (%u B) ptr=%s%s",
                    static_cast<unsigned long long>(rva), size, size, mem::Hex(ptr).c_str(),
                    rva == kRvaWeaponModelAllocRet ? "  [WeaponModel: 0x48 + payload]"
                                                   : "  [mission singleton: 0xA8940]");
        }
        if (rva == kRvaMissionSingletonRet && !singletonSeen_) {
            singletonSeen_ = true;
            singletonAt_ = now;
            wmScanWanted_ = true;
            wmScanGaveUp_ = false;
            nextWmScan_ = now + 2.0;
            r3At_ = now + 45.0;
            L().Key("PROBE-A: mission singleton allocated at %s; WeaponModel scan in 2 s "
                    "(retry 10 s, max 5), R3 tally at +45 s",
                    mem::Hex(ptr).c_str());
        }
        if (rva == kRvaWeaponModelAllocRet && pendingWmCount_ < kPendingWm) {
            PendingWm& p = pendingWm_[pendingWmCount_++];
            p.ptr = ptr;
            p.size = size;
            p.due = now + 0.2;  // the vftable is stored a few instructions later
        }
    }
    if (g_allocOverflow > 0 && !allocOverflowLogged_) {
        allocOverflowLogged_ = true;
        L().Key("PROBE-A: alloc capture overflow: %ld event(s) dropped (cap %d)",
                static_cast<long>(g_allocOverflow), kAllocCaps);
    }

    // ---- deferred class test of the WeaponModel allocations (R2) --------------
    for (int i = pendingWmCount_ - 1; i >= 0; --i) {
        PendingWm& p = pendingWm_[i];
        if (p.due > now) continue;
        const uintptr_t vt = base_ + kRvaWeaponModelVtable;
        uintptr_t first = 0;
        if (!mem::Read<uintptr_t>(p.ptr, first)) {
            L().Key("PROBE-A: WM-ALLOC ptr=%s size=0x%X: unreadable one frame later",
                    mem::Hex(p.ptr).c_str(), p.size);
        } else if (first == vt) {
            ++wmVerified_;
            L().Key("PROBE-A: WM-ALLOC VERIFIED ptr=%s size=0x%X -> payload=0x%X (vftable="
                    "WeaponModel)",
                    mem::Hex(p.ptr).c_str(), p.size, p.size - 0x48);
        } else {
            L().Key("PROBE-A: WM-ALLOC ptr=%s size=0x%X: first qword %s != WeaponModel vtable",
                    mem::Hex(p.ptr).c_str(), p.size, mem::Hex(first).c_str());
        }
        pendingWm_[i] = pendingWm_[pendingWmCount_ - 1];
        --pendingWmCount_;
    }

    // ---- node pop events ------------------------------------------------------
    const LONG popReserved = g_popReserved;
    for (LONG i = 0; i < popReserved && i < kPopCaps; ++i) {
        if (g_popCaps[i].ready == 0 || popSeen_[i]) continue;
        popSeen_[i] = true;
        ++popEvents_;
        const uintptr_t node = static_cast<uintptr_t>(g_popCaps[i].node);
        const uintptr_t rva = static_cast<uintptr_t>(g_popCaps[i].callerRva);
        const char* site = SiteNameFor(rva);
        if (popLogged_ < 64) {
            ++popLogged_;
            L().Key("PROBE-A: POP node=%s caller=exe+0x%llX %s", mem::Hex(node).c_str(),
                    static_cast<unsigned long long>(rva), site ? site : "[unknown call site]");
        }
        AddSampleNode(node);
        // The parameter object pointer lives at node+0x28 (dispatcher decompile).
        uintptr_t param = 0;
        if (mem::Read<uintptr_t>(node + 0x28, param)) DumpParamObject(node, param);
    }
    if (g_popOverflow > 0 && !popOverflowLogged_) {
        popOverflowLogged_ = true;
        L().Key("PROBE-A: pop capture overflow: %ld event(s) dropped (cap %d)",
                static_cast<long>(g_popOverflow), kPopCaps);
    }
}

// ---------------------------------------------------------------------------
// Node sampling: up to 8 captured pool nodes, 12 samples ~200 ms apart.
// ---------------------------------------------------------------------------
void ProbeModel::AddSampleNode(uintptr_t node) {
    if (node == 0 || sampleNodeCount_ >= kSampleNodes) return;
    for (int i = 0; i < sampleNodeCount_; ++i) {
        if (sampleNodes_[i] == node) return;  // recycled node, already tracked
    }
    const int idx = sampleNodeCount_++;
    sampleNodes_[idx] = node;
    sampleDead_[idx] = false;
    uint32_t cur[0x70 / 4] = {};
    if (!mem::ReadBytes(node, cur, sizeof(cur))) {
        sampleDead_[idx] = true;
        return;
    }
    std::memcpy(samplePrev_[idx], cur, sizeof(cur));
    L().Key("PROBE-A: sampling node %s (slot %d/%d) - 12 samples, 200 ms apart",
            mem::Hex(node).c_str(), idx + 1, kSampleNodes);
}

void ProbeModel::NodeSampleTick(double now) {
    if (sampleNodeCount_ == 0) {
        if (!idleHintLogged_ && (now - start_) > 90.0) {
            idleHintLogged_ = true;
            L().Key("PROBE-A: no pool node captured yet.  Fire a few plain MSL (no special "
                    "weapon) and watch the key log.");
        }
        return;
    }
    if (sampleRound_ >= kSampleRounds) return;
    if ((now - lastSample_) < 0.2) return;
    lastSample_ = now;
    ++sampleRound_;

    for (int i = 0; i < sampleNodeCount_; ++i) {
        if (sampleDead_[i]) continue;
        const uintptr_t node = sampleNodes_[i];
        uint32_t cur[0x70 / 4] = {};
        if (!mem::ReadBytes(node, cur, sizeof(cur))) {
            sampleDead_[i] = true;
            L().Key("PROBE-A: node %s became unreadable (freed); stopping its samples",
                    mem::Hex(node).c_str());
            continue;
        }
        char diff[448] = {};
        unsigned dn = 0;
        int changed = 0;
        for (int w = 0; w < 0x70 / 4; ++w) {
            if (cur[w] == samplePrev_[i][w]) continue;
            ++changed;
            float a = 0.f, b = 0.f;
            std::memcpy(&a, &samplePrev_[i][w], 4);
            std::memcpy(&b, &cur[w], 4);
            App(diff, sizeof(diff), dn, " +0x%02X:%08X>%08X(%.4g>%.4g)", w * 4,
                samplePrev_[i][w], cur[w], static_cast<double>(a), static_cast<double>(b));
        }
        std::memcpy(samplePrev_[i], cur, sizeof(cur));
        if (changed > 0 || sampleRound_ == 1) {
            L().Key("PROBE-A: SAMPLE %d/%d node=%s changed=%d%s", sampleRound_, kSampleRounds,
                    mem::Hex(node).c_str(), changed, diff);
        }
        // Explicit field view (Lead request: the 3 vectors + the scalar + the ids).
        float v20 = 0.f, v24 = 0.f, v28 = 0.f, v2c = 0.f;
        float v30 = 0.f, v34 = 0.f, v38 = 0.f, v3c = 0.f;
        float v40 = 0.f, v44 = 0.f, v48 = 0.f, v4c = 0.f;
        std::memcpy(&v20, &cur[0x20 / 4], 4);
        std::memcpy(&v24, &cur[0x24 / 4], 4);
        std::memcpy(&v28, &cur[0x28 / 4], 4);
        std::memcpy(&v2c, &cur[0x2C / 4], 4);
        std::memcpy(&v30, &cur[0x30 / 4], 4);
        std::memcpy(&v34, &cur[0x34 / 4], 4);
        std::memcpy(&v38, &cur[0x38 / 4], 4);
        std::memcpy(&v3c, &cur[0x3C / 4], 4);
        std::memcpy(&v40, &cur[0x40 / 4], 4);
        std::memcpy(&v44, &cur[0x44 / 4], 4);
        std::memcpy(&v48, &cur[0x48 / 4], 4);
        std::memcpy(&v4c, &cur[0x4C / 4], 4);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cur);
        int32_t t60 = 0;
        uint16_t u64 = 0;
        std::memcpy(&t60, bytes + 0x60, 4);
        std::memcpy(&u64, bytes + 0x64, 2);
        L().Key("PROBE-A:   v20=[%.4g %.4g %.4g %.4g] v30=[%.4g %.4g %.4g %.4g] "
                "v40=[%.4g %.4g %.4g %.4g] | t60=%d u64=%u ids=%u/%u team=%u st=%u",
                static_cast<double>(v20), static_cast<double>(v24), static_cast<double>(v28),
                static_cast<double>(v2c), static_cast<double>(v30), static_cast<double>(v34),
                static_cast<double>(v38), static_cast<double>(v3c), static_cast<double>(v40),
                static_cast<double>(v44), static_cast<double>(v48), static_cast<double>(v4c), t60,
                u64, static_cast<unsigned>(bytes[0x66]), static_cast<unsigned>(bytes[0x67]),
                static_cast<unsigned>(bytes[0x68]), static_cast<unsigned>(bytes[0x6B]));
    }
    SingletonSnapshot();
}

// ---------------------------------------------------------------------------
// Singleton S = exe+0x3219A0: list heads / counts (read-only).
// ---------------------------------------------------------------------------
void ProbeModel::SingletonSnapshot() {
    const uintptr_t s = base_ + kRvaPoolSingleton;
    uintptr_t l1Head = 0, l1Tail = 0, l2Head = 0, l2Tail = 0, l5Head = 0, l5Tail = 0, cached = 0;
    int32_t l1Count = 0, l3Count = 0, l4Count = 0, l5Count = 0;
    if (!mem::Read<uintptr_t>(s + 0x20, l1Head) || !mem::Read<uintptr_t>(s + 0x28, l1Tail) ||
        !mem::Read<uintptr_t>(s + 0x98, l2Head) || !mem::Read<uintptr_t>(s + 0xA0, l2Tail) ||
        !mem::Read<uintptr_t>(s + 0x290, l5Head) || !mem::Read<uintptr_t>(s + 0x298, l5Tail) ||
        !mem::Read<uintptr_t>(s + 0x318, cached)) {
        if (singletonSnaps_ == 0) {
            L().Key("PROBE-A: singleton S=%s is not readable yet", mem::Hex(s).c_str());
        }
        return;
    }
    mem::Read<int32_t>(s + 0x90, l1Count);
    mem::Read<int32_t>(s + 0x210, l3Count);
    mem::Read<int32_t>(s + 0x288, l4Count);
    mem::Read<int32_t>(s + 0x300, l5Count);
    ++singletonSnaps_;
    L().Key("PROBE-A:   S=%s L1[+20]=%s/+28=%s n90=%d | L2[+98]=%s/+A0=%s | "
            "free3[+1A0]n210=%d free4[+218]n288=%d | L5[+290]=%s/+2A0=%s n300=%d | cached+318=%s",
            mem::Hex(s).c_str(), mem::Hex(l1Head).c_str(), mem::Hex(l1Tail).c_str(), l1Count,
            mem::Hex(l2Head).c_str(), mem::Hex(l2Tail).c_str(), l3Count, l4Count,
            mem::Hex(l5Head).c_str(), mem::Hex(l5Tail).c_str(), l5Count,
            mem::Hex(cached).c_str());
}

// ---------------------------------------------------------------------------
// Global time-sorted node list at exe+0x33EDB0 (Lead's dispatcher decompile).
//   node+0x00 next, +0x20 u16 category, +0x28 qword param ptr, +0x60 i32 time,
//   +0x6B u8 state, floats +0x2c..+0x4c.
// ---------------------------------------------------------------------------
void ProbeModel::GlobalListTick(double now) {
    if (globalWalks_ >= 30) return;
    if ((now - lastGlobalWalk_) < 1.0) return;
    lastGlobalWalk_ = now;
    if ((now - start_) > 600.0) return;  // do not keep polling forever

    const uintptr_t head = base_ + kRvaGlobalList;
    uintptr_t first = 0;
    if (!mem::Read<uintptr_t>(head, first)) {
        ++globalEmpty_;
        if ((globalEmpty_ % 10) == 1) {
            L().Key("PROBE-A: GLIST head %s unreadable", mem::Hex(head).c_str());
        }
        return;
    }
    if (first == 0 || first == head) {
        ++globalEmpty_;
        if ((globalEmpty_ % 10) == 1) {
            L().Key("PROBE-A: GLIST head=%s empty (nothing spawned yet)", mem::Hex(head).c_str());
        }
        return;
    }

    ++globalWalks_;
    uintptr_t seen[64] = {};
    int seenCount = 0;
    int logged = 0;
    int count = 0;
    uintptr_t node = first;
    while (node != 0 && node != head && count < 64) {
        bool repeat = false;
        for (int i = 0; i < seenCount; ++i) {
            if (seen[i] == node) {
                repeat = true;
                break;
            }
        }
        if (repeat) {
            L().Key("PROBE-A: GLIST walk stopped: pointer %s repeats (cycle)",
                    mem::Hex(node).c_str());
            break;
        }
        if (seenCount < 64) seen[seenCount++] = node;
        uint8_t buf[0x70] = {};
        if (!mem::ReadBytes(node, buf, sizeof(buf))) {
            L().Key("PROBE-A: GLIST walk stopped: node %s unreadable", mem::Hex(node).c_str());
            break;
        }
        uintptr_t next = 0;
        std::memcpy(&next, buf + 0x00, 8);
        int32_t t60 = 0;
        std::memcpy(&t60, buf + 0x60, 4);
        uint16_t cat = 0;
        std::memcpy(&cat, buf + 0x20, 2);
        uintptr_t param = 0;
        std::memcpy(&param, buf + 0x28, 8);
        float f2c = 0.f, f30 = 0.f, f34 = 0.f, f38 = 0.f, f3c = 0.f, f40 = 0.f, f44 = 0.f,
              f48 = 0.f, f4c = 0.f;
        std::memcpy(&f2c, buf + 0x2C, 4);
        std::memcpy(&f30, buf + 0x30, 4);
        std::memcpy(&f34, buf + 0x34, 4);
        std::memcpy(&f38, buf + 0x38, 4);
        std::memcpy(&f3c, buf + 0x3C, 4);
        std::memcpy(&f40, buf + 0x40, 4);
        std::memcpy(&f44, buf + 0x44, 4);
        std::memcpy(&f48, buf + 0x48, 4);
        std::memcpy(&f4c, buf + 0x4C, 4);
        if (logged < 5) {
            ++logged;
            L().Key("PROBE-A:   gnode %s next=%s t60=%d st=%u cat=%u param=%s "
                    "f2c=%.4g f30=%.4g f34=%.4g f38=%.4g f3c=%.4g f40=%.4g f44=%.4g "
                    "f48=%.4g f4c=%.4g",
                    mem::Hex(node).c_str(), mem::Hex(next).c_str(), t60,
                    static_cast<unsigned>(buf[0x6B]), static_cast<unsigned>(cat),
                    mem::Hex(param).c_str(), static_cast<double>(f2c), static_cast<double>(f30),
                    static_cast<double>(f34), static_cast<double>(f38), static_cast<double>(f3c),
                    static_cast<double>(f40), static_cast<double>(f44), static_cast<double>(f48),
                    static_cast<double>(f4c));
        }
        node = next;
        ++count;
    }
    L().Key("PROBE-A: GLIST walk %d/30: head=%s first=%s nodes=%d%s", globalWalks_,
            mem::Hex(head).c_str(), mem::Hex(first).c_str(), count,
            count >= 64 ? " (capped at 64)" : "");
}

// ---------------------------------------------------------------------------
// Parameter object at node+0x28: dump 0x200 bytes once per unique pointer.
// ---------------------------------------------------------------------------
void ProbeModel::DumpParamObject(uintptr_t node, uintptr_t paramPtr) {
    if (paramPtr < 0x10000) return;  // almost certainly a float pair, not a pointer
    if (InsideOwnModule(paramPtr)) return;
    for (int i = 0; i < paramPtrCount_; ++i) {
        if (paramPtrs_[i] == paramPtr) return;  // already dumped
    }
    if (paramPtrCount_ >= kParamDumps) {
        if (paramPtrOverflow_ == 0) {
            L().Key("PROBE-A: PARAM dump limit reached (%d unique pointers); not dumping %s",
                    kParamDumps, mem::Hex(paramPtr).c_str());
        }
        ++paramPtrOverflow_;
        return;
    }
    if (!mem::IsReadable(paramPtr, 0x200)) {
        L().Key("PROBE-A: PARAM node=%s has +0x28=%s but that is not 0x200 readable bytes",
                mem::Hex(node).c_str(), mem::Hex(paramPtr).c_str());
        return;
    }
    paramPtrs_[paramPtrCount_++] = paramPtr;
    L().Key("PROBE-A: PARAM object #%d node=%s ptr=%s (0x200 bytes, offsets relative to ptr)",
            paramPtrCount_, mem::Hex(node).c_str(), mem::Hex(paramPtr).c_str());

    // ---- Explicit check of the flat .wpb record layout (docs/WEAPON_MODEL_LAYOUT.md):
    //      P = node+0x28; P-0x10 has u32 data_off = 0x10; P+0x10 scope, P+0x14 range,
    //      P+0x24 timeStep, P+0x2C lead, P+0x44 speed (u32), P+0x48 type u8,
    //      P+0x49 attack u8, P+0x4A startAng u8.  The record is 'mis'-specific for a
    //      player MSL; misf/misf2/mis_old differ (range/lead/startAng), so the actual
    //      values are logged regardless and a tolerant test names the variant. ----
    const uintptr_t p = paramPtr;
    if (mem::IsReadable(p - 0x10, 0x60)) {
        uint8_t head[0x60] = {};
        if (mem::ReadBytes(p - 0x10, head, sizeof(head))) {
            uint32_t dataOff = 0;
            std::memcpy(&dataOff, head + 0x00, 4);  // P-0x10
            float scope = 0.f, range = 0.f, timeStep = 0.f, lead = 0.f;
            uint32_t speed = 0;
            uint8_t type = 0, attack = 0, startAng = 0;
            std::memcpy(&scope, head + 0x20, 4);    // P+0x10
            std::memcpy(&range, head + 0x24, 4);    // P+0x14
            std::memcpy(&timeStep, head + 0x34, 4); // P+0x24
            std::memcpy(&lead, head + 0x3C, 4);     // P+0x2C
            std::memcpy(&speed, head + 0x54, 4);    // P+0x44
            type = head[0x58];                      // P+0x48
            attack = head[0x59];                    // P+0x49
            startAng = head[0x5A];                  // P+0x4A
            L().Key("PROBE-A:   rec P-0x10: %08X %08X %08X %08X -> data_off=0x%X",
                    *reinterpret_cast<const uint32_t*>(head + 0x00),
                    *reinterpret_cast<const uint32_t*>(head + 0x04),
                    *reinterpret_cast<const uint32_t*>(head + 0x08),
                    *reinterpret_cast<const uint32_t*>(head + 0x0C), dataOff);
            L().Key("PROBE-A:   rec P+0x10 scope=%.4g  P+0x14 range=%.4g  P+0x24 timeStep=%.4g  "
                    "P+0x2C lead=%.4g  P+0x44 speed=%u  P+0x48 type=%u  P+0x49 attack=%u  "
                    "P+0x4A startAng=%u",
                    static_cast<double>(scope), static_cast<double>(range),
                    static_cast<double>(timeStep), static_cast<double>(lead), speed,
                    static_cast<unsigned>(type), static_cast<unsigned>(attack),
                    static_cast<unsigned>(startAng));
            uint64_t q0 = 0, q1 = 0, q2 = 0, q3 = 0;
            std::memcpy(&q0, head + 0x20, 8);  // P+0x10
            std::memcpy(&q1, head + 0x28, 8);  // P+0x18
            std::memcpy(&q2, head + 0x30, 8);  // P+0x20
            std::memcpy(&q3, head + 0x38, 8);  // P+0x28
            L().Key("PROBE-A:   rec qwords P+0x10=%s P+0x18=%s P+0x20=%s P+0x28=%s",
                    mem::Hex(static_cast<uintptr_t>(q0)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(q1)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(q2)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(q3)).c_str());

            const bool scopeOk = NearVal(scope, 1600.f);
            const bool dataOk = (dataOff == 0x10u);
            const bool typeOk = (type == 2u);
            const bool speedOk = (speed == 1000u);
            const char* variant = "unknown";
            if (NearVal(range, 3000.f) && NearVal(lead, 40.f) && startAng == 90) variant = "mis";
            else if (NearVal(range, 2000.f) && NearVal(lead, 80.f) && startAng == 60) variant = "misf";
            else if (NearVal(lead, 28.f) && startAng == 30) variant = "misf2";
            else if (NearVal(lead, 10.f)) variant = "mis_old";
            const bool validated = dataOk && scopeOk && typeOk && speedOk;
            if (validated && !flatRecordSeen_) {
                flatRecordSeen_ = true;
                L().Key("PROBE-A:   rec VALIDATED (flat .wpb record, variant=%s): the values live "
                        "at P+0x10.. as a plain struct; WeaponModel vtable scan and R3 tally are "
                        "SKIPPED for the rest of this run",
                        variant);
            } else if (!validated) {
                L().Key("PROBE-A:   rec NOT validated (data_off=0x%X scope1600=%d type2=%d "
                        "speed1000=%d)",
                        dataOff, scopeOk ? 1 : 0, typeOk ? 1 : 0, speedOk ? 1 : 0);
            }
        }
    }
    DumpMarkedObject("p", paramPtr, 0, 0x200);
}

void ProbeModel::DumpMarkedObject(const char* tag, uintptr_t obj, uint32_t from, uint32_t to) {
    const uint32_t firstOff = from & ~0xFu;
    for (uint32_t off = firstOff; off < to; off += 16) {
        uint8_t row[16] = {};
        if (!mem::ReadBytes(obj + off, row, sizeof(row))) break;
        uint32_t dw[4] = {};
        std::memcpy(dw, row, sizeof(dw));
        float f[4] = {};
        std::memcpy(f, row, sizeof(f));
        char marks[224] = {};
        unsigned mn = 0;
        MarkRow(row, marks, sizeof(marks), mn);
        L().Key("PROBE-A:   %s+0x%03X: %08X %08X %08X %08X | %11.5g %11.5g %11.5g %11.5g |%s",
                tag, off, dw[0], dw[1], dw[2], dw[3], static_cast<double>(f[0]),
                static_cast<double>(f[1]), static_cast<double>(f[2]),
                static_cast<double>(f[3]), marks);
    }
}

// ---------------------------------------------------------------------------
// WeaponModel scan: qword == exeBase + 0x2AE790 (the only reliable class test).
// Runs after the mission singleton alloc event, retried every 10 s, max 5 times.
// ---------------------------------------------------------------------------
void ProbeModel::WeaponModelScan() {
    ++wmScans_;
    L().Key("PROBE-A: WM scan %d/5 starting (flushing the log first)", wmScans_);
    L().Flush();

    const uintptr_t vt = base_ + kRvaWeaponModelVtable;
    std::vector<uintptr_t> hits;
    const ScanRegions regs = CollectDataRegions();
    uint64_t scanned = 0;
    bool budgetHit = false;
    for (const mem::Region& r : regs.regions) {
        if (scanned >= kMaxScanBytes) {
            budgetHit = true;
            break;
        }
        mem::ScanBytes(r, reinterpret_cast<const uint8_t*>(&vt), "xxxxxxxx", sizeof(vt), hits, 64);
        scanned += r.size;
        if (hits.size() >= 64) break;
    }
    L().Key("PROBE-A: WM scan %d: %d hit(s); scanned %.1f MB of %.1f MB over %d data region(s)%s",
            wmScans_, static_cast<int>(hits.size()), static_cast<double>(scanned) / 1048576.0,
            static_cast<double>(regs.totalBytes) / 1048576.0, static_cast<int>(regs.count),
            budgetHit ? " (byte budget reached)" : "");

    int dumpedThisPass = 0;
    for (uintptr_t obj : hits) {
        bool known = false;
        for (int i = 0; i < wmDumpedCount_; ++i) {
            if (wmDumped_[i] == obj) {
                known = true;
                break;
            }
        }
        if (known) continue;
        if (wmDumpedCount_ >= kWmDumps) break;
        wmDumped_[wmDumpedCount_++] = obj;
        ++dumpedThisPass;
        L().Key("PROBE-A: WM object %s (#%d) - payload obj+0x48..0x300, offsets relative to obj",
                mem::Hex(obj).c_str(), wmDumpedCount_);
        DumpMarkedObject("w", obj, 0x48, 0x300);
    }
    if (dumpedThisPass == 0 && !hits.empty()) {
        L().Key("PROBE-A: WM scan %d: no new object to dump (all %d already dumped)", wmScans_,
                wmDumpedCount_);
    }

    if (wmScans_ >= 5 || (wmDumpedCount_ >= kWmDumps && wmScans_ >= 2)) {
        wmScanWanted_ = false;
        if (!r3Done_) r3At_ = std::min(r3At_, Now() + 2.0);
        L().Key("PROBE-A: WeaponModel scan finished (%d scan(s), %d object(s) dumped)",
                wmScans_, wmDumpedCount_);
    } else {
        nextWmScan_ = Now() + 10.0;
    }
}

// ---------------------------------------------------------------------------
// R3 anchored-delta tally (docs/RUNTIME_PARAMS.md §6 R3).
//
// One combined pass over the data regions collects the addresses of all the
// values of interest, then the distances anchor->companion are tallied.  This is
// exactly the R3 experiment ("do not look for a contiguous payload; look for the
// stride between the values") but needs one memory pass instead of one per value.
// ---------------------------------------------------------------------------
void ProbeModel::R3ScanAndTally() {
    L().Key("PROBE-A: R3 starting (one bounded pass over data regions; flushing the log first)");
    L().Flush();

    std::vector<uintptr_t> hits[kR3_Count];
    size_t truncated[kR3_Count] = {};
    const ScanRegions regs = CollectDataRegions();
    uint64_t scanned = 0;
    bool budgetHit = false;
    std::vector<uint8_t> buf(0x10000 + 16);

    auto push = [&](int kind, uintptr_t addr) {
        if (hits[kind].size() < kR3HitCap) {
            hits[kind].push_back(addr);
        } else {
            ++truncated[kind];
        }
    };

    for (const mem::Region& r : regs.regions) {
        if (budgetHit) break;
        for (size_t off = 0; off < r.size; off += 0x10000) {
            if (scanned >= kMaxScanBytes) {
                budgetHit = true;
                break;
            }
            const size_t len = std::min<size_t>(0x10000, r.size - off);
            if (len < 8) break;
            if (!mem::ReadBytes(r.base + off, buf.data(), len)) continue;
            const uint8_t* p = buf.data();
            // 4-byte aligned f32 / i32 patterns.
            const size_t n4 = len & ~static_cast<size_t>(3);
            for (size_t i = 0; i < n4; i += 4) {
                uint32_t w = 0;
                std::memcpy(&w, p + i, 4);
                switch (w) {
                    case 0x44C80000u: push(kR3_1600, r.base + off + i); break;
                    case 0x44FA0000u: push(kR3_2000, r.base + off + i); break;
                    case 0x453B8000u: push(kR3_3000, r.base + off + i); break;
                    case 0x42200000u: push(kR3_40, r.base + off + i); break;
                    case 0x42A00000u: push(kR3_80, r.base + off + i); break;
                    case 0x447A0000u: push(kR3_1000f, r.base + off + i); break;
                    case 0x42B40000u: push(kR3_90f, r.base + off + i); break;
                    case 0x000003E8u: push(kR3_1000i, r.base + off + i); break;
                    default: break;
                }
            }
            // u16 / u8 encodings (any alignment).
            for (size_t i = 0; i + 1 < len; ++i) {
                const uint16_t w = static_cast<uint16_t>(p[i] | (static_cast<uint16_t>(p[i + 1]) << 8));
                if (w == 0x03E8u) push(kR3_1000u16, r.base + off + i);
                if (w == 0x005Au) push(kR3_90u16, r.base + off + i);
                if (p[i] == 0x5Au) push(kR3_90u8, r.base + off + i);
            }
            scanned += len;
        }
    }

    L().Key("PROBE-A: R3 scan: %llu MB over %d data region(s)%s",
            static_cast<unsigned long long>(scanned / 1048576ull), static_cast<int>(regs.count),
            budgetHit ? " (2 GB budget reached - coverage partial)" : " (complete)");
    {
        char line[512] = {};
        unsigned n = 0;
        for (int k = 0; k < kR3_Count; ++k) {
            App(line, sizeof(line), n, " %s=%d%s", kR3Name[k],
                static_cast<int>(hits[k].size()),
                truncated[k] ? "(+trunc)" : "");
        }
        L().Key("PROBE-A: R3 hits:%s", line);
    }

    // ---- tally: for each anchor, distances to each companion within +-0x2000 ----
    static const int kAnchors[2] = {kR3_1600, kR3_2000};
    static const int kComps[9] = {kR3_3000, kR3_40,      kR3_80,   kR3_1000f, kR3_1000i,
                                  kR3_1000u16, kR3_90f,  kR3_90u16, kR3_90u8};
    constexpr int kSpan = 0x2000;
    std::vector<int> tally(static_cast<size_t>(2 * kSpan + 1), 0);
    std::vector<uintptr_t> example(static_cast<size_t>(2 * kSpan + 1), 0);

    for (int ai = 0; ai < 2; ++ai) {
        const int a = kAnchors[ai];
        if (hits[a].empty()) continue;
        std::sort(hits[a].begin(), hits[a].end());
        const size_t usedAnchors = std::min<size_t>(hits[a].size(), 32768);
        for (int ci = 0; ci < 9; ++ci) {
            const int c = kComps[ci];
            if (hits[c].empty()) continue;
            std::sort(hits[c].begin(), hits[c].end());
            std::fill(tally.begin(), tally.end(), 0);
            std::fill(example.begin(), example.end(), 0);
            for (size_t i = 0; i < usedAnchors; ++i) {
                const uintptr_t at = hits[a][i];
                const uintptr_t lo = (at > static_cast<uintptr_t>(kSpan)) ? at - kSpan : 0;
                const uintptr_t hi = at + static_cast<uintptr_t>(kSpan);
                auto it = std::lower_bound(hits[c].begin(), hits[c].end(), lo);
                for (; it != hits[c].end() && *it <= hi; ++it) {
                    const long long d = static_cast<long long>(*it) - static_cast<long long>(at);
                    if (d < -kSpan || d > kSpan) continue;
                    const size_t idx = static_cast<size_t>(d + kSpan);
                    ++tally[idx];
                    if (example[idx] == 0) example[idx] = at;
                }
            }
            // top 10 deltas by count
            int best[10] = {};
            int bestD[10] = {};
            int bestN = 0;
            for (int d = -kSpan; d <= kSpan; ++d) {
                const int cnt = tally[static_cast<size_t>(d + kSpan)];
                if (cnt <= 0) continue;
                // insert into the small top list
                if (bestN < 10 || cnt > best[bestN - 1]) {
                    int pos = (bestN < 10) ? bestN++ : bestN - 1;
                    while (pos > 0 && cnt > best[pos - 1]) {
                        best[pos] = best[pos - 1];
                        bestD[pos] = bestD[pos - 1];
                        --pos;
                    }
                    best[pos] = cnt;
                    bestD[pos] = d;
                }
            }
            if (bestN == 0) {
                L().Key("PROBE-A: R3 anchor=%s companion=%s: no pair within +-0x2000",
                        kR3Name[a], kR3Name[c]);
                continue;
            }
            char line[448] = {};
            unsigned n = 0;
            App(line, sizeof(line), n, "PROBE-A: R3 anchor=%s -> companion=%s (A=%d B=%d):",
                kR3Name[a], kR3Name[c], static_cast<int>(hits[a].size()),
                static_cast<int>(hits[c].size()));
            for (int k = 0; k < bestN; ++k) {
                const int d = bestD[k];
                const uintptr_t at = example[static_cast<size_t>(d + kSpan)];
                App(line, sizeof(line), n, " d%+d:%dx@%s", d, best[k],
                    mem::Hex(at).c_str());
            }
            L().Key("%s", line);
        }
    }
    r3Done_ = true;
    r3Wanted_ = false;
    L().Key("PROBE-A: R3 finished (one pass, coverage above)");
}

// ---------------------------------------------------------------------------
// World snapshot (docs/MSL_ADVANCE.md §6/§9): the real motion state is the part
// transform of a world object, not the pool node.  Every ~250 ms while something
// is flying we log
//   * the per-frame tick counter (proves the hook is live, once per frame),
//   * the live pool list L2 = S+0x98 (launch records),
//   * the world's objects whose part-0 position moved since the previous snapshot,
//   * full detail (object header + part transform + per-object sim cache) for the
//     fastest-mover and, on the first snapshot, for every object once.
// Every pointer is validated with mem::IsReadable before it is dereferenced.
// ---------------------------------------------------------------------------
void ProbeModel::WorldSnapshotTick(double now) {
    if (!worldHooked_ || worldSnaps_ >= kWorldSnaps) return;
    if ((now - lastWorldSnap_) < 0.25) return;
    lastWorldSnap_ = now;
    if (++worldAttempts_ > 800) return;  // ~200 s of attempts at most

    const uintptr_t world = reinterpret_cast<uintptr_t>(g_world);
    if (world == 0) return;
    world_ = world;

    uintptr_t objArr = 0;
    uint8_t count8 = 0;
    uintptr_t simBase = 0;
    if (!mem::Read<uintptr_t>(world + 0x697B8, objArr) ||
        !mem::Read<uint8_t>(world + 0xA87E4, count8) ||
        !mem::Read<uintptr_t>(world + 0xA8248, simBase)) {
        if (!worldUnreadableLogged_) {
            worldUnreadableLogged_ = true;
            L().Key("PROBE-A: world %s captured but its layout is not readable (world+0x697B8 / "
                    "+0xA87E4 / +0xA8248)",
                    mem::Hex(world).c_str());
        }
        return;
    }
    const int count = static_cast<int>(count8);
    if (count <= 0 || objArr == 0) return;

    // ---- live pool list L2 (S+0x98): launch records, logged every snapshot ----
    struct PoolRec {
        uintptr_t node;
        uint8_t state;
        int32_t t60;
        uint16_t cat;
        uintptr_t param;
        float pos[3];
        float dir[3];
    };
    PoolRec pool[4] = {};
    int poolCount = 0;
    int l2count = 0;
    const uintptr_t s = base_ + kRvaPoolSingleton;
    uintptr_t l2First = 0;
    if (mem::Read<uintptr_t>(s + 0x98, l2First)) {
        mem::Read<int32_t>(s + 0x108, l2count);
        uintptr_t node = (l2First == s + 0x98) ? 0 : l2First;
        int guard = 0;
        while (node != 0 && node != s + 0x98 && poolCount < 4 && guard++ < 32) {
            uint8_t nbuf[0x70] = {};
            if (!mem::ReadBytes(node, nbuf, sizeof(nbuf))) break;
            PoolRec& pr = pool[poolCount++];
            pr.node = node;
            pr.state = nbuf[0x6B];
            std::memcpy(&pr.t60, nbuf + 0x60, 4);
            std::memcpy(&pr.cat, nbuf + 0x20, 2);
            std::memcpy(&pr.param, nbuf + 0x28, 8);
            std::memcpy(pr.pos, nbuf + 0x34, 12);
            std::memcpy(pr.dir, nbuf + 0x40, 12);
            std::memcpy(&node, nbuf, 8);
        }
    }

    // ---- objects: part-0 transform and its delta vs the previous snapshot ----
    const int scanN = (count < kWorldObjects) ? count : kWorldObjects;
    const size_t bytes = static_cast<size_t>(scanN) * sizeof(uintptr_t);
    std::vector<uintptr_t> objs(static_cast<size_t>(scanN), 0);
    if (!mem::ReadBytes(objArr, objs.data(), bytes)) return;

    struct Mover {
        int idx;
        uintptr_t obj;
        float delta;  // m since the previous snapshot (<0 = first sighting)
        uintptr_t part;
        uint8_t team;
        uint32_t nparts;
        uint32_t flags;
        float pos[4];
        float dir[3];
        uint16_t alive;
        float sim[8];  // perObj+0x70..+0x8C
    };
    Mover movers[8] = {};
    int moverCount = 0;
    int moving = 0;

    for (int i = 0; i < scanN; ++i) {
        const uintptr_t obj = objs[static_cast<size_t>(i)];
        if (obj == 0) continue;
        uint8_t hdr[0x70] = {};
        if (!mem::IsReadable(obj + 0x100, sizeof(hdr))) continue;
        if (!mem::ReadBytes(obj + 0x100, hdr, sizeof(hdr))) continue;
        uintptr_t parts = 0;
        std::memcpy(&parts, hdr + 0x10, 8);  // obj+0x110
        if (parts == 0) continue;
        if (!mem::IsReadable(parts + 0x258, 0x28)) continue;
        uint8_t pbuf[0x28] = {};
        if (!mem::ReadBytes(parts + 0x258, pbuf, sizeof(pbuf))) continue;
        uint16_t alive = 0;
        mem::Read<uint16_t>(parts + 0x1F0, alive);
        float pos[4] = {};
        std::memcpy(pos, pbuf, 16);
        float dir[3] = {};
        std::memcpy(dir, pbuf + 0x10, 12);  // +0x268
        float delta = -1.f;
        if (prevObjValid_[i] && prevObjPtr_[i] == obj) {
            const float dx = pos[0] - prevObjPos_[i][0];
            const float dy = pos[1] - prevObjPos_[i][1];
            const float dz = pos[2] - prevObjPos_[i][2];
            delta = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        prevObjPtr_[i] = obj;
        prevObjValid_[i] = true;
        std::memcpy(prevObjPos_[i], pos, sizeof(pos));
        if (!(delta < 0.f || delta > 0.3f)) continue;  // static object: not interesting
        ++moving;
        if (moverCount >= 8) continue;
        Mover& m = movers[moverCount++];
        m.idx = i;
        m.obj = obj;
        m.delta = delta;
        m.part = parts;
        m.team = hdr[0x5D];  // obj+0x15D
        std::memcpy(&m.nparts, hdr + 0x18, 4);  // obj+0x118
        std::memcpy(&m.flags, hdr + 0x20, 4);  // obj+0x120
        std::memcpy(m.pos, pos, sizeof(pos));
        std::memcpy(m.dir, dir, sizeof(dir));
        m.alive = alive;
        const uintptr_t perObj = simBase + static_cast<uintptr_t>(i) * 0x27A0;
        if (perObj != 0 && mem::ReadBytes(perObj + 0x70, m.sim, sizeof(m.sim))) {
            // m.sim filled
        } else {
            std::memset(m.sim, 0, sizeof(m.sim));
        }
    }

    // Only consume a snapshot slot when something is actually flying/changing.
    if (poolCount == 0 && moving == 0) return;
    ++worldSnaps_;

    // Free anchors from the dispatcher decompile: engine frame counter and the
    // player's object index (so the interesting object can be spotted by name).
    uint32_t engineFrame = 0;
    mem::Read<uint32_t>(base_ + 0x33ED94, engineFrame);
    uint8_t playerIdx = 0;
    mem::Read<uint8_t>(base_ + 0x3409F2, playerIdx);

    L().Key("PROBE-A: WORLD t=%d/%d count=%d frame=%lld engineFrame=%u playerIdx=%u "
            "objArr=%s sim=%s poolL2=%d movers=%d",
            worldSnaps_, kWorldSnaps, count, static_cast<long long>(g_worldFrames),
            static_cast<unsigned>(engineFrame), static_cast<unsigned>(playerIdx),
            mem::Hex(objArr).c_str(), mem::Hex(simBase).c_str(), l2count, moving);
    if (poolCount > 0) {
        char line[400] = {};
        unsigned n = 0;
        App(line, sizeof(line), n, "PROBE-A:  poolL2:");
        for (int k = 0; k < poolCount; ++k) {
            App(line, sizeof(line), n,
                " n=%s st=%u t=%d cat=%u param=%s pos=(%.1f %.1f %.1f) dir=(%.2f %.2f %.2f)",
                mem::Hex(pool[k].node).c_str(), static_cast<unsigned>(pool[k].state), pool[k].t60,
                static_cast<unsigned>(pool[k].cat), mem::Hex(pool[k].param).c_str(),
                static_cast<double>(pool[k].pos[0]), static_cast<double>(pool[k].pos[1]),
                static_cast<double>(pool[k].pos[2]), static_cast<double>(pool[k].dir[0]),
                static_cast<double>(pool[k].dir[1]), static_cast<double>(pool[k].dir[2]));
        }
        L().Key("%s", line);
    }
    if (moverCount > 0) {
        char line[400] = {};
        unsigned n = 0;
        App(line, sizeof(line), n, "PROBE-A:  movers:");
        for (int k = 0; k < moverCount; ++k) {
            App(line, sizeof(line), n, " i%d/t%u/d%.1f", movers[k].idx,
                static_cast<unsigned>(movers[k].team), static_cast<double>(movers[k].delta));
        }
        L().Key("%s", line);
        // Full detail of the fastest mover every snapshot, and of every mover on
        // the first snapshot (that is the object-list census).
        int top = 0;
        for (int k = 1; k < moverCount; ++k) {
            if (movers[k].delta > movers[top].delta) top = k;
        }
        const int last = (worldSnaps_ == 1) ? moverCount : top + 1;
        for (int k = (worldSnaps_ == 1 ? 0 : top); k < last; ++k) {
            const Mover& m = movers[k];
            L().Key("PROBE-A:   obj i=%d ptr=%s team=%u flags=0x%X nparts=%u parts=%s "
                    "p0pos=(%.2f %.2f %.2f %.3f) p0dir=(%.4f %.4f %.4f) alive=0x%X "
                    "sim70..8C=[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] d=%.2f",
                    m.idx, mem::Hex(m.obj).c_str(), static_cast<unsigned>(m.team), m.flags,
                    static_cast<unsigned>(m.nparts), mem::Hex(m.part).c_str(),
                    static_cast<double>(m.pos[0]), static_cast<double>(m.pos[1]),
                    static_cast<double>(m.pos[2]), static_cast<double>(m.pos[3]),
                    static_cast<double>(m.dir[0]), static_cast<double>(m.dir[1]),
                    static_cast<double>(m.dir[2]), static_cast<unsigned>(m.alive),
                    static_cast<double>(m.sim[0]), static_cast<double>(m.sim[1]),
                    static_cast<double>(m.sim[2]), static_cast<double>(m.sim[3]),
                    static_cast<double>(m.sim[4]), static_cast<double>(m.sim[5]),
                    static_cast<double>(m.sim[6]), static_cast<double>(m.sim[7]),
                    static_cast<double>(m.delta));
        }
    }
}

// ---------------------------------------------------------------------------
// Heartbeat + state machine
// ---------------------------------------------------------------------------
void ProbeModel::Heartbeat(double now) {
    if ((now - lastHeartbeat_) < 5.0) return;
    lastHeartbeat_ = now;
    L().Key("PROBE-A: heartbeat t=%.0fs allocHook=%d popHook=%d allocCalls=%lld popCalls=%lld "
            "allocs=%d pops=%d nodes=%d sample=%d/%d singleton=%d wmVerified=%d wmScans=%d "
            "globalWalks=%d overflow=%ld/%ld",
            now - start_, allocHooked_ ? 1 : 0, popHooked_ ? 1 : 0,
            static_cast<long long>(g_allocCalls), static_cast<long long>(g_popCalls),
            allocEvents_, popEvents_, sampleNodeCount_, sampleRound_, kSampleRounds,
            singletonSeen_ ? 1 : 0, wmVerified_, wmScans_, globalWalks_,
            static_cast<long>(g_allocOverflow), static_cast<long>(g_popOverflow));
}

void ProbeModel::Tick() {
    if (!active_) return;
    const double now = Now();

    // 1) Deferred hook installation (SteamStub decrypts .text late).
    if ((!allocHooked_ && !allocGaveUp_) || (!popHooked_ && !popGaveUp_)) TickInstall(now);

    // 2) Publish the events the detours captured.
    DrainCaptures();

    // 3) Per-frame observations.
    NodeSampleTick(now);
    GlobalListTick(now);
    WorldSnapshotTick(now);

    // The flat .wpb record layout was proven from a node+0x28 dump: the two heavy
    // discovery passes are no longer needed for this run.
    if (flatRecordSeen_) {
        wmScanWanted_ = false;
        r3Wanted_ = false;
    }

    // 4) If the allocator hook never saw anything (e.g. it was refused), still run
    //    the WeaponModel scan on a timer so a run is not wasted.
    if (!singletonSeen_ && !wmScanWanted_ && !wmScanGaveUp_ && wmScans_ == 0 &&
        (now - start_) > 25.0) {
        wmScanWanted_ = true;
        nextWmScan_ = now;
        r3At_ = now + 40.0;
        L().Key("PROBE-A: no mission-singleton allocation seen by 25 s; running the WeaponModel "
                "scan on a timer instead");
    }

    // 5) Heavy one-shot passes.  Never while the 12-sample node window is running.
    if (wmScanWanted_ && !wmScanGaveUp_ && now >= nextWmScan_) {
        WeaponModelScan();
        if (wmScanWanted_ && wmScans_ >= 5) wmScanGaveUp_ = true;
    }
    if (r3Wanted_ && !r3Done_ && now >= r3At_ && !wmScanWanted_ &&
        (sampleNodeCount_ == 0 || sampleRound_ >= kSampleRounds || now > r3At_ + 60.0)) {
        R3ScanAndTally();
    }

    // 6) Heartbeat.
    Heartbeat(now);
}

} // namespace acz
