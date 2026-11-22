// ACZPropNav - offline acceptance test for the x86-64 hardware data-breakpoint
// mechanism (debug registers + vectored exception handler) planned for Run B.
//
// The point of this tool is to pin down, on this exact Windows build, how a data
// breakpoint is delivered and how to continue past it. Two facts were established
// with a scratch prototype (2026-10-26):
//
//   * a 4-byte read/write watchpoint on a volatile variable IS delivered as
//     EXCEPTION_SINGLE_STEP with the slot flagged in Dr6, and the recorded Rip is
//     the faulting instruction;
//   * a data breakpoint is a FAULT: simply setting RF (EFlags bit 8) and returning
//     EXCEPTION_CONTINUE_EXECUTION re-executes the instruction and faults forever
//     (Windows then terminates the process with 0x80000004).
//
// Therefore the shipping design arms one-shot: the VEH records the hit, clears
// Dr0..Dr3/Dr7 in the thread context and continues; the probe re-arms from its tick
// after a short delay. This tool verifies exactly that scheme:
//
//   1. arm read-watch; a read hits exactly once, the read value is correct, the
//      breakpoint has been disarmed automatically;
//   2. re-arm; the next read hits again (re-arm works after a one-shot hit);
//   3. write watchpoint: the write completes and hits once;
//   4. two slots: hits are attributed to the right slot through Dr6;
//   5. after disarm nothing fires;
//   6. Rip of a hit lies inside the small function that performs the read.
//
// Exit status: 0 all checks pass, 1 otherwise.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) {
        std::printf("    ok    %s\n", what);
    } else {
        std::printf("    FAIL  %s\n", what);
        ++g_failures;
    }
}

volatile uint32_t g_watchA = 0x11223344u;
volatile uint32_t g_watchB = 0x55667788u;

constexpr int kMaxHits = 64;
struct Hit {
    volatile LONG slot;
    volatile uintptr_t rip;
    volatile uint64_t dr6;
};
Hit g_hits[kMaxHits];
volatile LONG g_hitCount = 0;
volatile LONG g_armed = 0;

void ResetHits() {
    for (auto& h : g_hits) {
        h.slot = -1;
        h.rip = 0;
        h.dr6 = 0;
    }
    g_hitCount = 0;
}

// One-shot data-breakpoint handler: record, then clear every debug register in the
// Returning context so the faulting instruction can execute normally.
LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    if (!g_armed) return EXCEPTION_CONTINUE_SEARCH;
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!c) return EXCEPTION_CONTINUE_SEARCH;

    const uint64_t dr6 = static_cast<uint64_t>(c->Dr6);
    int slot = -1;
    for (int i = 0; i < 4; ++i) {
        if (dr6 & (1ull << i)) {
            slot = i;
            break;
        }
    }
    const LONG idx = InterlockedIncrement(&g_hitCount) - 1;
    if (idx >= 0 && idx < kMaxHits) {
        g_hits[idx].slot = slot;
        g_hits[idx].rip = static_cast<uintptr_t>(c->Rip);
        g_hits[idx].dr6 = dr6;
    }
    c->Dr0 = 0;
    c->Dr1 = 0;
    c->Dr2 = 0;
    c->Dr3 = 0;
    c->Dr6 = 0;
    c->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct Slot {
    uintptr_t addr;
    uint32_t rw;   // 1 = write, 3 = read/write
    uint32_t len;  // 1, 2, 4, 8
};

bool ArmLocal(const Slot* slots, int n) {
    if (n < 0 || n > 4) return false;
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL;
    if (!GetThreadContext(GetCurrentThread(), &ctx)) return false;
    uint64_t dr[4] = {0, 0, 0, 0};
    uint64_t dr7 = 0;
    for (int i = 0; i < n; ++i) {
        dr[i] = static_cast<uint64_t>(slots[i].addr);
        uint32_t lenBits = 0;
        switch (slots[i].len) {
            case 1: lenBits = 0b00; break;
            case 2: lenBits = 0b01; break;
            case 4: lenBits = 0b11; break;
            case 8: lenBits = 0b10; break;
            default: return false;
        }
        const uint32_t rwBits = (slots[i].rw == 1) ? 0b01 : 0b11;
        dr7 |= 1ull << (2 * i);
        dr7 |= static_cast<uint64_t>(rwBits) << (16 + 4 * i);
        dr7 |= static_cast<uint64_t>(lenBits) << (18 + 4 * i);
    }
    ctx.Dr0 = dr[0];
    ctx.Dr1 = dr[1];
    ctx.Dr2 = dr[2];
    ctx.Dr3 = dr[3];
    ctx.Dr6 = 0;
    ctx.Dr7 = dr7;
    if (!SetThreadContext(GetCurrentThread(), &ctx)) return false;
    return true;
}

bool DisarmLocal() { return ArmLocal(nullptr, 0); }

__declspec(noinline) uint32_t ReadA() { return g_watchA; }
__declspec(noinline) uint32_t ReadB() { return g_watchB; }
__declspec(noinline) void WriteA(uint32_t v) { g_watchA = v; }

// ---- cross-thread variant (this is what Run B actually does) ---------------
// Debug registers are thread state, so the shipping probe suspends every other
// thread and installs Dr0..Dr3 through SetThreadContext.  That path is different
// from the local one above and has to be proven too.
volatile uint32_t g_watchC = 0xDEADBEEFu;
volatile LONG g_workerGo = 0;
volatile LONG g_workerDone = 0;

__declspec(noinline) uint32_t ReadC() { return g_watchC; }

DWORD WINAPI WorkerProc(LPVOID) {
    while (InterlockedCompareExchange(&g_workerGo, 0, 0) == 0) ::Sleep(1);
    const uint32_t v = ReadC();  // the watched read, on a different thread
    ::InterlockedExchange(&g_workerDone, static_cast<LONG>(v));
    return 0;
}

bool ArmRemote(HANDLE thread, const Slot* slots, int n) {
    if (thread == nullptr || n < 0 || n > 4) return false;
    alignas(16) CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL;
    if (!::GetThreadContext(thread, &ctx)) return false;
    uint64_t dr[4] = {0, 0, 0, 0};
    uint64_t dr7 = 0;
    for (int i = 0; i < n; ++i) {
        dr[i] = static_cast<uint64_t>(slots[i].addr);
        uint32_t lenBits = 0;
        switch (slots[i].len) {
            case 1: lenBits = 0b00; break;
            case 2: lenBits = 0b01; break;
            case 4: lenBits = 0b11; break;
            case 8: lenBits = 0b10; break;
            default: return false;
        }
        const uint32_t rwBits = (slots[i].rw == 1) ? 0b01 : 0b11;
        dr7 |= 1ull << (2 * i);
        dr7 |= static_cast<uint64_t>(rwBits) << (16 + 4 * i);
        dr7 |= static_cast<uint64_t>(lenBits) << (18 + 4 * i);
    }
    ctx.Dr0 = dr[0];
    ctx.Dr1 = dr[1];
    ctx.Dr2 = dr[2];
    ctx.Dr3 = dr[3];
    ctx.Dr6 = 0;
    ctx.Dr7 = dr7;
    return ::SetThreadContext(thread, &ctx) != FALSE;
}

bool DisarmRemote(HANDLE thread) { return ArmRemote(thread, nullptr, 0); }

void PrintHits(const char* tag) {
    const LONG n = g_hitCount > kMaxHits ? kMaxHits : g_hitCount;
    std::printf("      %s: %ld hit(s)\n", tag, static_cast<long>(n));
    for (LONG i = 0; i < n; ++i) {
        std::printf("        hit %ld: slot=%ld rip=0x%llX dr6=0x%llX\n", static_cast<long>(i),
                    static_cast<long>(g_hits[i].slot),
                    static_cast<unsigned long long>(g_hits[i].rip),
                    static_cast<unsigned long long>(g_hits[i].dr6));
    }
}

int Run() {
    std::printf("ACZPropNav hwwatchtest (hardware data breakpoints, one-shot VEH)\n\n");
    const PVOID veh = AddVectoredExceptionHandler(1, &WatchVeh);
    Check(veh != nullptr, "AddVectoredExceptionHandler succeeds");

    // ---- 1. read watchpoint: one hit, value correct, auto-disarm -------------
    std::printf("-- read watchpoint (4 bytes, read/write, one-shot) --\n");
    ResetHits();
    const uint32_t a0 = g_watchA;  // no breakpoint yet
    const Slot readSlot[1] = {{reinterpret_cast<uintptr_t>(&g_watchA), 3, 4}};
    Check(ArmLocal(readSlot, 1), "SetThreadContext(DR0/DR7) succeeds");
    InterlockedExchange(&g_armed, 1);
    const uint32_t a1 = ReadA();
    InterlockedExchange(&g_armed, 0);
    PrintHits("read");
    Check(a0 == 0x11223344u && a1 == 0x11223344u, "watched read returns the correct value");
    Check(g_hitCount == 1, "exactly one #DB for the first watched read");
    Check(g_hits[0].slot == 0, "Dr6 identifies slot 0");

    // ---- 2. re-arm after the one-shot hit ----------------------------------
    ResetHits();
    Check(ArmLocal(readSlot, 1), "re-arm succeeds");
    InterlockedExchange(&g_armed, 1);
    const uint32_t a2 = ReadA();
    InterlockedExchange(&g_armed, 0);
    PrintHits("re-arm");
    Check(a2 == 0x11223344u, "read after re-arm returns the correct value");
    Check(g_hitCount == 1, "re-armed breakpoint hits again");
    {
        bool inFunc = false;
        const uintptr_t lo = reinterpret_cast<uintptr_t>(&ReadA);
        const uintptr_t hi = lo + 0x100;
        if (g_hits[0].rip >= lo && g_hits[0].rip < hi) inFunc = true;
        Check(inFunc, "recorded Rip lies inside the reading function (fault semantics)");
    }

    // ---- 3. write watchpoint ------------------------------------------------
    std::printf("-- write watchpoint (4 bytes, write) --\n");
    ResetHits();
    const Slot writeSlot[1] = {{reinterpret_cast<uintptr_t>(&g_watchA), 1, 4}};
    Check(ArmLocal(writeSlot, 1), "SetThreadContext(write watchpoint) succeeds");
    InterlockedExchange(&g_armed, 1);
    WriteA(0xAABBCCDDu);
    InterlockedExchange(&g_armed, 0);
    PrintHits("write");
    Check(g_watchA == 0xAABBCCDDu, "watched write completed");
    Check(g_hitCount == 1, "write watchpoint delivers exactly one #DB");

    // ---- 4. two slots -------------------------------------------------------
    std::printf("-- two slots --\n");
    const Slot two[2] = {{reinterpret_cast<uintptr_t>(&g_watchA), 3, 4},
                         {reinterpret_cast<uintptr_t>(&g_watchB), 3, 4}};
    ResetHits();
    Check(ArmLocal(two, 2), "SetThreadContext(two watchpoints) succeeds");
    InterlockedExchange(&g_armed, 1);
    const uint32_t b1 = ReadB();
    InterlockedExchange(&g_armed, 0);
    PrintHits("slot B");
    Check(b1 == 0x55667788u, "slot B read returns the correct value");
    Check(g_hitCount == 1 && g_hits[0].slot == 1, "Dr6 attributes the hit to slot 1");
    ResetHits();
    Check(ArmLocal(two, 2), "re-arm two slots");
    InterlockedExchange(&g_armed, 1);
    const uint32_t a3 = ReadA();
    InterlockedExchange(&g_armed, 0);
    PrintHits("slot A");
    Check(a3 == 0xAABBCCDDu, "slot A read returns the correct value");
    Check(g_hitCount == 1 && g_hits[0].slot == 0, "Dr6 attributes the hit to slot 0");

    // ---- 5. disarm ----------------------------------------------------------
    std::printf("-- disarm --\n");
    Check(DisarmLocal(), "SetThreadContext(Dr7=0) succeeds");
    ResetHits();
    InterlockedExchange(&g_armed, 1);
    const uint32_t a4 = ReadA();
    InterlockedExchange(&g_armed, 0);
    Check(a4 == 0xAABBCCDDu, "read still works after disarm");
    Check(g_hitCount == 0, "no #DB after disarm");

    // ---- 6. cross-thread arming (what Run B really does) --------------------
    std::printf("-- cross-thread: suspend -> set DR0 -> resume -> hit on the other thread --\n");
    ResetHits();
    ::InterlockedExchange(&g_armed, 0);
    ::InterlockedExchange(&g_workerGo, 0);
    ::InterlockedExchange(&g_workerDone, 0);
    HANDLE worker = ::CreateThread(nullptr, 0, &WorkerProc, nullptr, CREATE_SUSPENDED, nullptr);
    Check(worker != nullptr, "create a suspended worker thread");
    if (worker != nullptr) {
        const Slot cSlot[1] = {{reinterpret_cast<uintptr_t>(&g_watchC), 3, 4}};
        Check(ArmRemote(worker, cSlot, 1), "SetThreadContext(worker thread, Dr0/Dr7) succeeds");
        ::InterlockedExchange(&g_armed, 1);
        ::ResumeThread(worker);
        ::InterlockedExchange(&g_workerGo, 1);
        for (int i = 0; i < 400 && ::InterlockedCompareExchange(&g_workerDone, 0, 0) == 0; ++i) {
            ::Sleep(5);
        }
        ::InterlockedExchange(&g_armed, 0);
        const LONG v = ::InterlockedCompareExchange(&g_workerDone, 0, 0);
        PrintHits("cross-thread");
        Check(v == static_cast<LONG>(0xDEADBEEFu),
              "the other thread read the watched value correctly");
        Check(g_hitCount == 1, "exactly one #DB was delivered on the other thread");
        Check(g_hits[0].slot == 0, "Dr6 attributes the remote hit to slot 0");
        bool inReadC = false;
        {
            const uintptr_t lo = reinterpret_cast<uintptr_t>(&ReadC);
            const uintptr_t hi = lo + 0x100;
            if (g_hits[0].rip >= lo && g_hits[0].rip < hi) inReadC = true;
        }
        Check(inReadC, "recorded Rip lies inside the reading function on the other thread");
        DisarmRemote(worker);
        ::WaitForSingleObject(worker, 2000);
        ::CloseHandle(worker);
    }

    if (veh) RemoveVectoredExceptionHandler(veh);
    std::printf("\n==============================================\n");
    std::printf("hwwatchtest: %d checks, %d failure(s)\n", g_checks, g_failures);
    std::printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return Run();
}
