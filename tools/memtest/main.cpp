// ACZPropNav - tools/memtest/main.cpp
//
// Acceptance tests for src/mem (memory primitives) and src/mem/discovery (the
// signature-driven parameter-table / entity locator).
//
// The tests build a synthetic "game image" inside the test process:
//
//   * a parameter table with stride 0x40 and three rows, using invented but
//     ACZ-ish vanilla values (see kRows below),
//   * a deliberately NON-enum-order layout (see kOff* below) so the correlator
//     has to *discover* the offsets instead of assuming them,
//   * decoy floats with an inconsistent layout that must never be verified,
//   * three fake missile entities that reference the rows by pointer and whose
//     position/target-velocity slots are advanced by a background thread between
//     the two CorrelateMotion samples.
//
// Everything is deterministic (fixed values, fixed offsets, bounded scan caps)
// and finishes well under 10 s.
//
// Exit code: 0 when every check passed, 1 otherwise.
#include <windows.h>

#include <csetjmp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mem/discovery.h"
#include "mem/mem.h"

using namespace acz;
using namespace acz::game;

// ===========================================================================
// Test plumbing
// ===========================================================================

namespace {

int g_checks = 0;
int g_failures = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

// One test case: prints its name on entry and PASS on exit when no check failed.
struct TestScope {
    const char* name;
    int before;
    explicit TestScope(const char* n) : name(n), before(g_failures) { std::printf("- %s\n", n); }
    ~TestScope() {
        if (g_failures == before) std::printf("  PASS: %s\n", name);
    }
};

void DumpDiag(const std::vector<std::string>& diag) {
    std::printf("  -- diagnostics (%zu lines) --\n", diag.size());
    for (const std::string& s : diag) std::printf("  | %s\n", s.c_str());
}

bool Near(float a, float b, float relTol) {
    const float d = std::fabs(a - b);
    return d <= relTol * std::fabs(b) + 1e-3f;
}

// ===========================================================================
// Synthetic "game image"
// ===========================================================================

// Parameter layout used by the synthetic table.  Deliberately NOT the Property
// enum order (enum order is MaxHomingAngle=1, MaxRotationAngle=2, LifeTime=3,
// LockonRange=4): here MaxRotationAngle comes first.
constexpr uint32_t kOffMR = 0x00;  // MaxRotationAngle
constexpr uint32_t kOffMH = 0x08;  // MaxHomingAngle
constexpr uint32_t kOffLT = 0x10;  // LifeTime
constexpr uint32_t kOffLR = 0x18;  // LockonRange
constexpr uint32_t kStride = 0x40;
constexpr uintptr_t kTableOff = 0x100;  // table start inside the table allocation

struct RowDesc {
    const char* model;
    float mr;  // MaxRotationAngle (deg/s)
    float mh;  // MaxHomingAngle    (deg)
    float lt;  // LifeTime          (s)
    float lr;  // LockonRange       (m)
};

// Invented values; ACZ missiles are in this ballpark (docs/VANILLA.md is the
// real calibration table, owned by another teammate).  QAAM reuses MSL's 75 and
// intentionally collides MaxRotationAngle 75 with MSL's MaxHomingAngle 75 so the
// correlator has to disambiguate by model, not by value alone.
const RowDesc kRows[3] = {
    {"msl", 40.f, 75.f, 9.f, 2000.f},
    {"qaam", 75.f, 120.f, 9.f, 2000.f},
    {"laam", 150.f, 90.f, 12.f, 3000.f},
};

// Fake entity layout (documented in the test, not derived from the game):
//   +0x40 : uint64 pointer to the parameter row
//   +0x80 : missilePos   (Vec3, advanced by the background thread)
//   +0x90 : missileVel   (Vec3, constant)
//   +0xA0 : targetPos    (Vec3, advanced by the background thread)
//   +0xB0 : targetVel    (Vec3, constant)
constexpr uint32_t kOffRowPtr = 0x40;
constexpr uint32_t kOffPos = 0x80;
constexpr uint32_t kOffVel = 0x90;
constexpr uint32_t kOffTPos = 0xA0;
constexpr uint32_t kOffTVel = 0xB0;
constexpr uint32_t kEntitySize = 0x400;

// No value here may equal a signature value (keeps the synthetic entity heap
// from creating parameter-table anchors).
const Vec3 kInitPos(1000.f, 1500.f, 2500.f);
const Vec3 kVel(900.f, 60.f, 0.f);
const Vec3 kInitTPos(5000.f, 1300.f, 4000.f);
const Vec3 kTVel(140.f, 0.f, 30.f);

uint8_t* g_tableMem = nullptr;    // 256 KiB, holds the parameter table
uint8_t* g_decoyMem = nullptr;    // 64 KiB, inconsistent decoy floats
uint8_t* g_entityMem = nullptr;   // 3 * kEntitySize, the fake missiles
uintptr_t g_tableBase = 0;        // == g_tableMem + kTableOff

// Decoy patterns: two identical copies of "MSL values at 4-byte spacing" plus one
// "QAAM values at 4-byte spacing" copy.  They produce a consistent *unverified*
// hypothesis (two supporting rows) but no model2 cross-match, which is exactly
// the "must not be verified" property under test.
void BuildDecoys() {
    std::printf("  (building decoys at %p)\n", static_cast<void*>(g_decoyMem));
    std::memset(g_decoyMem, 0, 64 * 1024);
    auto put = [](uint32_t at, float v) { std::memcpy(g_decoyMem + at, &v, sizeof(v)); };

    // Trap 1: two copies of a *packed* (4-byte field) MSL row at stride 0x100.
    // It looks table-ish (support 2, consistent stride) but no second model
    // confirms those offsets, so it must stay unverified.
    for (int copy = 0; copy < 2; ++copy) {
        const uint32_t base = 0x100 + static_cast<uint32_t>(copy) * 0x100;
        put(base + 0x0, kRows[0].mr);
        put(base + 0x4, kRows[0].mh);
        put(base + 0x8, kRows[0].lt);
        put(base + 0xC, kRows[0].lr);
    }
    // Trap 2: QAAM values with a DIFFERENT field order, so it cannot confirm trap 1.
    put(0x300 + 0x0, kRows[1].mh);
    put(0x300 + 0x4, kRows[1].mr);
    put(0x300 + 0x8, kRows[1].lt);
    put(0x300 + 0xC, kRows[1].lr);
    // Trap 3: one QAAM-shaped row at the *real* 8-byte layout, far away from the
    // synthetic table.  It agrees with the real layout but is not contiguous with
    // it, so it must never become a verified row.
    put(0x7000 + 0x0, kRows[1].mr);
    put(0x7000 + 0x8, kRows[1].mh);
    put(0x7000 + 0x10, kRows[1].lt);
    put(0x7000 + 0x18, kRows[1].lr);
    // Isolated floats far apart: no consistent relative offsets at all.
    put(0x800, kRows[0].mh);
    put(0x900, kRows[0].mr);
    put(0xA00, kRows[0].lt);
    put(0xB00, kRows[0].lr);
    put(0xC00, kRows[1].mh);
    put(0xD00, kRows[2].mr);
    // Harmless filler away from every trap: values in [0.5, 2.5] never match a
    // signature value.
    float* filler = reinterpret_cast<float*>(g_decoyMem + 0x2000);
    for (int i = 0; i < 2000; ++i) filler[i] = 0.5f + 0.001f * static_cast<float>(i);
}
void BuildTable() {
    std::memset(g_tableMem, 0, 256 * 1024);
    g_tableBase = reinterpret_cast<uintptr_t>(g_tableMem) + kTableOff;
    for (int i = 0; i < 3; ++i) {
        uint8_t* row = g_tableMem + kTableOff + static_cast<size_t>(i) * kStride;
        std::memcpy(row + kOffMR, &kRows[i].mr, sizeof(float));
        std::memcpy(row + kOffMH, &kRows[i].mh, sizeof(float));
        std::memcpy(row + kOffLT, &kRows[i].lt, sizeof(float));
        std::memcpy(row + kOffLR, &kRows[i].lr, sizeof(float));
    }
}

uintptr_t RowAddress(int index) {
    return g_tableBase + static_cast<uintptr_t>(index) * kStride;
}

ModelSignature MakeSignature(const char* model) {
    ModelSignature sig;
    sig.modelKey = model;
    const RowDesc* d = nullptr;
    for (const RowDesc& r : kRows) {
        if (std::strcmp(r.model, model) == 0) d = &r;
    }
    if (!d) return sig;
    auto add = [&sig](Property p, float v) {
        KnownValue kv;
        kv.prop = p;
        kv.value = v;
        kv.tolerance = 0.02f;
        sig.values.push_back(kv);
    };
    add(Property::MaxRotationAngle, d->mr);
    add(Property::MaxHomingAngle, d->mh);
    add(Property::LifeTime, d->lt);
    add(Property::LockonRange, d->lr);
    return sig;
}

DiscoveryOptions MakeOptions(const std::vector<std::string>& models) {
    DiscoveryOptions opt;
    for (const std::string& m : models) opt.models.push_back(MakeSignature(m.c_str()));
    opt.windowBefore = 0x40;
    opt.windowAfter = 0x80;
    opt.maxAnchorHits = 2000;
    opt.maxRows = 64;
    opt.minScore = 3;
    opt.scanImageData = true;
    opt.scanPrivate = true;
    // Hard scope: only the synthetic game image.  The table is where the rows
    // must be found; the decoy block is deliberately included so the test proves
    // the correlator rejects inconsistent layouts even when it is allowed to see
    // them.  (Production patcher.cpp scopes to the game's module/heap instead.)
    opt.onlyRanges.push_back({reinterpret_cast<uintptr_t>(g_tableMem), 256 * 1024});
    opt.onlyRanges.push_back({reinterpret_cast<uintptr_t>(g_decoyMem), 64 * 1024});
    return opt;
}

// Compares a discovered row's property->offset mapping with the synthetic one.
bool LayoutMatches(const ParamRow& row) {
    bool seenMR = false, seenMH = false, seenLT = false, seenLR = false;
    for (const PropertySlot& s : row.slots) {
        switch (s.prop) {
            case Property::MaxRotationAngle:
                if (s.offset != kOffMR) return false;
                seenMR = true;
                break;
            case Property::MaxHomingAngle:
                if (s.offset != kOffMH) return false;
                seenMH = true;
                break;
            case Property::LifeTime:
                if (s.offset != kOffLT) return false;
                seenLT = true;
                break;
            case Property::LockonRange:
                if (s.offset != kOffLR) return false;
                seenLR = true;
                break;
            default:
                return false;  // HomingForesightAmount is not a vanilla value
        }
    }
    return seenMR && seenMH && seenLT && seenLR && row.slots.size() == 4;
}

bool RowValuesMatch(const ParamRow& row) {
    const RowDesc* d = nullptr;
    for (const RowDesc& r : kRows) {
        if (row.modelKey == r.model) d = &r;
    }
    if (!d) return false;
    for (const PropertySlot& s : row.slots) {
        float expected = 0.f;
        switch (s.prop) {
            case Property::MaxRotationAngle: expected = d->mr; break;
            case Property::MaxHomingAngle: expected = d->mh; break;
            case Property::LifeTime: expected = d->lt; break;
            case Property::LockonRange: expected = d->lr; break;
            default: return false;
        }
        if (std::fabs(s.foundValue - expected) > 0.01f) return false;
    }
    return true;
}

bool InTableAllocation(uintptr_t addr) {
    return addr >= reinterpret_cast<uintptr_t>(g_tableMem) &&
           addr < reinterpret_cast<uintptr_t>(g_tableMem) + 256 * 1024;
}

bool InDecoyAllocation(uintptr_t addr) {
    return addr >= reinterpret_cast<uintptr_t>(g_decoyMem) &&
           addr < reinterpret_cast<uintptr_t>(g_decoyMem) + 64 * 1024;
}

acz::mem::Region MakeManualRegion(uintptr_t base, size_t size) {
    acz::mem::Region r;
    r.base = base;
    r.size = size;
    r.protect = PAGE_READWRITE;
    r.state = MEM_COMMIT;
    r.type = MEM_PRIVATE;
    r.readable = true;
    r.writable = true;
    r.executable = false;
    return r;
}

// ===========================================================================
// Fake missile motion thread
// ===========================================================================

volatile LONG g_motionRun = 1;
uintptr_t g_entityBase[3] = {};

DWORD WINAPI MotionThread(LPVOID) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER last{};
    QueryPerformanceCounter(&last);
    while (InterlockedCompareExchange(&g_motionRun, 1, 1) == 1) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double dt = static_cast<double>(now.QuadPart - last.QuadPart) /
                    static_cast<double>(freq.QuadPart ? freq.QuadPart : 1);
        last = now;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.25) dt = 0.25;  // clamp a scheduling hiccup
        const float step = static_cast<float>(dt);
        for (int i = 0; i < 3; ++i) {
            uint8_t* base = reinterpret_cast<uint8_t*>(g_entityBase[i]);
            if (!base) continue;
            Vec3* pos = reinterpret_cast<Vec3*>(base + kOffPos);
            Vec3* vel = reinterpret_cast<Vec3*>(base + kOffVel);
            Vec3* tpos = reinterpret_cast<Vec3*>(base + kOffTPos);
            Vec3* tvel = reinterpret_cast<Vec3*>(base + kOffTVel);
            *pos += *vel * step;
            *tpos += *tvel * step;
        }
        Sleep(1);
    }
    return 0;
}

// ===========================================================================
// Test-level fault handler (proves the mem guard does not swallow other faults)
// ===========================================================================

volatile int g_testCatching = 0;
volatile LONG g_testHandlerCalls = 0;
jmp_buf g_testJb;

LONG CALLBACK TestLastChanceHandler(EXCEPTION_POINTERS* ep) {
    if (g_testCatching && ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        InterlockedIncrement(&g_testHandlerCalls);
        longjmp(g_testJb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

volatile uintptr_t g_badAddress = 0x10;

__attribute__((noinline)) void FaultOutsideGuard() {
    volatile int* p = reinterpret_cast<volatile int*>(g_badAddress);
    *p = 1;  // deliberate access violation, unguarded
}

// ===========================================================================
// Code-reference fixture
// ===========================================================================

volatile float g_codeRefTarget[4] = {0.f, 0.f, 0.f, 0.f};

extern "C" __attribute__((noinline)) float CodeRefProbe() {
    return g_codeRefTarget[1];  // RIP-relative load of the fixture
}

// ===========================================================================
// Tests
// ===========================================================================

void TestProcessAndModule() {
    TestScope t("process / module enumeration");
    Check(acz::mem::Process() == GetCurrentProcess(), "Process() defaults to the current process");
    acz::mem::ModuleInfo mi;
    Check(acz::mem::MainModule(mi), "MainModule() succeeds");
    Check(mi.base != 0 && mi.size != 0, "MainModule base/size valid");
    Check(!mi.path.empty(), "MainModule path non-empty");
    acz::mem::ModuleInfo mi2;
    Check(acz::mem::ModuleForAddress(reinterpret_cast<uintptr_t>(&TestProcessAndModule), mi2) &&
              mi2.base == mi.base,
          "ModuleForAddress resolves the test exe");

    std::vector<acz::mem::Region> regions;
    acz::mem::EnumRegions(regions);
    Check(regions.size() > 0, "EnumRegions returns regions");
    bool foundText = false;
    for (const acz::mem::Region& r : regions) {

        if (r.image && r.executable && r.readable) foundText = true;
    }
    Check(foundText, "EnumRegions reports the image's executable region");
    Check(acz::mem::WritableRegions(0x1000).size() > 0, "WritableRegions non-empty");
    Check(acz::mem::ExecutableRegions(0x1000).size() > 0, "ExecutableRegions non-empty");
    Check(acz::mem::Hex(0x1234) == "0x1234", "Hex formatting");
}

void TestReadSafety() {
    TestScope t("SEH-safe ReadBytes");
    uint8_t buf[64] = {};
    Check(!acz::mem::ReadBytes(0, buf, 4), "ReadBytes(null) == false");
    Check(!acz::mem::ReadBytes(1, buf, 4), "ReadBytes(0x1) == false");
    Check(!acz::mem::ReadBytes(0, buf, 0), "ReadBytes(size=0) == false");
    Check(!acz::mem::ReadBytes(0, nullptr, 4), "ReadBytes(dst=null) == false");
    Check(!acz::mem::ReadBytes(reinterpret_cast<uintptr_t>(&buf[0]), buf, 64 * 1024 + 4),
          "ReadBytes(oversized) == false");
    Check(!acz::mem::IsReadable(0, 4), "IsReadable(null) == false");
    Check(acz::mem::IsReadable(reinterpret_cast<uintptr_t>(buf), 16), "IsReadable(valid) == true");
    Check(acz::mem::ReadBytes(reinterpret_cast<uintptr_t>(buf) + 1, buf + 8, 4),
          "ReadBytes(unaligned source) works (reads must not require alignment)");

    // Guard page must be reported unreadable and reading into it must fail, not
    // crash (this is the "no guard page consumption" property).
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    uint8_t* pages = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(pages != nullptr, "VirtualAlloc for guard-page test");
    if (pages) {
        DWORD old = 0;
        VirtualProtect(pages + page, page, PAGE_NOACCESS, &old);
        Check(!acz::mem::IsReadable(reinterpret_cast<uintptr_t>(pages + page), 4),
              "IsReadable(no-access page) == false");
        Check(!acz::mem::ReadBytes(reinterpret_cast<uintptr_t>(pages + page - 8), buf, 16),
              "ReadBytes spanning into a no-access page == false");
        Check(acz::mem::ReadBytes(reinterpret_cast<uintptr_t>(pages + page - 8), buf, 8),
              "ReadBytes that stays inside the readable page succeeds");
        VirtualProtect(pages + page, page, PAGE_GUARD | PAGE_READWRITE, &old);
        Check(!acz::mem::IsReadable(reinterpret_cast<uintptr_t>(pages + page), 4),
              "IsReadable(PAGE_GUARD) == false");
        VirtualFree(pages, 0, MEM_RELEASE);
    }

    // Stress: many sequential faults must not destabilise the guard.
    int caught = 0;
    for (int i = 0; i < 200; ++i) {
        if (!acz::mem::ReadBytes(static_cast<uintptr_t>(i) * 16 + 8, buf, 8)) ++caught;
    }
    Check(caught == 200, "200 sequential invalid reads all reported false");
}

void TestWriteSafety() {
    TestScope t("WriteBytes + protection changes");
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    uint8_t* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(mem != nullptr, "VirtualAlloc for write test");
    if (!mem) return;
    const float v = 1234.5f;
    std::memcpy(mem + 0x10, &v, sizeof(v));

    DWORD old = 0;
    VirtualProtect(mem, page, PAGE_READONLY, &old);

    const float nv = 42.25f;
    Check(acz::mem::WriteBytes(reinterpret_cast<uintptr_t>(mem + 0x10), &nv, sizeof(nv)),
          "WriteBytes to a read-only page succeeds after re-protect");
    float back = 0.f;
    Check(acz::mem::ReadBytes(reinterpret_cast<uintptr_t>(mem + 0x10), &back, sizeof(back)) &&
              back == nv,
          "written value reads back");
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(mem, &mbi, sizeof(mbi));
    Check(mbi.Protect == PAGE_READONLY, "original page protection restored");

    Check(!acz::mem::WriteBytes(0, &v, 4), "WriteBytes(null) == false");
    Check(!acz::mem::WriteBytes(reinterpret_cast<uintptr_t>(mem), nullptr, 4),
          "WriteBytes(src=null) == false");
    Check(!acz::mem::WriteBytes(reinterpret_cast<uintptr_t>(mem), &v, 0), "WriteBytes(size=0) == false");
    Check(!acz::mem::WriteBytes(reinterpret_cast<uintptr_t>(mem), &v, 64 * 1024 + 4),
          "WriteBytes(oversized) == false");
    Check(!acz::mem::WriteBytes(reinterpret_cast<uintptr_t>(mem) + 1, &v, 4),
          "WriteBytes(unaligned) == false");

    VirtualFree(mem, 0, MEM_RELEASE);

    // REGRESSION: a write straddling a page boundary must restore EACH page's own
    // protection. VirtualProtectEx reports one protection value for a multi-page
    // range (the first page's), so the old single-save/single-restore code left the
    // trailing page at the first page's protection - e.g. PAGE_READONLY instead of
    // PAGE_READWRITE, which makes the game's own next write there fault.
    uint8_t* two = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(two != nullptr, "VirtualAlloc for page-crossing write test");
    if (two) {
        DWORD o1 = 0, o2 = 0;
        VirtualProtect(two, page, PAGE_READONLY, &o1);
        VirtualProtect(two + page, page, PAGE_EXECUTE_READ, &o2);

        // 12 bytes starting 8 bytes before the boundary: spans both pages.
        const float vec[3] = {1.5f, 2.5f, 3.5f};
        const bool ok = acz::mem::WriteBytes(reinterpret_cast<uintptr_t>(two + page - 8),
                                             vec, sizeof(vec));
        Check(ok, "page-crossing 12-byte write succeeds");

        MEMORY_BASIC_INFORMATION m1{}, m2{};
        VirtualQuery(two, &m1, sizeof(m1));
        VirtualQuery(two + page, &m2, sizeof(m2));
        if (m1.Protect != PAGE_READONLY) {
            std::printf("      first page protect = 0x%lX (expected PAGE_READONLY)\n",
                        static_cast<unsigned long>(m1.Protect));
        }
        Check(m1.Protect == PAGE_READONLY,
              "first page keeps its own protection (PAGE_READONLY)");
        if (m2.Protect != PAGE_EXECUTE_READ) {
            std::printf("      second page protect = 0x%lX (expected PAGE_EXECUTE_READ)\n",
                        static_cast<unsigned long>(m2.Protect));
        }
        Check(m2.Protect == PAGE_EXECUTE_READ,
              "second page keeps ITS OWN protection (PAGE_EXECUTE_READ, not the first's)");

        float back2 = 0.f;
        Check(acz::mem::ReadBytes(reinterpret_cast<uintptr_t>(two + page - 4), &back2, 4) &&
                  back2 == 3.5f,
              "page-crossing write landed correct bytes");
        VirtualFree(two, 0, MEM_RELEASE);
    }
}

void TestScanBytes() {
    TestScope t("ScanBytes (guard page adjacency, chunk bounds)");
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    uint8_t* pages = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(pages != nullptr, "VirtualAlloc for scan test");
    if (!pages) return;
    std::memset(pages, 0, page * 2);
    const uint8_t pattern[4] = {0x11, 0x22, 0x33, 0x44};
    std::memcpy(pages + page - sizeof(pattern), pattern, sizeof(pattern));

    acz::mem::Region r = MakeManualRegion(reinterpret_cast<uintptr_t>(pages), page);
    std::vector<uintptr_t> hits;
    acz::mem::ScanBytes(r, pattern, "xxxx", sizeof(pattern), hits, 8);
    Check(hits.size() == 1 && hits[0] == reinterpret_cast<uintptr_t>(pages + page - sizeof(pattern)),
          "pattern at the last bytes of a page is found");

    DWORD old = 0;
    VirtualProtect(pages + page, page, PAGE_NOACCESS, &old);
    // Overstate the region so the scanner runs into the no-access page: it must
    // fail the chunk copy and not fault.
    acz::mem::Region over = MakeManualRegion(reinterpret_cast<uintptr_t>(pages), page * 2);
    hits.clear();
    acz::mem::ScanBytes(over, pattern, "xxxx", sizeof(pattern), hits, 8);
    Check(hits.empty() || hits[0] < reinterpret_cast<uintptr_t>(pages + page),
          "scan over a no-access page does not fault and reports nothing bogus");

    // Wildcard mask works.
    std::vector<uintptr_t> wild;
    acz::mem::ScanBytes(r, pattern, "xx??", sizeof(pattern), wild, 8);
    Check(!wild.empty(), "wildcard mask matches");
    VirtualFree(pages, 0, MEM_RELEASE);
}

void TestFindFloatRuns() {
    TestScope t("FindFloatRuns");
    acz::mem::Region r = MakeManualRegion(reinterpret_cast<uintptr_t>(g_tableMem), 256 * 1024);
    const float want = 75.f;
    const float tol = 0.02f;
    std::vector<uintptr_t> hits;
    acz::mem::FindFloatRuns(r, &want, &tol, 1, hits, 64);
    Check(!hits.empty(), "75.0 found in the synthetic table");
    bool atMslMh = false, atQaamMr = false;
    for (uintptr_t h : hits) {
        if (h == RowAddress(0) + kOffMH) atMslMh = true;
        if (h == RowAddress(1) + kOffMR) atQaamMr = true;
        if ((h & 3) != 0) Check(false, "FindFloatRuns returned an unaligned hit");
    }
    Check(atMslMh, "hit at msl.MaxHomingAngle (+0x08)");
    Check(atQaamMr, "hit at qaam.MaxRotationAngle (+0x00)");
    // Multi-element run: MR/MH of MSL are not adjacent, so a 2-run must fail here
    // and a 1-run must succeed; this also exercises the tolerance path.
    const float twoValues[2] = {kRows[0].mr, kRows[0].mh};
    const float twoTols[2] = {0.02f, 0.02f};
    std::vector<uintptr_t> runHits;
    acz::mem::FindFloatRuns(r, twoValues, twoTols, 2, runHits, 8);
    Check(runHits.empty(), "no false 2-element run for the non-adjacent synthetic layout");
}

void TestScanAll() {
    TestScope t("ScanAll");
    const uint8_t unique[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67};
    std::memcpy(g_tableMem + 0x8000, unique, sizeof(unique));
    std::vector<uintptr_t> hits;
    acz::mem::ScanAll(false, false, unique, "xxxxxxxx", sizeof(unique), hits, 64);
    bool found = false;
    for (uintptr_t h : hits) {
        if (h == reinterpret_cast<uintptr_t>(g_tableMem) + 0x8000) found = true;
    }
    Check(found, "ScanAll finds the planted pattern in writable memory");
}

void TestRipRelativeRefs() {
    TestScope t("CollectRipRelativeRefs / FindCodeReferences");
    // Force the fixture function to be emitted and executed.
    const float probeValue = CodeRefProbe();
    Check(probeValue == 0.f, "code-ref fixture is readable");

    const uintptr_t lo = reinterpret_cast<uintptr_t>(&g_codeRefTarget[0]);
    const uintptr_t hi = lo + sizeof(g_codeRefTarget);

    acz::mem::ModuleInfo mi;
    Check(acz::mem::MainModule(mi), "main module for code scan");
    acz::mem::Region code = MakeManualRegion(mi.base, mi.size);
    code.executable = true;
    code.readable = true;
    code.image = true;
    std::vector<uintptr_t> refs;
    acz::mem::CollectRipRelativeRefs(code, lo, hi, refs, 256);
    Check(!refs.empty(), "CollectRipRelativeRefs found the RIP-relative load");

    std::vector<CodeRefSite> sites;
    FindCodeReferences(lo, hi, sites, 256);
    Check(!sites.empty(), "FindCodeReferences found the instruction");
    bool targetOk = true;
    for (const CodeRefSite& s : sites) {
        if (s.target < lo || s.target >= hi) targetOk = false;
        if (s.insnAddr == 0) targetOk = false;
    }
    Check(targetOk, "every site has insnAddr != 0 and target inside the range");

    // A range that no code references: the synthetic table is heap memory.
    std::vector<CodeRefSite> none;
    FindCodeReferences(g_tableBase, g_tableBase + kStride * 3, none, 256);
    Check(none.empty(), "no code references to the synthetic heap table");
}

// ---- discovery -------------------------------------------------------------

struct LocateOutcome {
    bool ok = false;
    ParamTableResult result;
    std::vector<std::string> diag;
    int verifiedRows = 0;
    bool allVerifiedInTable = true;
    bool layoutsMatch = true;
    bool valuesMatch = true;
    bool modelsMatch = true;
    bool tableBaseOk = false;
    bool strideOk = false;
    bool decoysClean = true;
};

LocateOutcome RunLocate(const std::vector<std::string>& models) {
    LocateOutcome o;
    DiscoveryOptions opt = MakeOptions(models);
    o.ok = LocateParameterTables(opt, o.result, o.diag);
    for (const ParamRow& r : o.result.rows) {
        if (!r.verified) continue;
        ++o.verifiedRows;
        if (!InTableAllocation(r.rowAddr)) {
            o.allVerifiedInTable = false;
            std::printf("  bogus verified row: model=%s addr=0x%llX slots=%zu\n", r.modelKey.c_str(),
                        static_cast<unsigned long long>(r.rowAddr), r.slots.size());
            for (const PropertySlot& s : r.slots) {
                std::printf("    %s +0x%X = %.4f\n", PropertyName(s.prop), s.offset, s.foundValue);
            }
        }
        if (InDecoyAllocation(r.rowAddr)) o.decoysClean = false;
        if (!LayoutMatches(r)) o.layoutsMatch = false;
        if (!RowValuesMatch(r)) o.valuesMatch = false;
        // Every verified row must belong to one of the requested models.
        bool modelFound = false;
        for (const std::string& m : models) {
            if (r.modelKey == m) modelFound = true;
        }
        if (!modelFound) o.modelsMatch = false;
    }
    o.tableBaseOk = (o.result.tableBase == g_tableBase);
    o.strideOk = (o.result.stride == kStride);
    return o;
}

void TestLocate3Models(const LocateOutcome& o) {
    TestScope t("LocateParameterTables (3 models, cross-validated)");
    Check(o.ok, "LocateParameterTables returns true");
    Check(o.verifiedRows >= 3, "at least 3 verified rows (one per synthetic model)");
    Check(o.allVerifiedInTable, "all verified rows live in the synthetic table");
    Check(o.decoysClean, "no decoy float pattern was verified");
    Check(o.layoutsMatch, "recovered offsets == synthetic layout (MR+0, MH+8, LT+0x10, LR+0x18)");
    Check(o.valuesMatch, "recovered values match the synthetic values");
    Check(o.modelsMatch, "each verified row is attributed to a requested model");
    Check(o.tableBaseOk, "tableBase == first synthetic row");
    Check(o.strideOk, "stride == 0x40");
    Check(!o.diag.empty(), "diagnostics were produced");
    // Every synthetic row must be present as a verified row.
    for (int i = 0; i < 3; ++i) {
        bool found = false;
        for (const ParamRow& r : o.result.rows) {
            if (r.verified && r.rowAddr == RowAddress(i)) found = true;
        }
        if (!found) {
            char what[96];
            std::snprintf(what, sizeof(what), "synthetic row %d (0x%llX) was verified", i,
                          static_cast<unsigned long long>(RowAddress(i)));
            Check(false, what);
        }
    }
    if (g_failures != t.before) DumpDiag(o.diag);
}

// Runs the two-model scenario in a *fresh process*.  Discovery is a one-shot
// operation in the mod (one call per process); running it twice in one process
// would let the first run's own result vectors (which contain the discovered
// vanilla values) act as a second, table-shaped value copy - by design the
// locator cannot tell that copy from game data, so the clean-process test is the
// honest one.
void TestLocate2Models() {
    TestScope t("LocateParameterTables (2 models, separate process)");
    wchar_t path[MAX_PATH * 2] = {};
    Check(GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0]))) != 0,
          "own executable path");
    std::wstring cmd = L"\"";
    cmd += path;
    cmd += L"\" --locate2";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL spawned = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE /*inherit stdout*/,
                                        0, nullptr, nullptr, &si, &pi);
    Check(spawned != FALSE, "2-model child process spawned");
    if (!spawned) return;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 20000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Check(wait == WAIT_OBJECT_0, "child process finished");
    Check(exitCode == 0, "2-model discovery passed in the child process");
    if (exitCode != 0) std::printf("  child exit code: %lu\n", static_cast<unsigned long>(exitCode));
}

void TestFindEntities(const std::vector<ParamRow>& rows) {
    TestScope t("FindEntities");
    std::vector<EntityCandidate> candidates;
    std::vector<std::string> diag;
    const bool ok = FindEntities(rows, candidates, 2048, diag);
    Check(ok, "FindEntities returns true");
    Check(!candidates.empty(), "at least one entity candidate");

    // The three synthetic entities must have been found, with the exact field
    // address of their row pointer.
    for (int i = 0; i < 3; ++i) {
        const uintptr_t wantField = reinterpret_cast<uintptr_t>(g_entityMem) +
                                    static_cast<uintptr_t>(i) * kEntitySize + kOffRowPtr;
        bool found = false;
        for (const EntityCandidate& c : candidates) {
            if (c.rowPtrAddr == wantField) found = true;
        }
        if (!found) {
            char what[120];
            std::snprintf(what, sizeof(what), "entity %d row pointer at 0x%llX was found", i,
                          static_cast<unsigned long long>(wantField));
            Check(false, what);
        }
    }
    // Base convention: the reported base is the pointer field rounded down to 16.
    for (const EntityCandidate& c : candidates) {
        if ((c.addr & 0xF) != 0) Check(false, "entity base is 16-byte aligned");
        if (c.rowPtrAddr < c.addr || c.rowPtrAddr > c.addr + 0xF) {
            Check(false, "entity base is the pointer field rounded down to 16 bytes");
        }
        if (c.rowAddr == 0) Check(false, "entity candidate has a row address");
    }
    if (g_failures != t.before) DumpDiag(diag);
}

void TestCorrelateAndSnapshot() {
    TestScope t("CorrelateMotion / Snapshot");
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_entityMem);
    EntityLayout layout;
    const bool ok = CorrelateMotion(base, 0.2f, layout);
    std::printf("  diag: %s\n", layout.diag.c_str());
    Check(ok, "CorrelateMotion verifies the synthetic entity");
    Check(layout.verified, "layout.verified");
    Check(layout.posOff == kOffPos, "missilePos discovered at +0x80");
    Check(layout.velOff == kOffVel, "missileVel discovered at +0x90");
    Check(layout.targetPosOff == kOffTPos, "targetPos discovered at +0xA0");
    Check(layout.targetVelOff == kOffTVel, "targetVel discovered at +0xB0");
    Check(layout.hasTarget, "target vector found");
    const float wantSpeed = Length(kVel);
    const float wantTSpeed = Length(kTVel);
    Check(Near(layout.speed, wantSpeed, 0.30f), "observed missile speed ~ |vel|");
    Check(Near(layout.targetSpeed, wantTSpeed, 0.45f), "observed target speed ~ |targetVel|");

    // Now the entity as FindEntities reports it (base = row-pointer field rounded
    // down to 16): the layout must be discovered relative to that base and
    // Snapshot must read the same physical vectors.
    EntityCandidate cand;
    cand.addr = base + kOffRowPtr;
    cand.rowPtrAddr = base + kOffRowPtr;
    cand.rowAddr = RowAddress(0);
    cand.modelKey = "msl";
    EntityLayout shifted;
    const bool ok2 = CorrelateMotion(cand.addr, 0.2f, shifted);
    std::printf("  diag: %s\n", shifted.diag.c_str());
    Check(ok2, "CorrelateMotion verifies the entity at the reported base");
    Check(shifted.posOff == kOffPos - kOffRowPtr, "shifted missilePos at +0x40");
    Check(shifted.targetPosOff == kOffTPos - kOffRowPtr, "shifted targetPos at +0x60");

    EntitySnapshot snap;
    const bool snapOk = Snapshot(cand, shifted, snap);
    if (!snapOk) std::printf("  diag: %s\n", shifted.diag.c_str());
    Check(snapOk, "Snapshot returns a valid snapshot");
    Check(snap.valid, "snapshot.valid");
    Check(mem::IsFinite(snap.pos) && mem::IsFinite(snap.targetPos), "snapshot vectors finite");
    Check(Near(Length(snap.vel), wantSpeed, 0.05f), "snapshot missileVel == constant vel slot");
    Check(Near(Length(snap.targetVel), wantTSpeed, 0.05f), "snapshot targetVel == constant slot");
    Check(snap.pos.x > kInitPos.x, "snapshot position advanced by the motion thread");
}

void TestGuardDoesNotSwallowForeignFaults() {
    TestScope t("VEH guard declines faults outside the guarded copy");
    // Make sure the mem guard is installed before we register the last-chance
    // handler, so the handler order under test is "mem first, test last".
    uint8_t tmp[8] = {};
    (void)acz::mem::ReadBytes(0, tmp, sizeof(tmp));

    PVOID handler = AddVectoredExceptionHandler(0 /* last */, &TestLastChanceHandler);
    Check(handler != nullptr, "test last-chance handler installed");
    if (!handler) return;

    g_testHandlerCalls = 0;
    g_testCatching = 1;
    if (setjmp(g_testJb) == 0) {
        FaultOutsideGuard();
        Check(false, "fault outside a guarded copy must not be recovered by mem");
    } else {
        Check(g_testHandlerCalls == 1,
              "fault outside a guarded copy reached the test's last-chance handler");
    }
    g_testCatching = 0;
    RemoveVectoredExceptionHandler(handler);
}

}  // namespace

// ===========================================================================
// main
// ===========================================================================

// ---------------------------------------------------------------------------
// Child mode: 2-model discovery in a clean process (see TestLocate2Models).
// ---------------------------------------------------------------------------
int ChildLocate2() {
    std::printf("memtest --locate2: clean-process two-model discovery\n");
    g_tableMem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 256 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_decoyMem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_tableMem || !g_decoyMem) {
        std::printf("FATAL: VirtualAlloc failed\n");
        return 1;
    }
    BuildTable();
    BuildDecoys();

    TestScope t("2-model discovery (msl+qaam, clean process)");
    const LocateOutcome o = RunLocate({"msl", "qaam"});
    Check(o.ok, "LocateParameterTables returns true with 2 models");
    Check(o.verifiedRows >= 2, "at least 2 verified rows with 2 models");
    Check(o.allVerifiedInTable, "all verified rows are the synthetic rows");
    Check(o.decoysClean, "decoys still not verified");
    Check(o.layoutsMatch, "layout still recovered correctly");
    Check(o.valuesMatch, "values still recovered correctly");
    for (int i = 0; i < 2; ++i) {
        bool found = false;
        for (const ParamRow& r : o.result.rows) {
            if (r.verified && r.rowAddr == RowAddress(i)) found = true;
        }
        if (!found) {
            char what[96];
            std::snprintf(what, sizeof(what), "synthetic row %d (0x%llX) verified", i,
                          static_cast<unsigned long long>(RowAddress(i)));
            Check(false, what);
        }
    }
    if (g_failures != t.before) DumpDiag(o.diag);
    std::printf("\n--locate2: %d checks, %d failures -> %s\n", g_checks, g_failures,
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--locate2") == 0) return ChildLocate2();
    std::printf("memtest - ACZPropNav memory layer acceptance tests\n");
    std::printf("synthetic layout: MR+0x00 MH+0x08 LT+0x10 LR+0x18, stride 0x40\n");
    std::printf("synthetic values: msl {40,75,9,2000} qaam {75,120,9,2000} laam {150,90,12,3000}\n");
    std::printf("entities: rowptr+0x40 pos+0x80 vel+0x90 tpos+0xA0 tvel+0xB0 (advanced by a thread)\n\n");

    g_tableMem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 256 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_decoyMem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_entityMem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 3 * kEntitySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_tableMem || !g_decoyMem || !g_entityMem) {
        std::printf("FATAL: VirtualAlloc failed\n");
        return 1;
    }
    BuildTable();
    BuildDecoys();

    // ---- fake entities ---------------------------------------------------
    std::memset(g_entityMem, 0, 3 * kEntitySize);
    for (int i = 0; i < 3; ++i) {
        g_entityBase[i] = reinterpret_cast<uintptr_t>(g_entityMem) + static_cast<uintptr_t>(i) * kEntitySize;
        uint8_t* base = reinterpret_cast<uint8_t*>(g_entityBase[i]);
        const uint64_t rowPtr = static_cast<uint64_t>(RowAddress(i));
        std::memcpy(base + kOffRowPtr, &rowPtr, sizeof(rowPtr));
        const Vec3 pos = kInitPos;
        const Vec3 tpos = kInitTPos;
        std::memcpy(base + kOffPos, &pos, sizeof(pos));
        std::memcpy(base + kOffVel, &kVel, sizeof(kVel));
        std::memcpy(base + kOffTPos, &tpos, sizeof(tpos));
        std::memcpy(base + kOffTVel, &kTVel, sizeof(kTVel));
    }

    TestProcessAndModule();
    TestReadSafety();
    TestWriteSafety();
    TestScanBytes();
    TestFindFloatRuns();
    TestScanAll();
    TestRipRelativeRefs();
    TestGuardDoesNotSwallowForeignFaults();

    // Discovery is a one-shot operation per process (in the mod it runs once at
    // startup), so run it exactly once here and reuse the result for stages 2/3.
    const LocateOutcome located = RunLocate({"msl", "qaam", "laam"});
    TestLocate3Models(located);
    TestLocate2Models();

    if (!located.ok) {
        std::printf("FATAL: discovery failed, cannot test entities\n");
        DumpDiag(located.diag);
        std::printf("\nmemtest: %d checks, %d failures -> FAIL\n", g_checks, g_failures);
        return 1;
    }

    HANDLE thread = CreateThread(nullptr, 0, &MotionThread, nullptr, 0, nullptr);
    Check(thread != nullptr, "motion thread started");
    if (thread) Sleep(30);  // let the fake missiles move a little

    TestFindEntities(located.result.rows);
    TestCorrelateAndSnapshot();

    if (thread) {
        InterlockedExchange(&g_motionRun, 0);
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }

    std::printf("\nmemtest: %d checks, %d failures -> %s\n", g_checks, g_failures,
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
