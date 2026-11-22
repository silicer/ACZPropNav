// ACZPropNav - offline acceptance tests for the inline hook engine and the
// XINPUT1_4 proxy loader.
//
// Everything here runs in one process, deterministically, in well under a
// second of real work:
//   1. hook a normal function, verify detour + trampoline, Remove, re-install
//   2. 128 install/remove cycles (both patch encodings), leak check
//   3. refusal: prologue starting with a RIP-relative instruction
//   4. InstructionLength table (34 hand-written encodings)
//   5. proxy smoke test: load build\XINPUT1_4.dll, forward XInputGetState
//   6. real prologues of the EFF_MIS_POSDIR ring writers (plain-MSL survey)
//   7. real prologues of the Run A probe hook targets + the documented refusal
//   8. PASS/FAIL summary, exit code 1 on any failure
//
// Exit status: 0 = all passed, 1 = at least one check failed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hook/hook.h"
#include "pn/msl_pn.h"

// Global (external linkage) on purpose: TargetRipRel's hand-written assembly
// references it by its raw symbol name.
volatile int g_refuse_global = 1234;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_testOk = true;

// ---- check helpers -------------------------------------------------------
void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) {
        std::printf("    ok    %s\n", what);
    } else {
        g_testOk = false;
        ++g_failures;
        std::printf("    FAIL  %s\n", what);
    }
}

// Function pointer -> void*, for handing code addresses to Install/Probe.
template <typename Fn>
void* AsVoid(Fn fn) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fn));
}

void DumpBytes(const char* tag, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    std::printf("       %s:", tag);
    for (size_t i = 0; i < n; ++i) std::printf(" %02X", b[i]);
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Test targets. These are hand-written machine code so that the tests are
// independent of what the optimiser decides to emit: they are still ordinary
// functions in this executable, called through function pointers.
// ---------------------------------------------------------------------------

// 14+ byte relative-free prologue -> Install must pick the 14-byte absolute
// jump form. Returns a+b.
//   push rbx / push rbp / sub rsp,0x28 / mov rcx,rbx / mov rdx,rbp /
//   mov ebx,eax / add ebp,eax / add rsp,0x28 / pop rbp / pop rbx / ret
//   = 1+1+4+3+3+2 = exactly 14 bytes before the first "real" instruction.
extern "C" __attribute__((naked, noinline)) int Target14(int, int) {
    __asm__ __volatile__(
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "subq $0x28, %rsp\n\t"
        "movq %rcx, %rbx\n\t"
        "movq %rdx, %rbp\n\t"
        "movl %ebx, %eax\n\t"
        "addl %ebp, %eax\n\t"
        "addq $0x28, %rsp\n\t"
        "popq %rbp\n\t"
        "popq %rbx\n\t"
        "ret\n\t");
}

// Tiny 5-byte prologue -> Install can only use the E9 rel32 form (the detour
// lives in this module, well inside +-2 GB). Returns a+b.
//   mov eax,ecx / add eax,edx / ret
extern "C" __attribute__((naked, noinline)) int Target5(int, int) {
    __asm__ __volatile__(
        "movl %ecx, %eax\n\t"
        "addl %edx, %eax\n\t"
        "ret\n\t");
}

// Refusal target: the very first instruction is RIP-relative
// (mov eax, [rip+disp32]), which cannot be relocated verbatim. It must be
// refused -- and must still work afterwards.
extern "C" __attribute__((naked, noinline)) int TargetRipRel(void) {
    __asm__ __volatile__(
        "movl g_refuse_global(%rip), %eax\n\t"
        "ret\n\t");
}

// Indirect call sites (volatile pointers: the compiler cannot inline or
// constant-fold through them, so every call really goes to the target's
// current entry point).
int (*volatile g_callTarget14)(int, int) = &Target14;
int (*volatile g_callTarget5)(int, int) = &Target5;
int (*volatile g_callRipRel)(void) = &TargetRipRel;

// ---- detours -------------------------------------------------------------
int (*g_tramp14)(int, int) = nullptr;
int (*g_tramp5)(int, int) = nullptr;
int g_hits14 = 0;
int g_hits5 = 0;
int g_tramp14_result = -1;
int g_tramp5_result = -1;

extern "C" __attribute__((noinline)) int Detour14(int a, int b) {
    ++g_hits14;
    if (g_tramp14 != nullptr) g_tramp14_result = g_tramp14(a, b);
    return 4242;
}

extern "C" __attribute__((noinline)) int Detour5(int a, int b) {
    ++g_hits5;
    if (g_tramp5 != nullptr) g_tramp5_result = g_tramp5(a, b);
    return 7777;
}

// ---------------------------------------------------------------------------
// 1. Hook a normal function: detour is called, trampoline still runs the
//    original, Remove restores it, Install works again.
// ---------------------------------------------------------------------------
bool Test1() {
    g_testOk = true;
    std::printf("[1/9] hook a normal function / trampoline / Remove / re-install\n");

    DumpBytes("Target14 prologue", AsVoid(&Target14), 16);

    size_t probeStolen = 0;
    std::string probeWhy;
    const bool probed =
        acz::hook::Probe(AsVoid(&Target14), probeStolen, probeWhy);
    Check(probed, "Probe(Target14) succeeds");
    if (!probeWhy.empty()) std::printf("       probe: %s\n", probeWhy.c_str());
    Check(probeStolen == 14, "Probe reports 14 stolen bytes");

    acz::hook::InstallResult r;
    void* tramp = nullptr;
    std::string whyNot;
    g_hits14 = 0;
    g_tramp14_result = -1;
    const bool installed =
        acz::hook::Install(AsVoid(&Target14), AsVoid(&Detour14), &tramp, r, &whyNot);
    Check(installed, "Install(Target14 -> Detour14)");
    if (!installed) {
        std::printf("       reason: %s\n", r.reason.c_str());
        return g_testOk;
    }
    std::printf("       install: %s, stolen=%zu\n", r.reason.c_str(), r.stolenBytes);
    Check(tramp != nullptr, "trampoline pointer is set");
    Check(r.ok, "InstallResult::ok");
    Check(r.stolenBytes == 14, "14 bytes stolen (absolute form needs 14)");
    Check(r.reason.find("absolute") != std::string::npos,
          "absolute 14-byte jump form was used");
    Check(r.original.size() == 14, "original prologue bytes were saved");

    g_tramp14 = reinterpret_cast<int (*)(int, int)>(tramp);
    Check(g_callTarget14(2, 3) == 4242, "call through the patched entry hits the detour");
    Check(g_hits14 == 1, "detour was called exactly once");
    Check(g_tramp14_result == 5, "detour sees the trampoline return the original 5");
    Check(g_tramp14(4, 5) == 9, "direct trampoline call returns the original 9");
    Check(g_tramp14(0, 0) == 0, "direct trampoline call returns the original 0");
    Check(g_hits14 == 1, "trampoline calls do not re-enter the detour");

    // A second Install on a live hook must be refused, not corrupt anything.
    acz::hook::InstallResult r2;
    void* tramp2 = reinterpret_cast<void*>(0x1);
    std::string why2;
    Check(!acz::hook::Install(AsVoid(&Target14), AsVoid(&Detour14), &tramp2, r2, &why2),
          "second Install while hooked fails cleanly");
    Check(tramp2 == nullptr, "failed re-install leaves *trampoline null");
    Check(why2.find("already") != std::string::npos, "reason mentions it is already hooked");
    Check(g_callTarget14(2, 3) == 4242, "hook still works after the refused re-install");

    // Remove
    g_tramp14 = nullptr;
    Check(acz::hook::Remove(AsVoid(&Target14), r), "Remove(Target14)");
    Check(g_callTarget14(2, 3) == 5, "original behaviour restored after Remove");
    Check(std::memcmp(AsVoid(&Target14), r.original.data(), r.original.size()) == 0,
          "prologue bytes restored exactly");
    Check(!acz::hook::Remove(AsVoid(&Target14), r), "second Remove is a safe no-op");

    // Re-install (must work again)
    acz::hook::InstallResult r3;
    void* tramp3 = nullptr;
    std::string why3;
    g_hits14 = 0;
    Check(acz::hook::Install(AsVoid(&Target14), AsVoid(&Detour14), &tramp3, r3, &why3),
          "re-install after Remove");
    if (tramp3 != nullptr) {
        g_tramp14 = reinterpret_cast<int (*)(int, int)>(tramp3);
        Check(g_callTarget14(10, 20) == 4242, "re-installed hook is active");
        Check(g_hits14 == 1, "re-installed detour hit exactly once");
        Check(g_tramp14_result == 30, "re-installed trampoline returns the original 30");
        g_tramp14 = nullptr;
        Check(acz::hook::Remove(AsVoid(&Target14), r3), "final Remove");
        Check(g_callTarget14(10, 20) == 30, "original behaviour restored again");
    }
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 2. Many install/remove cycles, alternating both patch encodings.
// ---------------------------------------------------------------------------
size_t CountPrivateCommitted() {
    size_t total = 0;
    uintptr_t addr = 0x10000;
    const uintptr_t kMaxUser = 0x7FFFFFFF0000ull;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < kMaxUser) {
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) break;
        if (mbi.RegionSize == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) total += mbi.RegionSize;
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return total;
}

bool Test2() {
    g_testOk = true;
    std::printf("[2/9] 128 install/remove cycles, both jump encodings\n");

    const int kCycles = 128;
    const size_t baseline = CountPrivateCommitted();
    int bad = 0;
    int absForms = 0;
    int relForms = 0;
    g_hits14 = 0;
    g_hits5 = 0;
    g_tramp14_result = -1;
    g_tramp5_result = -1;
    g_tramp14 = nullptr;
    g_tramp5 = nullptr;

    for (int i = 0; i < kCycles; ++i) {
        const bool use14 = (i % 2) == 0;
        void* target = use14 ? AsVoid(&Target14) : AsVoid(&Target5);
        void* detour = use14 ? AsVoid(&Detour14) : AsVoid(&Detour5);
        const int expectedDetour = use14 ? 4242 : 7777;
        int (*call)(int, int) = use14 ? g_callTarget14 : g_callTarget5;

        acz::hook::InstallResult r;
        void* tramp = nullptr;
        std::string why;
        if (!acz::hook::Install(target, detour, &tramp, r, &why)) {
            if (bad < 8)
                std::printf("       cycle %d (%s): install failed: %s\n", i,
                            use14 ? "14-byte" : "5-byte", why.c_str());
            ++bad;
            continue;
        }
        if (use14) {
            ++absForms;
            g_tramp14 = reinterpret_cast<int (*)(int, int)>(tramp);
            if (r.stolenBytes != 14) {
                if (bad < 8)
                    std::printf("       cycle %d: stolen=%zu (want 14), reason='%s'\n", i,
                                r.stolenBytes, r.reason.c_str());
                ++bad;
            }
        } else {
            ++relForms;
            g_tramp5 = reinterpret_cast<int (*)(int, int)>(tramp);
            if (r.stolenBytes != 5) {
                if (bad < 8)
                    std::printf("       cycle %d: stolen=%zu (want 5), reason='%s'\n", i,
                                r.stolenBytes, r.reason.c_str());
                ++bad;
            }
            if (r.reason.find("rel32") == std::string::npos) {
                if (bad < 8)
                    std::printf("       cycle %d: reason '%s' does not mention rel32\n", i,
                                r.reason.c_str());
                ++bad;
            }
        }

        const int patched = call(3, 4);
        if (patched != expectedDetour) {
            if (bad < 8)
                std::printf("       cycle %d: patched call returned %d (want %d)\n", i, patched,
                            expectedDetour);
            ++bad;
        }
        const int viaTramp = reinterpret_cast<int (*)(int, int)>(tramp)(3, 4);
        if (viaTramp != 7) {
            if (bad < 8)
                std::printf("       cycle %d: trampoline returned %d (want 7)\n", i, viaTramp);
            ++bad;
        }
        g_tramp14 = nullptr;
        g_tramp5 = nullptr;
        if (!acz::hook::Remove(target, r)) {
            if (bad < 8) std::printf("       cycle %d: Remove failed\n", i);
            ++bad;
        }
        const int restored = call(3, 4);
        if (restored != 7) {
            if (bad < 8)
                std::printf("       cycle %d: restored call returned %d (want 7)\n", i, restored);
            ++bad;
        }
    }

    Check(bad == 0, "all 128 cycles behaved correctly");
    Check(absForms == 64, "64 cycles used the 14-byte absolute form");
    Check(relForms == 64, "64 cycles used the 5-byte rel32 form");
    Check(g_hits14 == 64, "detour 14 entered once per absolute-form cycle");
    Check(g_hits5 == 64, "detour 5 entered once per rel32-form cycle");
    Check(g_tramp14_result == 7 && g_tramp5_result == 7,
          "both detours saw the trampoline return the original result");

    const size_t after = CountPrivateCommitted();
    if (after > baseline) {
        std::printf("       private commit: before=%zu after=%zu (+%zu)\n", baseline, after,
                    after - baseline);
    } else {
        std::printf("       private commit: before=%zu after=%zu\n", baseline, after);
    }
    Check(after <= baseline + 0x10000,
          "no executable pages leaked (private commit baseline restored)");
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 3. Refusal: RIP-relative first instruction.
// ---------------------------------------------------------------------------
bool Test3() {
    g_testOk = true;
    std::printf("[3/9] refusal of a RIP-relative prologue\n");

    const uint8_t* const code = static_cast<const uint8_t*>(AsVoid(&TargetRipRel));
    DumpBytes("TargetRipRel prologue", code, 8);

    // Hand-verified with objdump before writing this assertion:
    //   mov eax, dword ptr [rip + disp32]   -> 8B 05 xx xx xx xx
    //   ret                                 -> C3
    const bool isRipRel = code[0] == 0x8B && code[1] == 0x05;
    Check(isRipRel, "target really starts with 8B 05 (mov eax,[rip+disp32])");
    Check(acz::hook::InstructionLength(code, 15) == 6, "its first instruction is 6 bytes");
    Check(!acz::hook::InstructionRelocatable(code, 6), "first instruction is not relocatable");

    size_t probeStolen = 99;
    std::string probeWhy;
    Check(!acz::hook::Probe(code, probeStolen, probeWhy),
          "Probe refuses the RIP-relative prologue");
    Check(probeStolen == 0, "Probe reports 0 stolen bytes on refusal");
    Check(!probeWhy.empty(), "Probe explains why");
    std::printf("       probe: %s\n", probeWhy.c_str());

    uint8_t before[16];
    std::memcpy(before, code, sizeof(before));

    acz::hook::InstallResult r;
    void* tramp = reinterpret_cast<void*>(0x1);
    std::string whyNot;
    Check(!acz::hook::Install(AsVoid(&TargetRipRel), AsVoid(&Detour14), &tramp, r, &whyNot),
          "Install fails on the RIP-relative prologue");
    Check(tramp == nullptr, "failed Install nulls *trampoline");
    Check(!r.ok, "failed InstallResult::ok == false");
    Check(!r.reason.empty(), "failed Install reports a precise reason");
    Check(r.original.empty() || r.stolenBytes == 0, "failed Install stole nothing");
    std::printf("       install: %s\n", r.reason.c_str());
    Check(std::memcmp(before, code, sizeof(before)) == 0,
          "target bytes untouched by the failed Install");

    Check(g_callRipRel() == 1234, "function still works after the refused hook");
    Check(acz::hook::InstructionRelocatable(static_cast<const uint8_t*>(AsVoid(&Target5)), 2),
          "the first instruction of a plain prologue is reported relocatable");
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 4. InstructionLength / InstructionRelocatable table.
// ---------------------------------------------------------------------------
struct LenCase {
    uint8_t bytes[16];  // zero padded; only `len` bytes are examined
    size_t len;         // bytes available (the maxLen handed to the decoder)
    size_t expect;      // expected instruction length (0 = unsupported/truncated)
    int reloc;          // 1 = must be relocatable, 0 = must not, -1 = don't care
    const char* label;
};

const LenCase kLenCases[] = {
    // --- prologue shapes that appear in real x64 code ---
    {{0x48, 0x89, 0x5C, 0x24, 0x08}, 5, 5, 1, "mov [rsp+8], rbx"},
    {{0x48, 0x83, 0xEC, 0x28}, 4, 4, 1, "sub rsp, 0x28"},
    {{0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00}, 7, 7, 1, "sub rsp, 0x100"},
    {{0x48, 0x89, 0xE5}, 3, 3, 1, "mov rbp, rsp"},
    {{0x40, 0x53}, 2, 2, 1, "push rbx (REX)"},
    {{0x55}, 1, 1, 1, "push rbp"},
    {{0xC3}, 1, 1, 1, "ret"},
    {{0x48, 0x8B, 0x44, 0x24, 0x10}, 5, 5, 1, "mov rax, [rsp+0x10]"},
    {{0x48, 0x83, 0x7C, 0x24, 0x08, 0x00}, 6, 6, 1, "cmp qword [rsp+8], 0"},
    {{0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}, 6, 6, 1, "nop word [rax+rax]"},
    {{0x0F, 0x1F, 0x40, 0x00}, 4, 4, 1, "nop dword [rax+0]"},
    {{0xF3, 0x0F, 0x1E, 0xFA}, 4, 4, 1, "endbr64"},
    {{0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, 10, 10, 1,
     "mov rax, imm64"},
    {{0xB8, 0x01, 0x00, 0x00, 0x00}, 5, 5, 1, "mov eax, 1"},
    {{0x6A, 0x00}, 2, 2, 1, "push 0"},
    {{0xC7, 0x00, 0x01, 0x00, 0x00, 0x00}, 6, 6, 1, "mov dword [rax], 1"},
    {{0xF6, 0xC3, 0x01}, 3, 3, 1, "test bl, 1"},
    {{0xF7, 0xC3, 0x01, 0x00, 0x00, 0x00}, 6, 6, 1, "test ebx, 1"},
    {{0x0F, 0xB6, 0xC0}, 3, 3, 1, "movzx eax, al"},
    {{0xD9, 0x44, 0x24, 0x04}, 4, 4, 1, "fld dword [rsp+4]"},
    {{0xC5, 0xF8, 0x77}, 3, 3, 1, "vzeroupper (VEX)"},
    {{0xCD, 0x29}, 2, 2, 1, "int 0x29"},
    {{0x48, 0x63, 0xC1}, 3, 3, 1, "movsxd rax, ecx"},
    // --- relative control flow: decodable but NOT relocatable ---
    {{0xE8, 0x00, 0x00, 0x00, 0x00}, 5, 5, 0, "call rel32"},
    {{0xE9, 0x00, 0x00, 0x00, 0x00}, 5, 5, 0, "jmp rel32"},
    {{0xEB, 0xFE}, 2, 2, 0, "jmp rel8"},
    {{0x74, 0x05}, 2, 2, 0, "jz rel8"},
    {{0x0F, 0x84, 0x00, 0x00, 0x00, 0x00}, 6, 6, 0, "jz rel32"},
    {{0xE2, 0xFE}, 2, 2, 0, "loop rel8"},
    // --- RIP-relative memory operands: decodable but NOT relocatable ---
    {{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00}, 7, 7, 0, "mov rax, [rip+disp32]"},
    {{0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00}, 7, 7, 0, "lea rax, [rip+disp32]"},
    {{0x48, 0x89, 0x0D, 0x10, 0x00, 0x00, 0x00}, 7, 7, 0, "mov [rip+0x10], rcx"},
    {{0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}, 6, 6, 0, "jmp [rip+0]"},
    // --- truncated / unsupported: must return 0 ---
    {{0x48}, 1, 0, -1, "truncated: lone REX"},
    {{0x48, 0x89}, 2, 0, -1, "truncated: mov r/m64 without ModRM"},
    {{0xE8, 0x00, 0x00}, 3, 0, -1, "truncated: call rel32 missing bytes"},
    {{0x0F, 0x84, 0x00, 0x00}, 4, 0, -1, "truncated: jz rel32 missing bytes"},
    {{0x62}, 1, 0, -1, "unsupported: EVEX prefix"},
    {{0x0F}, 1, 0, -1, "truncated: lone 0F escape"},
};

bool Test4() {
    g_testOk = true;
    std::printf("[4/9] InstructionLength / InstructionRelocatable table\n");

    int bad = 0;
    int cases = 0;
    for (const LenCase& c : kLenCases) {
        ++cases;
        const size_t got = acz::hook::InstructionLength(c.bytes, c.len);
        bool ok = (got == c.expect);
        if (ok && c.reloc >= 0) {
            const bool reloc = acz::hook::InstructionRelocatable(c.bytes, c.expect);
            ok = (reloc == (c.reloc == 1));
        }
        if (!ok) ++bad;
        std::printf("    %s  %-38s len=%zu want=%zu\n", ok ? "ok  " : "FAIL", c.label, got,
                    c.expect);
    }
    Check(cases >= 30, "table has at least 30 hand-written encodings");
    Check(bad == 0, "every length / relocatability case matches");

    // One instruction, but a longer buffer: must report the instruction length.
    const uint8_t padded[16] = {0x55};
    Check(acz::hook::InstructionLength(padded, sizeof(padded)) == 1,
          "InstructionLength reports the instruction, not the buffer size");
    Check(acz::hook::InstructionLength(nullptr, 8) == 0, "null code returns 0");
    Check(acz::hook::InstructionLength(padded, 0) == 0, "zero maxLen returns 0");
    Check(!acz::hook::InstructionRelocatable(padded, 2),
          "InstructionRelocatable rejects a length that is not one instruction");
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 5. Proxy smoke test.
// ---------------------------------------------------------------------------
void JoinPathW(wchar_t* out, DWORD cch, const wchar_t* a, const wchar_t* b) {
    lstrcpynW(out, a, static_cast<int>(cch));
    const int len = lstrlenW(out);
    if (static_cast<DWORD>(len) + 1 < cch) {
        lstrcpynW(out + len, L"\\", static_cast<int>(cch - static_cast<DWORD>(len)));
    }
    const int len2 = lstrlenW(out);
    if (static_cast<DWORD>(len2) < cch) {
        lstrcpynW(out + len2, b, static_cast<int>(cch - static_cast<DWORD>(len2)));
    }
}

// Directory that contains this executable (build\).
bool ExeDir(wchar_t* out, DWORD cch) {
    if (GetModuleFileNameW(nullptr, out, cch) == 0) return false;
    wchar_t* slash = nullptr;
    for (wchar_t* p = out; *p != L'\0'; ++p)
        if (*p == L'\\' || *p == L'/') slash = p;
    if (slash == nullptr) return false;
    *slash = L'\0';
    return true;
}

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);

bool Test5() {
    g_testOk = true;
    std::printf("[5/9] proxy smoke test: XINPUT1_4.dll forwards to the real DLL\n");

    wchar_t exeDir[MAX_PATH] = {};
    Check(ExeDir(exeDir, MAX_PATH), "locate the test executable's directory");
    if (exeDir[0] == L'\0') return false;

    // Load an *isolated copy* of the proxy so that its bootstrap cannot pull
    // ACZPropNav.dll (which lives next to it in build\) into this process:
    // the test must exercise the "mod absent" path, which has to stay silent
    // and harmless.
    wchar_t srcXInput[MAX_PATH] = {};
    wchar_t isoDir[MAX_PATH] = {};
    wchar_t isoXInput[MAX_PATH] = {};
    JoinPathW(srcXInput, MAX_PATH, exeDir, L"XINPUT1_4.dll");
    JoinPathW(isoDir, MAX_PATH, exeDir, L"proxy_iso");
    JoinPathW(isoXInput, MAX_PATH, isoDir, L"XINPUT1_4.dll");
    CreateDirectoryW(isoDir, nullptr);
    if (GetFileAttributesW(srcXInput) == INVALID_FILE_ATTRIBUTES) {
        std::printf("       %ls is missing -- build the proxy first:\n", srcXInput);
        std::printf("       make build/XINPUT1_4.dll\n");
    }
    const bool copied = CopyFileW(srcXInput, isoXInput, FALSE) != FALSE;
    if (!copied) std::printf("       (copy failed, loading the proxy in place)\n");

    const wchar_t* const proxyPath = copied ? isoXInput : srcXInput;
    std::printf("       loading: %ls\n", proxyPath);
    HMODULE proxy = LoadLibraryW(proxyPath);
    if (proxy == nullptr) {
        std::printf("       LoadLibraryW failed, GetLastError=%lu\n", GetLastError());
    }
    Check(proxy != nullptr, "LoadLibraryW(build\\XINPUT1_4.dll) succeeds");
    if (proxy == nullptr) return false;

    typedef DWORD(WINAPI * GetStateFn)(DWORD, void*);
    typedef DWORD(WINAPI * SetStateFn)(DWORD, void*);
    GetStateFn getState =
        reinterpret_cast<GetStateFn>(GetProcAddress(proxy, "XInputGetState"));
    SetStateFn setState =
        reinterpret_cast<SetStateFn>(GetProcAddress(proxy, "XInputSetState"));

    Check(getState != nullptr, "proxy exports XInputGetState by name");
    Check(setState != nullptr, "proxy exports XInputSetState by name (probe only)");
    Check(GetProcAddress(proxy, reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(2))) != nullptr,
          "proxy exports ordinal 2 (the ordinal acz.exe imports)");
    Check(GetProcAddress(proxy, reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(3))) != nullptr,
          "proxy exports ordinal 3 (the second ordinal acz.exe imports)");
    Check(GetProcAddress(proxy, "XInputGetCapabilities") != nullptr,
          "proxy exports XInputGetCapabilities");

    if (getState != nullptr) {
        unsigned char state[16];
        std::memset(state, 0, sizeof(state));
        const DWORD rc1 = getState(0, state);
        std::printf("       XInputGetState(0) -> %lu\n", rc1);
        Check(rc1 == ERROR_DEVICE_NOT_CONNECTED || rc1 == ERROR_SUCCESS,
              "forwarded XInputGetState returns 1167 or 0 (forwarded, no crash)");
        const DWORD rc2 = getState(0, state);
        Check(rc2 == rc1, "second call through the cached real pointer agrees");
        const DWORD rc3 = getState(1, state);
        Check(rc3 == ERROR_DEVICE_NOT_CONNECTED || rc3 == ERROR_SUCCESS,
              "another pad index forwards cleanly too");
    }

    // Give the bootstrap thread a moment, then unload. The proxy pins itself,
    // so FreeLibrary is expected to be harmless rather than unload it.
    Sleep(50);
    Check(FreeLibrary(proxy) != FALSE, "FreeLibrary(proxy) returns without crashing");

    // --- optional: the VERSION.dll proxy, if it was built ---
    wchar_t srcVer[MAX_PATH] = {};
    wchar_t isoVer[MAX_PATH] = {};
    JoinPathW(srcVer, MAX_PATH, exeDir, L"VERSION.dll");
    JoinPathW(isoVer, MAX_PATH, isoDir, L"VERSION.dll");
    if (GetFileAttributesW(srcVer) != INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(srcVer, isoVer, FALSE) != FALSE) {
            HMODULE ver = LoadLibraryW(isoVer);
            Check(ver != nullptr, "LoadLibraryW(build\\VERSION.dll) succeeds");
            if (ver != nullptr) {
                typedef DWORD(WINAPI * SizeFn)(LPCWSTR, LPDWORD);
                SizeFn sizeFn = reinterpret_cast<SizeFn>(
                    GetProcAddress(ver, "GetFileVersionInfoSizeW"));
                Check(sizeFn != nullptr, "version proxy exports GetFileVersionInfoSizeW");
                if (sizeFn != nullptr) {
                    wchar_t kernel32[MAX_PATH] = {};
                    GetSystemDirectoryW(kernel32, MAX_PATH);
                    JoinPathW(kernel32, MAX_PATH, kernel32, L"kernel32.dll");
                    DWORD handle = 0;
                    const DWORD bytes = sizeFn(kernel32, &handle);
                    std::printf("       GetFileVersionInfoSizeW(kernel32.dll) -> %lu\n", bytes);
                    Check(bytes > 0, "version proxy forwards a real query (size > 0)");
                }
                Sleep(50);
                Check(FreeLibrary(ver) != FALSE, "FreeLibrary(version proxy) returns");
            }
        }
    } else {
        std::printf("       (build\\VERSION.dll not present -- version proxy skipped)\n");
    }
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 6. Real prologues of the EFF_MIS_POSDIR ring writers (plain-MSL survey)
// ---------------------------------------------------------------------------
// Both are leaf functions without .pdata entries, so no symbol can anchor them and
// nothing else proves they are hookable. A refusal here would mean the ring survey
// silently installs nothing, i.e. a wasted game run.
bool Test6() {
    g_testOk = true;
    std::printf("[6/9] real prologues: EFF_MIS_POSDIR ring writers\n");
    struct Pro {
        const char* label;
        uint8_t b[24];
    };
    const Pro pros[2] = {
        {"exe+0x3B300",
         {0x8B, 0x42, 0x08, 0x4C, 0x8B, 0xD2, 0x41, 0x03, 0xC1, 0x99, 0x41, 0xF7, 0x7A, 0x04}},
        {"exe+0x3B370",
         {0x48, 0x63, 0x02, 0x4C, 0x63, 0x4A, 0x0C, 0x4C, 0x03, 0xC8, 0x41, 0x8B, 0x00, 0x49,
          0xC1, 0xE1, 0x05}},
    };
    for (const Pro& p : pros) {
        size_t total = 0;
        bool reloc = true;
        int n = 0;
        while (total < 14 && n < 8) {
            const size_t len = acz::hook::InstructionLength(p.b + total, 24 - total);
            if (!len) {
                reloc = false;
                break;
            }
            if (!acz::hook::InstructionRelocatable(p.b + total, len)) reloc = false;
            total += len;
            ++n;
        }
        std::printf("    %-13s instructions=%d stolen=%zu bytes relocatable=%s\n", p.label, n, total,
                    reloc ? "yes" : "no");
        Check(total >= 14, "prologue is long enough for the 14-byte absolute-jump form");
        Check(reloc, "every stolen instruction is relocatable");
    }
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 7. Real prologues of the "Run A" probe hook targets (docs/PROBE_RUN_A.md)
// ---------------------------------------------------------------------------
// The probe hooks the pool allocator at exe+0x201460 and the node-pop function.
// The documented pop entry exe+0x1B2C0 is NOT hookable:
//     83 39 00      cmp dword [rcx], 0
//     7F 03         jg +3              <- relative branch: cannot be relocated
//     33 C0 C3      xor eax,eax / ret  <- the "no free node" path
// (it also has no .pdata entry, so no symbol/unwind record anchors it).  The engine
// must refuse that entry and accept the jg's own target exe+0x1B2C8, which is the
// only path that can return a node.  These tests make the deviation a recorded fact
// instead of a silent assumption; a game-build change is caught here, offline.
bool Test7() {
    g_testOk = true;
    std::printf("[7/9] real prologues: Run A probe hook targets (alloc + node pop)\n");
    struct Pro {
        const char* label;
        uint8_t b[32];   // first bytes from work/acz_copy.exe.unpacked.exe, zero padded
        size_t have;     // how many of them are real code bytes
        bool hookable;
        size_t wantStolen;
    };
    const Pro pros[4] = {
        {"exe+0x201460 pool Allocate",
         {0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
          0x83, 0xEC, 0x10},
         17, true, 17},
        {"exe+0x1B2C0 node pop ENTRY",
         {0x83, 0x39, 0x00, 0x7F, 0x03, 0x33, 0xC0, 0xC3},
         8, false, 0},
        {"exe+0x1B2C8 node pop HOOK POINT",
         {0x48, 0x8D, 0x91, 0x18, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0x42, 0x08, 0x4C, 0x3B, 0xC2},
         14, true, 14},
        {"exe+0x1B7B00 world tick",
         {0x40, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x81, 0x90, 0x87, 0x0A,
          0x00},
         15, true, 15},
    };
    for (const Pro& p : pros) {
        // Same walk Test6 uses (and hook::Install performs internally): whole
        // instructions until the 14-byte absolute-jump form is covered; the 5-byte
        // rel32 form is the fallback for the refusal case.
        size_t total = 0;
        bool reloc = true;
        int n = 0;
        const size_t minBytes = p.hookable ? 14 : 5;
        while (total < minBytes && n < 8) {
            const size_t len = acz::hook::InstructionLength(p.b + total, p.have - total);
            if (!len) {
                reloc = false;
                break;
            }
            if (!acz::hook::InstructionRelocatable(p.b + total, len)) reloc = false;
            total += len;
            ++n;
        }
        std::printf("    %-32s instr=%d stolen=%zu relocatable=%s\n", p.label, n, total,
                    reloc ? "yes" : "no");
        if (p.hookable) {
            Check(total == p.wantStolen,
                  "decoder consumes the hookable prologue with the expected byte count");
            Check(reloc, "every stolen instruction is relocatable");
        } else {
            Check(!reloc, "decoder refuses the entry prologue (relative jg at +3)");
        }

        // The real engine entry point on the same bytes.  32-byte zero-padded
        // buffer so Probe() can never read past it.
        size_t probeStolen = 0;
        std::string why;
        const bool probed = acz::hook::Probe(p.b, probeStolen, why);
        Check(probed == p.hookable,
              p.hookable ? "hook::Probe accepts the prologue"
                         : "hook::Probe refuses the node-pop entry prologue");
        if (p.hookable) {
            Check(probeStolen == p.wantStolen, "hook::Probe reports the expected stolen byte count");
        } else {
            Check(probeStolen == 0, "the refused prologue reports 0 stolen bytes");
            Check(why.find("relocatable") != std::string::npos,
                  "the refusal reason names the non-relocatable instruction");
            std::printf("       expected refusal: %s\n", why.c_str());
        }
    }
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 8. Real prologue of the plain-MSL guidance hook target (docs/MSL_PN.md)
// ---------------------------------------------------------------------------
// FUN_14013ECA0 is the per-frame plain-MSL guidance/steering function found by the
// Run B hardware watch.  Its entry prologue is
//     48 8B C4        mov rax, rsp
//     48 89 58 08     mov [rax+0x8], rbx
//     48 89 70 18     mov [rax+0x18], rsi
//     48 89 78 20     mov [rax+0x20], rdi
// = 15 position-independent bytes ending exactly at the instruction boundary
// 0x13ECAF (the following `push rbp` is NOT stolen).  If this ever stops decoding,
// the plain-MSL PN hook must not be installed.
bool Test8() {
    g_testOk = true;
    std::printf("[8/9] real prologue: plain-MSL PN hook target (exe+0x13ECA0)\n");
    uint8_t pro[32] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                       0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
    size_t total = 0;
    bool reloc = true;
    int n = 0;
    while (total < 14 && n < 8) {
        const size_t len = acz::hook::InstructionLength(pro + total, 15 - total);
        if (!len) {
            reloc = false;
            break;
        }
        if (!acz::hook::InstructionRelocatable(pro + total, len)) reloc = false;
        total += len;
        ++n;
    }
    std::printf("    %-30s instr=%d stolen=%zu relocatable=%s\n",
                "exe+0x13ECA0 plain-MSL guidance", n, total, reloc ? "yes" : "no");
    Check(total == 15, "decoder consumes 15 bytes (mov rax,rsp + three spill moves)");
    Check(reloc, "every stolen instruction is relocatable (no RIP-relative operand)");
    size_t probeStolen = 0;
    std::string why;
    const bool probed = acz::hook::Probe(pro, probeStolen, why);
    Check(probed, "hook::Probe accepts the prologue");
    if (probed) {
        Check(probeStolen == 15, "hook::Probe reports 15 stolen bytes (14-byte abs jump + NOP)");
    } else {
        Check(false, "hook::Probe reports 15 stolen bytes (14-byte abs jump + NOP)");
        std::printf("       refusal: %s\n", why.c_str());
    }
    return g_testOk;
}

// ---------------------------------------------------------------------------
// 9. Step 1B Euler helper RVAs (verified against the unpacked reference image)
// ---------------------------------------------------------------------------
// The plain-MSL orientation takeover calls two pure-math game functions:
//   FUN_140202FA0(out_mat16, euler4)   euler angles -> 4x4 matrix (sincos)
//   FUN_1402036D0(out_euler4, in_euler4, rotvec3)   rotate Euler angles by rotvec
// The DLL byte-checks their entries at runtime before calling them; this test
// verifies the hard-coded expected bytes against work/acz_copy.exe.unpacked.exe so a
// game update is caught offline (the file is the same reference used by the prologue
// tests).  The functions themselves are never called here.
struct HelperRva {
    uint32_t rva;
    const uint8_t* bytes;
    size_t n;
    const char* name;
};

bool Test9() {
    g_testOk = true;
    std::printf("[9/9] Step 1B Euler helper entries (reference image byte check)%c", 10);
    const HelperRva items[2] = {
        {acz::msl_pn::kRvaEulerToMatrix, acz::msl_pn::kEulerToMatrixHead,
         sizeof(acz::msl_pn::kEulerToMatrixHead), "FUN_140202FA0 (euler->matrix)"},
        {acz::msl_pn::kRvaRotateEuler, acz::msl_pn::kRotateEulerHead,
         sizeof(acz::msl_pn::kRotateEulerHead), "FUN_1402036D0 (rotate euler)"},
    };

    // Parse the PE section table to map an RVA to a file offset.
    FILE* f = std::fopen("work/acz_copy.exe.unpacked.exe", "rb");
    if (f == nullptr) {
        std::printf("    (reference image work/acz_copy.exe.unpacked.exe not present - "
                    "helper byte check skipped)%c", 10);
        return g_testOk;
    }
    uint8_t hdr[0x1000] = {};
    const size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    bool peOk = got >= 0x200 && hdr[0] == 'M' && hdr[1] == 'Z';
    uint32_t eLfanew = 0;
    if (peOk) {
        std::memcpy(&eLfanew, hdr + 0x3C, 4);
        peOk = (eLfanew + 0x400) < got && hdr[eLfanew] == 'P' && hdr[eLfanew + 1] == 'E';
    }
    Check(peOk, "reference image parses as a PE file");
    if (!peOk) {
        std::fclose(f);
        return g_testOk;
    }
    uint16_t nsec = 0;
    uint16_t optSize = 0;
    std::memcpy(&nsec, hdr + eLfanew + 6, 2);
    std::memcpy(&optSize, hdr + eLfanew + 20, 2);
    const size_t secTable = static_cast<size_t>(eLfanew) + 24 + optSize;
    Check(nsec >= 1 && secTable + static_cast<size_t>(nsec) * 40 <= got,
          "PE section table is inside the read window");
    for (const HelperRva& it : items) {
        uint64_t fileOff = static_cast<uint64_t>(-1);
        for (uint16_t i = 0; i < nsec; ++i) {
            const uint8_t* sec = hdr + secTable + static_cast<size_t>(i) * 40;
            uint32_t vsize = 0, va = 0, rsize = 0, ra = 0;
            std::memcpy(&vsize, sec + 8, 4);
            std::memcpy(&va, sec + 12, 4);
            std::memcpy(&rsize, sec + 16, 4);
            std::memcpy(&ra, sec + 20, 4);
            const uint32_t span = (vsize > rsize) ? vsize : rsize;
            if (it.rva >= va && it.rva < va + span) {
                fileOff = static_cast<uint64_t>(ra) + (it.rva - va);
                break;
            }
        }
        bool match = false;
        if (fileOff != static_cast<uint64_t>(-1) && fileOff + it.n <= 0xFFFFFFFFull) {
            uint8_t disk[32] = {};
            if (it.n <= sizeof(disk) && std::fseek(f, static_cast<long>(fileOff), SEEK_SET) == 0 &&
                std::fread(disk, 1, it.n, f) == it.n) {
                match = std::memcmp(disk, it.bytes, it.n) == 0;
            }
        }
        std::printf("    %-34s rva=0x%X first %zu bytes %s%c", it.name, it.rva, it.n,
                    match ? "MATCH the reference image" : "DO NOT MATCH", 10);
        Check(match, it.name);
    }
    std::fclose(f);
    return g_testOk;
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
int Summarise() {
    std::printf("\n==============================================\n");
    std::printf("hooktest: %d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}

}  // namespace

int main() {
    std::printf("ACZPropNav hooktest (x64 inline hook + XINPUT1_4 proxy)\n\n");

    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"hook / trampoline / remove / reinstall", &Test1},
        {"128 install-remove cycles", &Test2},
        {"RIP-relative refusal", &Test3},
        {"InstructionLength table", &Test4},
        {"proxy smoke test", &Test5},
        {"real MIS ring prologues", &Test6},
        {"real Run A probe prologues", &Test7},
        {"real plain-MSL PN prologue", &Test8},
        {"Euler helper entries (reference image)", &Test9},
    };

    for (const auto& t : tests) {
        std::printf("----------------------------------------------------------\n");
        const bool ok = t.fn();
        std::printf("  => %s: %s\n", ok ? "PASS" : "FAIL", t.name);
    }
    return Summarise();
}
