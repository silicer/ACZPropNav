// ACZPropNav - "Run B" hardware read-watch probe implementation.
//
// Mechanism (offline-proven by tools/hwwatchtest, added to ./build.sh test):
//   * a 4-byte read/write watchpoint in Dr0..Dr3 + Dr7 is delivered on this
//     Windows build as EXCEPTION_SINGLE_STEP with the slot bit in Dr6, and the
//     saved Rip is the faulting instruction;
//   * a data breakpoint is a FAULT, so setting RF and continuing re-faults
//     forever (Windows then kills the process).  Correct scheme: record the hit,
//     CLEAR Dr0..Dr3/Dr6/Dr7 in the returning context and continue; the probe
//     re-arms from its tick a bit later.
//
// Everything the VEH does is fixed-size and lock-free (Interlocked + local
// registers): no allocation, no logging, no mem:: calls (mem:: has its own VEH and
// TLS setjmp state - calling it from an exception handler is not safe).
#include "core/probe_watch.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

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
// RVAs for acz.exe 4,046,008 B / SHA-256 3802B0DC... (docs/MSL_FIRE_OBJECT.md,
// docs/WEAPON_MODEL_LAYOUT.md).
// ---------------------------------------------------------------------------
constexpr uint32_t kRvaMcGlobal = 0x3409F8;        // *(void**) -> mission control
constexpr uint32_t kRvaPlayerIdx = 0x3409F2;       // u8 player object index
constexpr uint32_t kRvaPoolSingleton = 0x3219A0;   // S (fallback: MC == *(S+0x428))
constexpr uint32_t kRvaAllocate = 0x201460;        // pool Allocate(pool,size,align)
constexpr uint32_t kRvaWeaponModelAllocRet = 0x0C767D;  // return addr of the WeaponModel alloc
constexpr uint32_t kRvaWeaponModelVtable = 0x2AE790;    // WeaponModel vftable

const uint8_t kAllocPrologue[17] = {0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x41, 0x55,
                                    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x10};

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

// ---------------------------------------------------------------------------
// Hardware-watchpoint hit ring (written by the VEH on any game thread).
// ---------------------------------------------------------------------------
constexpr int kHitCap = 512;
struct RawHit {
    volatile LONG ready = 0;
    volatile LONG slot = -1;
    volatile LONG64 rip = 0;
    volatile LONG64 rax = 0;
    volatile LONG64 rcx = 0;
    volatile LONG64 rdx = 0;
    volatile LONG64 r8 = 0;
    volatile LONG64 r9 = 0;
    volatile LONG64 rsp = 0;
    volatile LONG dr6 = 0;
    volatile LONG tid = 0;
};
RawHit g_hits[kHitCap];
volatile LONG g_hitReserved = 0;
volatile LONG64 g_hitOverflow = 0;
volatile LONG64 g_totalHits = 0;
volatile LONG g_watchArmed = 0;

// One-shot data-breakpoint handler: record, then clear every debug register in the
// returning context.  No I/O, no allocation, no locks, no mem:: calls.
LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    if (g_watchArmed == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = ep->ContextRecord;
    const ULONG64 dr6 = c->Dr6;
    int slot = -1;
    for (int i = 0; i < 4; ++i) {
        if ((dr6 & (1ull << i)) != 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return EXCEPTION_CONTINUE_SEARCH;  // not one of ours
    const LONG idx = ::InterlockedIncrement(&g_hitReserved) - 1;
    if (idx >= 0 && idx < kHitCap) {
        RawHit& h = g_hits[idx];
        h.slot = slot;
        h.rip = static_cast<LONG64>(c->Rip);
        h.rax = static_cast<LONG64>(c->Rax);
        h.rcx = static_cast<LONG64>(c->Rcx);
        h.rdx = static_cast<LONG64>(c->Rdx);
        h.r8 = static_cast<LONG64>(c->R8);
        h.r9 = static_cast<LONG64>(c->R9);
        h.rsp = static_cast<LONG64>(c->Rsp);
        h.dr6 = static_cast<LONG>(dr6);
        h.tid = static_cast<LONG>(::GetCurrentThreadId());
        ::InterlockedExchange(&h.ready, 1);
    } else {
        ::InterlockedIncrement64(&g_hitOverflow);
    }
    ::InterlockedIncrement64(&g_totalHits);
    c->Dr0 = 0;
    c->Dr1 = 0;
    c->Dr2 = 0;
    c->Dr3 = 0;
    c->Dr6 = 0;
    c->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---------------------------------------------------------------------------
// WeaponModel allocation capture: our own copy of the Run A allocator hook.  It is
// only installed when the Run A probe is not running (both must never patch the
// same prologue), and it only tracks the WeaponModel registration caller.
// ---------------------------------------------------------------------------
uintptr_t g_exeBase = 0;
PVOID volatile g_allocTramp = nullptr;
struct WmAlloc {
    volatile LONG ready = 0;
    volatile LONG64 ptr = 0;
    volatile LONG64 size = 0;
};
constexpr int kWmAllocCap = 64;
WmAlloc g_wmAlloc[kWmAllocCap];
volatile LONG g_wmReserved = 0;
volatile LONG g_wmOverflow = 0;
volatile LONG64 g_allocCalls = 0;

void* WaitTramp(PVOID volatile* slot) {
    void* t = *slot;
    for (int i = 0; i < 100 && t == nullptr; ++i) ::SwitchToThread();
    while (t == nullptr) {
        ::SwitchToThread();
        t = *slot;
    }
    return t;
}

typedef void* (*AllocFn)(void*, uint32_t, uint32_t, void*);
__attribute__((noinline)) void* WatchAllocDetour(void* pool, uint32_t size, uint32_t align,
                                                void* a4) {
    ::InterlockedIncrement64(&g_allocCalls);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    AllocFn f = reinterpret_cast<AllocFn>(WaitTramp(&g_allocTramp));
    void* r = f(pool, size, align, a4);
    const uintptr_t base = g_exeBase;
    const uintptr_t rva = (caller >= base) ? (caller - base) : ~static_cast<uintptr_t>(0);
    if (rva == kRvaWeaponModelAllocRet && r != nullptr) {
        const LONG slot = ::InterlockedIncrement(&g_wmReserved) - 1;
        if (slot < kWmAllocCap) {
            WmAlloc& w = g_wmAlloc[slot];
            w.ptr = static_cast<LONG64>(reinterpret_cast<uintptr_t>(r));
            w.size = static_cast<LONG64>(size);
            ::InterlockedExchange(&w.ready, 1);
        } else {
            ::InterlockedIncrement(&g_wmOverflow);
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Thread enumeration / arming helpers.
// ---------------------------------------------------------------------------
int EnumerateOtherThreads(DWORD* tids, int cap) {
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int n = 0;
    const DWORD selfPid = ::GetCurrentProcessId();
    const DWORD selfTid = ::GetCurrentThreadId();
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != selfPid) continue;
            if (te.th32ThreadID == selfTid) continue;  // never arm our own tick thread
            if (n < cap) tids[n++] = te.th32ThreadID;
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return n;
}

// Suspend -> set Dr0..3/Dr7 -> resume.  No allocation/logging while suspended.
bool SetThreadWatch(uint32_t tid, const uintptr_t addrs[4], int nAddrs, bool disarm) {
    const HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                  FALSE, tid);
    if (h == nullptr) return false;
    bool ok = false;
    if (::SuspendThread(h) != static_cast<DWORD>(-1)) {
        alignas(16) CONTEXT ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (::GetThreadContext(h, &ctx)) {
            if (disarm) {
                ctx.Dr0 = 0;
                ctx.Dr1 = 0;
                ctx.Dr2 = 0;
                ctx.Dr3 = 0;
                ctx.Dr6 = 0;
                ctx.Dr7 = 0;
            } else {
                ULONG64 dr[4] = {0, 0, 0, 0};
                ULONG64 dr7 = 0;
                for (int i = 0; i < nAddrs && i < 4; ++i) {
                    dr[i] = static_cast<ULONG64>(addrs[i]);
                    dr7 |= 1ull << (2 * i);                 // L_i: local enable
                    dr7 |= 3ull << (16 + 4 * i);            // R/W = 11 (read or write)
                    dr7 |= 3ull << (18 + 4 * i);            // LEN = 11 (4 bytes)
                }
                ctx.Dr0 = dr[0];
                ctx.Dr1 = dr[1];
                ctx.Dr2 = dr[2];
                ctx.Dr3 = dr[3];
                ctx.Dr6 = 0;
                ctx.Dr7 = dr7;
            }
            ok = ::SetThreadContext(h, &ctx) != FALSE;
        }
        ::ResumeThread(h);
    }
    ::CloseHandle(h);
    return ok;
}

bool NearF(float a, float b) {
    float tol = std::fabs(b) * 0.01f;
    if (tol < 0.001f) tol = 0.001f;
    return std::fabs(a - b) <= tol;
}

// Our own module: its static tables contain the vtable RVAs, so scans skip it.
void OwnAnchorFn() {}
bool InsideOwnModule(uintptr_t addr) {
    static uintptr_t ownBase = 0;
    static size_t ownSize = 0;
    static bool init = false;
    if (!init) {
        init = true;
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(0x00000004u | 0x00000002u,
                                 reinterpret_cast<LPCWSTR>(&OwnAnchorFn), &self) &&
            self) {
            MODULEINFO mi{};
            if (::GetModuleInformation(::GetCurrentProcess(), self, &mi, sizeof(mi))) {
                ownBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                ownSize = mi.SizeOfImage;
            }
        }
    }
    return ownBase != 0 && addr >= ownBase && addr < ownBase + ownSize;
}

// Weapon-controller vtables (docs/RUNTIME_PARAMS.md §5.1, docs/RTTI.md).
struct Vt {
    uint32_t rva;
    const char* name;
};
const Vt kVtables[7] = {
    {0x2AF470u, "WeaponExFireCtrl"}, {0x2AF740u, "WeaponExQaamCtrl"},
    {0x2AFAE8u, "WeaponExSaamCtrl"}, {0x2AF560u, "WeaponExXlaaCtrl"},
    {0x2AF308u, "WeaponExXmaaCtrl"}, {0x2AF980u, "WeaponExXagmCtrl"},
    {0x2AE790u, "WeaponModel"},
};

// Markers for the WeaponModel payload dump: the .wpb values a mis/misf model can hold.
struct MarkF {
    float v;
    const char* tag;
};
const MarkF kMarks[] = {
    {1600.f, "SCOPE1600"}, {3000.f, "RANGE3000"}, {2000.f, "V2000"}, {400.f, "V400"},
    {40.f, "LEAD40"},      {80.f, "LEAD80"},      {28.f, "LEAD28"},  {10.f, "V10"},
    {90.f, "ANG90"},
};

void MarkPayloadRow(const uint8_t* row, char* out, unsigned cap, unsigned& n) {
    for (int i = 0; i < 4; ++i) {
        float v = 0.f;
        std::memcpy(&v, row + i * 4, 4);
        if (!std::isfinite(v)) continue;
        for (const MarkF& m : kMarks) {
            if (NearF(v, m.v)) {
                App(out, cap, n, " f+%02X=%s", i * 4, m.tag);
                break;
            }
        }
    }
    for (int off = 0; off + 3 < 16; off += 4) {
        uint32_t w = 0;
        std::memcpy(&w, row + off, 4);
        if (w == 1000u) App(out, cap, n, " u32+%02X=1000", off);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Start / Shutdown
// ---------------------------------------------------------------------------
void ProbeWatch::Start(uintptr_t exeBase, int seconds, bool installModelHook) {
    if (active_) return;
    active_ = true;
    base_ = exeBase;
    seconds_ = (seconds > 0) ? seconds : 25;
    start_ = Now();
    hookWanted_ = installModelHook;
    g_exeBase = exeBase;

    mem::ModuleInfo mi;
    if (mem::MainModule(mi)) moduleSize_ = mi.size;

    ::InterlockedExchange64(&g_totalHits, 0);
    ::InterlockedExchange(&g_hitReserved, 0);
    ::InterlockedExchange64(&g_hitOverflow, 0);
    ::InterlockedExchange(&g_watchArmed, 0);
    ::InterlockedExchangePointer(&g_allocTramp, nullptr);
    g_wmReserved = 0;
    g_wmOverflow = 0;
    g_allocCalls = 0;

    vehHandle_ = ::AddVectoredExceptionHandler(1 /* first handler */, &WatchVeh);

    L().Key("================ PROBE-B (Run B, hardware read-watch) ================");
    L().Key("PROBE-B: exe base %s; watching P+0x2C (lead), P+0x48 (type/attack/"
            "startAng), P+0x14 (range), P+0x44 (speed) with Dr0..Dr3 on every game "
            "thread, one-shot VEH, re-arm every 200 ms",
            mem::Hex(exeBase).c_str());
    L().Key("PROBE-B: window %d s; %s; no game memory is written",
            seconds_, hookWanted_ ? "WeaponModel payload capture enabled (own allocator hook)"
                                  : "Run A owns the allocator hook, payload capture disabled");
    if (vehHandle_ == nullptr) {
        L().Key("PROBE-B: !! AddVectoredExceptionHandler failed - Run B cannot arm watchpoints");
    }
    L().Key("PROBE-B: waiting for mission control / the player's weapon record (retry every "
            "200 ms, give up at 180 s)");
    L().Flush();
}

void ProbeWatch::Shutdown() {
    if (!active_ && vehHandle_ == nullptr) return;
    if (armed_) {
        ArmAllThreads(true);
        armed_ = false;
    }
    ::InterlockedExchange(&g_watchArmed, 0);
    if (vehHandle_ != nullptr) {
        ::RemoveVectoredExceptionHandler(vehHandle_);
        vehHandle_ = nullptr;
    }
    if (hookInstalled_ && hookTarget_ != nullptr) {
        if (!hook::Remove(hookTarget_, hookResult_)) {
            L().Key("PROBE-B: cannot restore the alloc prologue at %s",
                    mem::Hex(reinterpret_cast<uintptr_t>(hookTarget_)).c_str());
        }
        hookTarget_ = nullptr;
        hookInstalled_ = false;
    }
    ::InterlockedExchangePointer(&g_allocTramp, nullptr);
    active_ = false;
}

void ProbeWatch::Finish(const char* reason) {
    if (finished_) return;
    finished_ = true;
    if (armed_) {
        ArmAllThreads(true);
        armed_ = false;
    }
    ::InterlockedExchange(&g_watchArmed, 0);
    DrainHits();
    L().Key("PROBE-B: FINISHED (%s): armed %.1f s, hits=%lld distinctRips=%d sweeps=%d "
            "threads=%d armFailures=%d overflow=%lld allocEvents=%d payloadDumps=%d",
            reason, armedAt_ > 0.0 ? (Now() - armedAt_) : 0.0,
            static_cast<long long>(g_totalHits), ripCount_, armSweeps_, threadsArmed_,
            armFailures_, static_cast<long long>(g_hitOverflow), allocEvents_, wmDumpedCount_);
    for (int i = 0; i < ripCount_; ++i) {
        const RipEntry& r = rips_[i];
        const bool inImage = (r.rip >= base_) && (moduleSize_ != 0) && (r.rip < base_ + moduleSize_);
        const std::string rs =
            inImage ? ("exe+0x" + mem::Hex(static_cast<uintptr_t>(r.rip - base_)))
                    : mem::Hex(static_cast<uintptr_t>(r.rip));
        L().Key("PROBE-B: RIPSUM slot=%d rip=%s count=%d tid=%lu", r.slot, rs.c_str(), r.count,
                r.tid);
    }
    L().Flush();
    active_ = false;
}

// ---------------------------------------------------------------------------
// Chain resolution: MC -> objArray -> playerObj -> part -> fire(WC) -> P
// Every pointer is validated before use; on failure the tick retries.
// ---------------------------------------------------------------------------
bool ProbeWatch::ResolveChain() {
    // MC: global first, then the pool singleton's +0x428 back-pointer.
    uintptr_t mc = 0;
    if (!mem::Read<uintptr_t>(base_ + kRvaMcGlobal, mc) || mc == 0) mc = 0;
    uintptr_t mcFromS = 0;
    mem::Read<uintptr_t>(base_ + kRvaPoolSingleton + 0x428, mcFromS);
    if (mc == 0) mc = mcFromS;
    if (mc == 0) return false;
    if (!mem::IsReadable(mc + 0x697B8, 8) || !mem::IsReadable(mc + 0xA87E4, 1) ||
        !mem::IsReadable(mc + 0xA8248, 8)) {
        return false;
    }
    uintptr_t objArray = 0, simBase = 0;
    uint8_t count8 = 0;
    if (!mem::Read<uintptr_t>(mc + 0x697B8, objArray) || objArray == 0) return false;
    if (!mem::Read<uint64_t>(mc + 0xA8248, simBase)) return false;
    if (!mem::Read<uint8_t>(mc + 0xA87E4, count8)) return false;
    const int count = static_cast<int>(count8);
    if (count < 1 || count > 64) return false;

    int playerIdx = -1;
    uint8_t pidx = 0xFF;
    if (mem::Read<uint8_t>(base_ + kRvaPlayerIdx, pidx) && pidx < count) {
        playerIdx = static_cast<int>(pidx);
    }

    const int scanN = std::min(count, 32);
    std::vector<uintptr_t> objs(static_cast<size_t>(scanN), 0);
    if (!mem::ReadBytes(objArray, objs.data(), static_cast<size_t>(scanN) * sizeof(uintptr_t))) {
        return false;
    }

    // Candidate order: the player's index first (if valid), then 0..scanN-1.
    int order[33] = {};
    int orderN = 0;
    if (playerIdx >= 0 && playerIdx < scanN) order[orderN++] = playerIdx;
    for (int i = 0; i < scanN && orderN < 33; ++i) {
        if (i != playerIdx) order[orderN++] = i;
    }

    for (int k = 0; k < orderN; ++k) {
        const int i = order[k];
        const uintptr_t obj = objs[static_cast<size_t>(i)];
        if (obj == 0) continue;
        uintptr_t parts = 0;
        if (!mem::Read<uintptr_t>(obj + 0x110, parts) || parts == 0) continue;
        if (!mem::IsReadable(parts + 0x1B8, 8)) continue;
        uintptr_t fire = 0;
        if (!mem::Read<uintptr_t>(parts + 0x1B8, fire) || fire == 0) continue;
        if (!mem::IsReadable(fire + 0x1FB8, 8)) continue;
        uintptr_t p = 0;
        if (!mem::Read<uintptr_t>(fire + 0x1FB8, p) || p < 0x10000) continue;
        if (!mem::IsReadable(p - 0x10, 0x60)) continue;
        uint8_t raw[0x60] = {};
        if (!mem::ReadBytes(p - 0x10, raw, sizeof(raw))) continue;

        uint32_t dataOff = 0;
        float scope = 0.f, range = 0.f, timeStep = 0.f, lead = 0.f;
        uint32_t speed = 0;
        uint8_t type = 0, attack = 0, startAng = 0;
        std::memcpy(&dataOff, raw + 0x00, 4);   // P-0x10
        std::memcpy(&scope, raw + 0x20, 4);     // P+0x10
        std::memcpy(&range, raw + 0x24, 4);     // P+0x14
        std::memcpy(&timeStep, raw + 0x34, 4);  // P+0x24
        std::memcpy(&lead, raw + 0x3C, 4);      // P+0x2C
        std::memcpy(&speed, raw + 0x54, 4);     // P+0x44
        type = raw[0x58];                        // P+0x48
        attack = raw[0x59];                      // P+0x49
        startAng = raw[0x5A];                    // P+0x4A

        const bool valid = dataOff == 0x10u && scope >= 800.f && scope <= 4000.f &&
                           range >= 500.f && range <= 5000.f && lead >= 0.f && lead <= 400.f &&
                           speed >= 100u && speed <= 3000u;
        if (!valid) continue;

        P_ = p;
        mc_ = mc;
        mcFromS_ = mcFromS;
        objArray_ = objArray;
        simBase_ = simBase;
        objCount_ = count;
        playerIdx_ = playerIdx;
        usedIdx_ = i;
        playerObj_ = obj;
        part_ = parts;
        fire_ = fire;
        std::memcpy(pBytes_, raw, sizeof(pBytes_));
        pDataOff_ = dataOff;
        pScope_ = scope;
        pRange_ = range;
        pTimeStep_ = timeStep;
        pLead_ = lead;
        pSpeed_ = speed;
        pType_ = type;
        pAttack_ = attack;
        pStartAng_ = startAng;
        return true;
    }
    return false;
}

void ProbeWatch::LogChain() {
    L().Key("PROBE-B: CHAIN MC=%s (from global 0x%X; *(S+0x428)=%s) count=%d objArray=%s "
            "simBase=%s playerIdx=%d usedIdx=%d obj=%s part=%s fire=%s P=%s",
            mem::Hex(mc_).c_str(), kRvaMcGlobal, mem::Hex(mcFromS_).c_str(), objCount_,
            mem::Hex(objArray_).c_str(), mem::Hex(simBase_).c_str(), playerIdx_, usedIdx_,
            mem::Hex(playerObj_).c_str(), mem::Hex(part_).c_str(), mem::Hex(fire_).c_str(),
            mem::Hex(P_).c_str());
}

void ProbeWatch::LogPBlock() {
    L().Key("PROBE-B: P=%s data_off(P-0x10)=0x%X scope(P+0x10)=%.4g range(P+0x14)=%.4g "
            "timeStep(P+0x24)=%.4g lead(P+0x2C)=%.4g speed(P+0x44)=%u type(P+0x48)=%u "
            "attack(P+0x49)=%u startAng(P+0x4A)=%u  [%s]",
            mem::Hex(P_).c_str(), pDataOff_, static_cast<double>(pScope_),
            static_cast<double>(pRange_), static_cast<double>(pTimeStep_),
            static_cast<double>(pLead_), pSpeed_, static_cast<unsigned>(pType_),
            static_cast<unsigned>(pAttack_), static_cast<unsigned>(pStartAng_),
            (pRange_ > 2500.f && pLead_ > 30.f && pStartAng_ == 90) ? "mis"
                                                                   : "variant/other");
    L().Key("PROBE-B: P raw 0x60 bytes (from P-0x10, 16-byte rows + floats):");
    for (uint32_t off = 0; off < 0x60; off += 16) {
        uint8_t row[16] = {};
        std::memcpy(row, pBytes_ + off, sizeof(row));
        uint32_t dw[4] = {};
        float f[4] = {};
        std::memcpy(dw, row, sizeof(dw));
        std::memcpy(f, row, sizeof(f));
        L().Key("PROBE-B:   P%+03d: %08X %08X %08X %08X | %11.5g %11.5g %11.5g %11.5g",
                static_cast<int>(off) - 0x10, dw[0], dw[1], dw[2], dw[3],
                static_cast<double>(f[0]), static_cast<double>(f[1]),
                static_cast<double>(f[2]), static_cast<double>(f[3]));
    }
}

void ProbeWatch::StartArming(double now) {
    if (vehHandle_ == nullptr) {
        Finish("no VEH");
        return;
    }
    ::InterlockedExchange(&g_watchArmed, 1);
    armed_ = true;
    armedAt_ = now;
    lastArmSweep_ = now;
    ArmAllThreads(false);
    L().Key("PROBE-B: ARMED: 4 watchpoints on P at %s (+0x2C, +0x48, +0x14, +0x44) over %d "
            "thread(s); window %d s.  FIRE A PLAIN MSL NOW and keep flying.",
            mem::Hex(P_).c_str(), threadsArmed_, seconds_);
    L().Flush();
}

void ProbeWatch::ArmAllThreads(bool disarm) {
    if (!disarm && P_ == 0) return;
    const uintptr_t addrs[4] = {P_ + 0x2C, P_ + 0x48, P_ + 0x14, P_ + 0x44};
    DWORD tids[256] = {};
    const int n = EnumerateOtherThreads(tids, 256);
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (SetThreadWatch(tids[i], addrs, 4, disarm)) {
            ++ok;
        } else if (!disarm) {
            ++armFailures_;
        }
    }
    threadsArmed_ = ok;
}

// ---------------------------------------------------------------------------
// Hit draining / aggregation (runs on the mod thread; the ring is lock-free).
// ---------------------------------------------------------------------------
void ProbeWatch::DrainHits() {
    const LONG reserved = g_hitReserved;
    for (LONG i = 0; i < reserved && i < kHitCap; ++i) {
        if (g_hits[i].ready == 0 || hitSeen_[i]) continue;
        hitSeen_[i] = true;
        ++totalHitsSeen_;
        const uint64_t rip = static_cast<uint64_t>(g_hits[i].rip);
        const int slot = static_cast<int>(g_hits[i].slot);
        const uint32_t tid = static_cast<uint32_t>(g_hits[i].tid);
        const bool inImage = (rip >= base_) && (moduleSize_ != 0) && (rip < base_ + moduleSize_);
        const std::string ripStr =
            inImage ? ("exe+0x" + mem::Hex(static_cast<uintptr_t>(rip - base_)))
                    : mem::Hex(static_cast<uintptr_t>(rip));

        // Raw copy of every hit, even after aggregation (cheap, capped).
        if (rawHitsLogged_ < 420) {
            ++rawHitsLogged_;
            L().Key("PROBE-B: RAW hit #%d slot=%d dr6=0x%X rip=%s rax=%s rcx=%s rdx=%s r8=%s "
                    "r9=%s rsp=%s tid=%lu",
                    rawHitsLogged_, slot, static_cast<unsigned>(g_hits[i].dr6), ripStr.c_str(),
                    mem::Hex(static_cast<uintptr_t>(g_hits[i].rax)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(g_hits[i].rcx)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(g_hits[i].rdx)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(g_hits[i].r8)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(g_hits[i].r9)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(g_hits[i].rsp)).c_str(),
                    static_cast<unsigned long>(tid));
        }

        RipEntry* e = nullptr;
        for (int k = 0; k < ripCount_; ++k) {
            if (rips_[k].rip == rip) {
                e = &rips_[k];
                break;
            }
        }
        if (e == nullptr) {
            if (ripCount_ >= kMaxRips) continue;
            e = &rips_[ripCount_++];
            e->rip = rip;
            e->slot = slot;
            e->count = 0;
            e->rax = static_cast<uint64_t>(g_hits[i].rax);
            e->rcx = static_cast<uint64_t>(g_hits[i].rcx);
            e->rdx = static_cast<uint64_t>(g_hits[i].rdx);
            e->r8 = static_cast<uint64_t>(g_hits[i].r8);
            e->r9 = static_cast<uint64_t>(g_hits[i].r9);
            e->rsp = static_cast<uint64_t>(g_hits[i].rsp);
            e->tid = tid;
            // Return-address hints from the faulting stack (guarded reads; the stack
            // may have moved on, so these are hints, logged once per Rip).
            uintptr_t ret0 = 0, ret1 = 0;
            const uintptr_t rsp = static_cast<uintptr_t>(g_hits[i].rsp);
            mem::Read<uintptr_t>(rsp, ret0);
            mem::Read<uintptr_t>(rsp + 8, ret1);
            const bool r0in = (ret0 >= base_) && (moduleSize_ != 0) && (ret0 < base_ + moduleSize_);
            const bool r1in = (ret1 >= base_) && (moduleSize_ != 0) && (ret1 < base_ + moduleSize_);
            const std::string c0 = r0in ? ("exe+0x" + mem::Hex(ret0 - base_)) : mem::Hex(ret0);
            const std::string c1 = r1in ? ("exe+0x" + mem::Hex(ret1 - base_)) : mem::Hex(ret1);
            L().Key("PROBE-B: HIT slot=%d rip=%s count=1 first rax=%s rcx=%s rdx=%s r8=%s r9=%s "
                    "rsp=%s tid=%lu caller0=%s caller1=%s",
                    slot, ripStr.c_str(),
                    mem::Hex(static_cast<uintptr_t>(e->rax)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(e->rcx)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(e->rdx)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(e->r8)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(e->r9)).c_str(),
                    mem::Hex(static_cast<uintptr_t>(e->rsp)).c_str(),
                    static_cast<unsigned long>(tid), c0.c_str(), c1.c_str());
        }
        ++e->count;
    }
    const long long overflow = g_hitOverflow;
    if (overflow > 0 && rawHitsLogged_ < 420) {
        L().Key("PROBE-B: hit ring overflow: %lld hit(s) not recorded (cap %d)", overflow,
                kHitCap);
        rawHitsLogged_ = 420;  // log the overflow note once
    }
}

// ---------------------------------------------------------------------------
// MOV snapshots: which world object/part actually moves (the flying missile).
// ---------------------------------------------------------------------------
void ProbeWatch::MoveSnapshotTick(double now) {
    if (!armed_ || objArray_ == 0 || objCount_ <= 0) return;
    if ((now - lastMoveSnap_) < 0.25) return;
    lastMoveSnap_ = now;
    ++moveSnaps_;
    const int n = std::min(objCount_, kMaxObj);
    uintptr_t objs[kMaxObj] = {};
    if (!mem::ReadBytes(objArray_, objs, static_cast<size_t>(n) * sizeof(uintptr_t))) return;

    struct Mover {
        uintptr_t obj = 0;
        int idx = 0;
        float delta = 0.f;
        float d[3] = {};
        float pos[4] = {};
        float head[3] = {};
    };
    Mover all[kMaxObj];
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t obj = objs[i];
        if (obj == 0) {
            prev_[i].valid = false;
            continue;
        }
        uintptr_t parts = 0;
        if (!mem::Read<uintptr_t>(obj + 0x110, parts) || parts == 0 ||
            !mem::IsReadable(parts + 0x258, 0x28)) {
            prev_[i].valid = false;
            continue;
        }
        uint8_t buf[0x28] = {};
        if (!mem::ReadBytes(parts + 0x258, buf, sizeof(buf))) {
            prev_[i].valid = false;
            continue;
        }
        float pos[4] = {};
        float head[3] = {};
        std::memcpy(pos, buf, 16);
        std::memcpy(head, buf + 0x10, 12);
        float d[3] = {};
        bool haveDelta = false;
        if (prev_[i].valid && prev_[i].obj == obj) {
            d[0] = pos[0] - prev_[i].pos[0];
            d[1] = pos[1] - prev_[i].pos[1];
            d[2] = pos[2] - prev_[i].pos[2];
            haveDelta = true;
        }
        prev_[i].obj = obj;
        std::memcpy(prev_[i].pos, pos, sizeof(pos));
        prev_[i].valid = true;
        if (!haveDelta) continue;  // first sighting: no delta yet
        Mover& m = all[count++];
        m.obj = obj;
        m.idx = i;
        m.delta = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        std::memcpy(m.d, d, sizeof(d));
        std::memcpy(m.pos, pos, sizeof(pos));
        std::memcpy(m.head, head, sizeof(head));
    }
    if (count <= 0) return;
    std::sort(all, all + count, [](const Mover& a, const Mover& b) { return a.delta > b.delta; });
    const int topN = std::min(count, 3);

    const Mover& m = all[0];
    uint8_t hdr[0x70] = {};
    uint32_t team = 0, flags = 0;
    if (mem::IsReadable(m.obj + 0x100, sizeof(hdr)) &&
        mem::ReadBytes(m.obj + 0x100, hdr, sizeof(hdr))) {
        team = hdr[0x5D];                    // obj+0x15D
        std::memcpy(&flags, hdr + 0x20, 4);  // obj+0x120
    }
    char tails[80] = {};
    unsigned tn = 0;
    for (int k = 1; k < topN; ++k) {
        App(tails, sizeof(tails), tn, " %d/%.1f", all[k].idx,
            static_cast<double>(all[k].delta));
    }
    uint32_t engineFrame = 0;
    mem::Read<uint32_t>(base_ + 0x33ED94, engineFrame);
    L().Key("PROBE-B: MOV t=%d obj=%d team=%u flags=0x%X pos=(%.1f %.1f %.1f) "
            "dpos=(%.2f %.2f %.2f|%.2f) head=(%.4f %.4f %.4f) engineFrame=%u tops=%s",
            moveSnaps_, m.idx, team, flags, static_cast<double>(m.pos[0]),
            static_cast<double>(m.pos[1]), static_cast<double>(m.pos[2]),
            static_cast<double>(m.d[0]), static_cast<double>(m.d[1]),
            static_cast<double>(m.d[2]), static_cast<double>(m.delta),
            static_cast<double>(m.head[0]), static_cast<double>(m.head[1]),
            static_cast<double>(m.head[2]), engineFrame, tails);
    PoolNodesLog();
}

// Plain-MSL pool nodes: filter by node+0x28 == P (Run A proved that field holds the
// weapon record pointer; this is the hook-free equivalent of "spawn caller 0x13D703").
void ProbeWatch::PoolNodesLog() {
    if (P_ == 0) return;
    const uintptr_t s = base_ + kRvaPoolSingleton;
    uintptr_t head = 0;
    if (!mem::Read<uintptr_t>(s + 0x98, head)) return;
    uintptr_t node = (head == s + 0x98) ? 0 : head;
    char line[512] = {};
    unsigned n = 0;
    int found = 0;
    int walk = 0;
    while (node != 0 && node != s + 0x98 && walk < 24 && found < 8) {
        uint8_t buf[0x70] = {};
        if (!mem::ReadBytes(node, buf, sizeof(buf))) break;
        uintptr_t next = 0;
        std::memcpy(&next, buf, 8);
        uintptr_t param = 0;
        std::memcpy(&param, buf + 0x28, 8);
        if (param == P_) {
            bool known = false;
            for (int k = 0; k < poolNodeCount_; ++k) {
                if (poolNodes_[k] == node) {
                    known = true;
                    break;
                }
            }
            if (!known && poolNodeCount_ < 8) poolNodes_[poolNodeCount_++] = node;
            float pos[3] = {}, dir[3] = {}, life = 0.f;
            int32_t t60 = 0;
            std::memcpy(pos, buf + 0x34, 12);
            std::memcpy(dir, buf + 0x40, 12);
            std::memcpy(&life, buf + 0x4C, 4);
            std::memcpy(&t60, buf + 0x60, 4);
            App(line, sizeof(line), n,
                " n=%s P=%s st=%u t=%d pos=(%.1f %.1f %.1f) dir=(%.3f %.3f %.3f) life=%.2f",
                mem::Hex(node).c_str(), mem::Hex(param).c_str(),
                static_cast<unsigned>(buf[0x6B]), t60, static_cast<double>(pos[0]),
                static_cast<double>(pos[1]), static_cast<double>(pos[2]),
                static_cast<double>(dir[0]), static_cast<double>(dir[1]),
                static_cast<double>(dir[2]), static_cast<double>(life));
            ++found;
        }
        node = next;
        ++walk;
    }
    if (found > 0) L().Key("PROBE-B: POOL%s", line);
}

// ---------------------------------------------------------------------------
// WeaponModel payload capture (read-only, no watchpoints on it).
// ---------------------------------------------------------------------------
void ProbeWatch::TickInstallHook(double now) {
    if (!hookWanted_ || hookInstalled_ || hookGaveUp_) return;
    if ((now - lastHookTry_) < 0.2) return;
    lastHookTry_ = now;
    ++hookTries_;
    if (InstallAllocHook()) return;
    if (hookTries_ >= 300) {
        hookGaveUp_ = true;
        L().Key("PROBE-B: alloc hook not installed after 60 s; WeaponModel payload capture "
                "disabled (the P watchpoints are unaffected)");
    }
}

bool ProbeWatch::InstallAllocHook() {
    const uintptr_t target = base_ + kRvaAllocate;
    uint8_t pro[sizeof(kAllocPrologue)] = {};
    if (!mem::ReadBytes(target, pro, sizeof(pro)) ||
        std::memcmp(pro, kAllocPrologue, sizeof(pro)) != 0) {
        return false;  // .text not decrypted yet
    }
    hook::InstallResult ir;
    std::string why;
    void* tramp = nullptr;
    if (!hook::Install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&WatchAllocDetour),
                       &tramp, ir, &why)) {
        if (hookTries_ == 1) {
            L().Key("PROBE-B: alloc hook refused: %s", why.c_str());
        }
        return false;
    }
    ::InterlockedExchangePointer(&g_allocTramp, tramp);
    hookResult_ = ir;
    hookTarget_ = reinterpret_cast<void*>(target);
    hookInstalled_ = true;
    L().Key("PROBE-B: alloc hook installed at exe+0x%X (stole %d byte(s)) for WeaponModel "
            "payload capture",
            kRvaAllocate, static_cast<int>(ir.stolenBytes));
    return true;
}

void ProbeWatch::DumpWeaponModelPayload(uintptr_t obj, uint32_t size) {
    uintptr_t vt = 0;
    mem::Read<uintptr_t>(obj, vt);
    const uintptr_t wantVt = base_ + kRvaWeaponModelVtable;
    const uint32_t payload = (size > 0x48) ? (size - 0x48) : 0;
    L().Key("PROBE-B: WMALLOC obj=%s size=0x%X payload=0x%X vtable=%s (%s)", mem::Hex(obj).c_str(),
            size, payload, mem::Hex(vt).c_str(),
            (vt == wantVt) ? "WeaponModel OK" : "NOT the WeaponModel vtable");
    if (payload == 0) return;
    uint32_t dumpLen = payload;
    bool truncated = false;
    if (dumpLen > 0x1000) {
        dumpLen = 0x1000;
        truncated = true;
    }
    const int rows = static_cast<int>((dumpLen + 15) / 16);
    if (wmDumpRows_ + rows > 900) {
        L().Key("PROBE-B: WM payload dump budget exhausted (%d rows logged); not dumping "
                "obj+0x48.. of this object",
                wmDumpRows_);
        return;
    }
    L().Key("PROBE-B: WM payload obj+0x48..0x%X (%d bytes, offsets relative to obj+0x48)%s",
            0x48 + dumpLen, dumpLen, truncated ? " - TRUNCATED at 0x1000" : "");
    for (uint32_t off = 0; off < dumpLen; off += 16) {
        uint8_t row[16] = {};
        if (!mem::ReadBytes(obj + 0x48 + off, row, sizeof(row))) break;
        uint32_t dw[4] = {};
        float f[4] = {};
        std::memcpy(dw, row, sizeof(dw));
        std::memcpy(f, row, sizeof(f));
        char marks[192] = {};
        unsigned mn = 0;
        MarkPayloadRow(row, marks, sizeof(marks), mn);
        L().Key("PROBE-B:   w+0x%03X: %08X %08X %08X %08X | %11.5g %11.5g %11.5g %11.5g |%s",
                0x48 + off, dw[0], dw[1], dw[2], dw[3], static_cast<double>(f[0]),
                static_cast<double>(f[1]), static_cast<double>(f[2]),
                static_cast<double>(f[3]), marks);
        ++wmDumpRows_;
    }
}

void ProbeWatch::AllocCaptureTick() {
    const LONG reserved = g_wmReserved;
    for (LONG i = 0; i < reserved && i < kWmAllocCap; ++i) {
        if (g_wmAlloc[i].ready == 0 || allocSeen_[i]) continue;
        allocSeen_[i] = true;
        ++allocEvents_;
        const uintptr_t ptr = static_cast<uintptr_t>(g_wmAlloc[i].ptr);
        const uint32_t size = static_cast<uint32_t>(g_wmAlloc[i].size);
        L().Key("PROBE-B: WMALLOC event #%d caller=exe+0x%X size=0x%X ptr=%s", allocEvents_,
                kRvaWeaponModelAllocRet, size, mem::Hex(ptr).c_str());
        bool known = false;
        for (int k = 0; k < wmDumpedCount_; ++k) {
            if (wmDumped_[k] == ptr) {
                known = true;
                break;
            }
        }
        if (!known && wmDumpedCount_ < 8) {
            wmDumped_[wmDumpedCount_++] = ptr;
            DumpWeaponModelPayload(ptr, size);
        }
    }
    const LONG overflow = g_wmOverflow;
    if (overflow > 0 && allocEvents_ > 0 && allocEvents_ <= 64) {
        L().Key("PROBE-B: WeaponModel alloc capture overflow: %ld event(s) dropped",
                static_cast<long>(overflow));
        g_wmOverflow = 0;  // note once
    }
}

// ---------------------------------------------------------------------------
// Dispatcher health: exe+0x33ED94 is the only writer of the frame counter, so it
// proves whether FUN_1401B9A80 (the per-frame dispatcher) runs at all.
// ---------------------------------------------------------------------------
void ProbeWatch::CheckEngineFrame(double now) {
    if (frameChecked_) return;
    if (!frameSampleTaken_) {
        frameSampleTaken_ = true;
        frameCheckAt_ = now;
        engineFrame0_ = 0;
        mem::Read<uint32_t>(base_ + 0x33ED94, engineFrame0_);
        engineFrameNow_ = engineFrame0_;
        return;
    }
    if ((now - frameCheckAt_) < 2.0) return;
    frameChecked_ = true;
    uint32_t f1 = 0;
    if (!mem::Read<uint32_t>(base_ + 0x33ED94, f1)) {
        L().Key("PROBE-B: engine frame counter exe+0x33ED94 unreadable - dispatcher state unknown");
        return;
    }
    engineFrameNow_ = f1;
    if (f1 != engineFrame0_) {
        L().Key("PROBE-B: DISPATCHER COUNTER ADVANCES (exe+0x33ED94: %u -> %u): FUN_1401B9A80 "
                "runs, so a dead 0x1B7B00 world hook would be a hook problem, not a phase gate",
                engineFrame0_, f1);
    } else {
        frameFrozen_ = true;
        L().Key("PROBE-B: DISPATCHER COUNTER FROZEN at %u -> plain MSL bypasses 0x1B9A80 (or "
                "the game is paused while loading)",
                engineFrame0_);
    }
}

// When the chain cannot be resolved (MC global / player index / fire object), log
// the raw values instead of guessing, so one run still yields a diagnosis.
void ProbeWatch::LogResolveFailure() {
    const uintptr_t s = base_ + kRvaPoolSingleton;
    uintptr_t mc = 0, fromS = 0, arr = 0, sim = 0;
    uint8_t cnt = 0, pidx = 0xFF;
    const bool mcOk = mem::Read<uintptr_t>(base_ + kRvaMcGlobal, mc) && mc != 0;
    mem::Read<uintptr_t>(s + 0x428, fromS);
    mem::Read<uint8_t>(base_ + kRvaPlayerIdx, pidx);
    const bool mcReadable = mc != 0 && mem::IsReadable(mc + 0x697B8, 8);
    if (mcReadable) {
        mem::Read<uintptr_t>(mc + 0x697B8, arr);
        mem::Read<uint64_t>(mc + 0xA8248, sim);
        mem::Read<uint8_t>(mc + 0xA87E4, cnt);
    }
    L().Key("PROBE-B: RESOLVE FAILED raw: global[exe+0x%X]=%s ok=%d; *(S+0x428)=%s; "
            "mcReadable=%d objArray=%s count=%u simBase=%s playerIdx=%u",
            kRvaMcGlobal, mem::Hex(mc).c_str(), mcOk ? 1 : 0, mem::Hex(fromS).c_str(),
            mcReadable ? 1 : 0, mem::Hex(arr).c_str(), static_cast<unsigned>(cnt),
            mem::Hex(sim).c_str(), static_cast<unsigned>(pidx));
    if (arr != 0 && mem::IsReadable(arr, 8)) {
        uintptr_t o0 = 0;
        mem::Read<uintptr_t>(arr, o0);
        L().Key("PROBE-B: RESOLVE FAILED objArray[0]=%s (a valid obj would have parts at "
                "+0x110 and WC at part+0x1B8 -> WC+0x1FB8 = P)",
                mem::Hex(o0).c_str());
    }
}

// ---------------------------------------------------------------------------
// Vtable presence scan (one-shot, read-only, runs before the watch window).
// Answers whether the plain MSL secretly is a WeaponExFireCtrl/WeaponEx* object.
// ---------------------------------------------------------------------------
void ProbeWatch::VtableScan() {
    if (vtableScanned_) return;
    vtableScanned_ = true;
    L().Key("PROBE-B: VTABLE scan starting (one-shot, read-only; 7 controller vtables)");
    L().Flush();

    uintptr_t vt[7] = {};
    for (int k = 0; k < 7; ++k) vt[k] = base_ + kVtables[k].rva;
    uintptr_t first[7][4] = {};
    int counts[7] = {};

    std::vector<mem::Region> regions;
    mem::EnumRegions(regions, 0x1000);
    std::vector<uint8_t> buf(0x10000 + 8);
    uint64_t scanned = 0;
    for (const mem::Region& r : regions) {
        if (!r.readable || r.executable) continue;
        if (InsideOwnModule(r.base)) continue;
        for (size_t off = 0; off < r.size; off += 0x10000) {
            const size_t len = std::min<size_t>(0x10000, r.size - off);
            if (len < 8) break;
            if (!mem::ReadBytes(r.base + off, buf.data(), len)) continue;
            const size_t n8 = len & ~static_cast<size_t>(7);
            for (size_t i = 0; i < n8; i += 8) {
                uint64_t v = 0;
                std::memcpy(&v, buf.data() + i, 8);
                for (int k = 0; k < 7; ++k) {
                    if (v != static_cast<uint64_t>(vt[k])) continue;
                    if (counts[k] < 4) first[k][counts[k]] = r.base + off + i;
                    if (counts[k] < 512) ++counts[k];
                    break;
                }
            }
            scanned += len;
        }
    }

    for (int k = 0; k < 7; ++k) {
        L().Key("PROBE-B: VTABLE %s count=%d%s", kVtables[k].name, counts[k],
                counts[k] >= 512 ? " (capped)" : "");
        char line[320] = {};
        unsigned n = 0;
        for (int i = 0; i < 4 && i < counts[k]; ++i) {
            App(line, sizeof(line), n, " %s",
                mem::Hex(first[k][i]).c_str());
        }
        if (n > 0) L().Key("PROBE-B: VTABLEOBJ %s:%s", kVtables[k].name, line);
    }
    L().Key("PROBE-B: VTABLE scan done (%.1f MB of data regions)", static_cast<double>(scanned) / 1048576.0);

    // Detail for up to 4 FireCtrl objects: mask + live missile slots.
    const int fireIdx = 0;
    for (int i = 0; i < 4 && i < counts[fireIdx]; ++i) {
        const uintptr_t ctrl = first[fireIdx][i];
        if (!mem::IsReadable(ctrl + 0x5C, 8)) continue;
        uint32_t mask = 0;
        int32_t idx60 = -1;
        mem::Read<uint32_t>(ctrl + 0x5C, mask);
        mem::Read<int32_t>(ctrl + 0x60, idx60);
        L().Key("PROBE-B: FIRE ctrl=%s mask=0x%08X idx60=%d", mem::Hex(ctrl).c_str(), mask, idx60);
        for (int b = 0; b < 16; ++b) {
            if ((mask & (1u << b)) == 0) continue;
            const uintptr_t slot = ctrl + 0x80 + static_cast<uintptr_t>(b) * 0x68;
            uint8_t sb[0x58] = {};
            if (!mem::IsReadable(slot, sizeof(sb)) || !mem::ReadBytes(slot, sb, sizeof(sb))) continue;
            float pos[4] = {}, head[3] = {}, vel[3] = {}, tgt[3] = {}, age = 0.f;
            std::memcpy(pos, sb + 0x00, 16);
            std::memcpy(head, sb + 0x10, 12);
            std::memcpy(vel, sb + 0x20, 12);
            std::memcpy(tgt, sb + 0x30, 12);
            std::memcpy(&age, sb + 0x54, 4);
            L().Key("PROBE-B: FIRE   slot%d=0x%llX pos=(%.1f %.1f %.1f %.2f) "
                    "head=(%.4g %.4g %.4g) vel=(%.2f %.2f %.2f) tgt=(%.1f %.1f %.1f) "
                    "tgtIdx=%u/%u valid=%u/%u age=%.2f",
                    b, static_cast<unsigned long long>(slot), static_cast<double>(pos[0]),
                    static_cast<double>(pos[1]), static_cast<double>(pos[2]),
                    static_cast<double>(pos[3]), static_cast<double>(head[0]),
                    static_cast<double>(head[1]), static_cast<double>(head[2]),
                    static_cast<double>(vel[0]), static_cast<double>(vel[1]),
                    static_cast<double>(vel[2]), static_cast<double>(tgt[0]),
                    static_cast<double>(tgt[1]), static_cast<double>(tgt[2]),
                    static_cast<unsigned>(sb[0x50]), static_cast<unsigned>(sb[0x51]),
                    static_cast<unsigned>(sb[0x52]), static_cast<unsigned>(sb[0x53]),
                    static_cast<double>(age));
        }
    }
}

// ---------------------------------------------------------------------------
// Heartbeat + state machine
// ---------------------------------------------------------------------------
void ProbeWatch::Heartbeat(double now) {
    if ((now - lastHeartbeat_) < 5.0) return;
    lastHeartbeat_ = now;
    L().Key("PROBE-B: heartbeat t=%.0fs phase=%s hits=%lld distinctRips=%d threads=%d "
            "sweeps=%d movSnaps=%d poolNodes=%d allocEvents=%d payloadDumps=%d",
            now - start_, finished_ ? "finished" : (armed_ ? "armed" : "waiting"),
            static_cast<long long>(g_totalHits), ripCount_, threadsArmed_, armSweeps_, moveSnaps_,
            poolNodeCount_, allocEvents_, wmDumpedCount_);
}

void ProbeWatch::Tick() {
    if (!active_) return;
    const double now = Now();

    // 0) Dispatcher frame-counter health check (2 s after the first sample).
    CheckEngineFrame(now);

    // 1) Optional WeaponModel payload capture (own allocator hook, never conflicts
    //    with Run A because the patcher only enables one of the two probes).
    if (hookWanted_ && !hookInstalled_ && !hookGaveUp_) TickInstallHook(now);
    if (hookInstalled_) AllocCaptureTick();

    // 2) Wait for the mission data, then locate P.
    if (!pFound_) {
        if ((now - lastResolveTry_) >= 0.2) {
            lastResolveTry_ = now;
            ++resolveTries_;
            if (ResolveChain()) {
                pFound_ = true;
                LogChain();
                LogPBlock();
                VtableScan();  // heavy one-shot, deliberately before the watch window
                StartArming(Now());
            } else if ((now - start_) > 180.0) {
                // The mission data is only live after the user selects a mission, which can
                // take a while in the menus; the hard stop is generous on purpose.
                LogResolveFailure();
                VtableScan();  // still useful evidence even without P
                Finish("mission data / P never became valid");
            } else if (resolveTries_ == 1 || (resolveTries_ % 50) == 0) {
                L().Key("PROBE-B: waiting for the mission data / P (t=%.0fs, retry every 200 ms, "
                        "give up at 180 s) - enter a mission",
                        now - start_);
            }
        }
        Heartbeat(now);
        return;
    }
    if (finished_) return;

    // 3) Measurement window.
    if (armed_) {
        if ((now - lastArmSweep_) > 0.2) {
            lastArmSweep_ = now;
            ArmAllThreads(false);
            ++armSweeps_;
        }
        DrainHits();
        if (g_totalHits >= 400) {
            Finish("400 hits");
            return;
        }
        if ((now - armedAt_) > static_cast<double>(seconds_)) {
            Finish("window elapsed");
            return;
        }
        MoveSnapshotTick(now);
    }
    Heartbeat(now);
}

} // namespace acz
