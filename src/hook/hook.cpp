// ACZPropNav - minimal, conservative x86-64 inline hook engine (implementation).
//
// Design goals, in order:
//   1. NEVER corrupt a prologue it cannot fully understand. Everything is
//      decoded and validated *before* a single byte is written; on any doubt
//      Install() fails with a reason instead of patching.
//   2. Be usable from a live game under a loader lock, with -fno-exceptions:
//      return codes only, no allocation in hot paths, no SEH, no C++ throw.
//   3. Be recoverable: Remove() puts the exact original bytes back and frees
//      the trampoline.
//
// Layout of what we build (14-byte absolute form, the preferred one):
//
//   target:   FF 25 00 00 00 00 <imm64 detour> 90 90 ...   (stolen >= 14)
//   trampoline: <copy of stolen prologue bytes> FF 25 00 00 00 00 <imm64 target+stolen>
//
//   or, when the prologue is too short for 14 bytes AND the detour is within
//   +-2 GB of the target (5-byte form):
//
//   target:   E9 <rel32 detour> 90 ...                     (stolen >= 5)
//   trampoline: <copy of stolen prologue bytes> FF 25 ... <imm64 target+stolen>
//
//   A relay (nearby RWX page holding the 14-byte absolute jump) is created only
//   when a 5-byte steal is forced and the detour is farther than +-2 GB away.
#include "hook/hook.h"

#include <windows.h>

#include <cstring>

namespace acz::hook {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr size_t kAbsJumpLen = 14;  // FF 25 00 00 00 00 + imm64
constexpr size_t kRelJumpLen = 5;   // E9 + rel32
constexpr size_t kMaxInsnLen = 15;  // architectural x86-64 instruction limit
constexpr size_t kMaxHooks = 64;    // concurrent hooks
constexpr size_t kMaxRelays = 16;   // standalone WriteJump() relays
constexpr uintptr_t kAllocGranularity = 0x10000;  // 64 KiB on every modern Windows

// ---------------------------------------------------------------------------
// Small helpers (no iostream / no exceptions)
// ---------------------------------------------------------------------------

std::string Hex(uint64_t v) {
    static const char* kDigits = "0123456789ABCDEF";
    char buf[2 + 16 + 1];
    char tmp[16];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v != 0) {
        tmp[t++] = kDigits[v & 0xF];
        v >>= 4;
    }
    int n = 0;
    buf[n++] = '0';
    buf[n++] = 'x';
    while (t > 0) buf[n++] = tmp[--t];
    buf[n] = '\0';
    return std::string(buf, static_cast<size_t>(n));
}

// Does a 5-byte E9 at `src` reach `dest`? `rel` is relative to src+5.
bool Rel32Fits(uintptr_t src, uintptr_t dest, int32_t& rel) {
    const int64_t d = static_cast<int64_t>(dest) - static_cast<int64_t>(src + kRelJumpLen);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    rel = static_cast<int32_t>(d);
    return true;
}

size_t PageSize() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096u;
}

// ---------------------------------------------------------------------------
// Global lock: CRITICAL_SECTION is recursive, which lets Install() call the
// public WriteJump() while holding it without deadlocking.
// ---------------------------------------------------------------------------

CRITICAL_SECTION g_lock;
INIT_ONCE g_lockOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitLockOnce(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

void Lock() {
    InitOnceExecuteOnce(&g_lockOnce, InitLockOnce, nullptr, nullptr);
    EnterCriticalSection(&g_lock);
}
void Unlock() { LeaveCriticalSection(&g_lock); }

// ---------------------------------------------------------------------------
// Hook registry. `Install` fills an entry, `Remove` finds it by target address.
// ---------------------------------------------------------------------------

struct Entry {
    uintptr_t target = 0;
    uintptr_t detour = 0;
    void* trampoline = nullptr;   // RWX page holding relocated prologue + jump back
    void* relay = nullptr;        // optional RWX page holding the absolute jump
    size_t stolen = 0;
    uint8_t original[kMaxStolen] = {};
    bool active = false;
};

Entry g_entries[kMaxHooks];

// Relays created by a *standalone* WriteJump() call (not via Install) so that
// Remove() can free them again.
struct RelayRec {
    uintptr_t src = 0;
    void* mem = nullptr;
};
RelayRec g_relays[kMaxRelays];

int FindEntry(uintptr_t target) {
    for (size_t i = 0; i < kMaxHooks; ++i)
        if (g_entries[i].active && g_entries[i].target == target) return static_cast<int>(i);
    return -1;
}

void RememberRelay(uintptr_t src, void* mem) {
    for (size_t i = 0; i < kMaxRelays; ++i) {
        if (g_relays[i].mem == nullptr) {
            g_relays[i].src = src;
            g_relays[i].mem = mem;
            return;
        }
    }
    // Table full: leak rather than crash (this path is not reachable from Install).
}

void* TakeRelay(uintptr_t src) {
    for (size_t i = 0; i < kMaxRelays; ++i)
        if (g_relays[i].mem != nullptr && g_relays[i].src == src) {
            void* mem = g_relays[i].mem;
            g_relays[i] = RelayRec();
            return mem;
        }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Executable memory allocation near a target address.
//
// We try 64 KiB-aligned candidates around the anchor, first linearly (+-4 MiB),
// then geometrically out to +-1 GiB, so a relay/trampoline lands inside rel32
// range of the target in practice. If everything nearby is occupied we fall
// back to "anywhere": the trampoline always ends in a 14-byte absolute jump,
// so its own position is irrelevant for correctness.
// ---------------------------------------------------------------------------
void* AllocNear(uintptr_t anchor, size_t size) {
    const uintptr_t page = PageSize();
    size_t aligned = (size + (page - 1)) & ~(page - 1);
    if (aligned == 0) aligned = page;

    // 1) linear scan within +-4 MiB (covers the common case: a free page right
    //    after the hooked module).
    for (unsigned step = 1; step <= 64; ++step) {
        const uintptr_t delta = static_cast<uintptr_t>(step) * kAllocGranularity;
        const uintptr_t cands[2] = {anchor - delta, anchor + delta};
        for (int k = 0; k < 2; ++k) {
            const uintptr_t c = cands[k];
            if (c < kAllocGranularity) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(c), aligned,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return p;
        }
    }
    // 2) geometric scan out to +-1 GiB.
    for (uintptr_t delta = 8ull * 1024 * 1024; delta <= 0x40000000ull; delta <<= 1) {
        const uintptr_t cands[2] = {anchor - delta, anchor + delta};
        for (int k = 0; k < 2; ++k) {
            const uintptr_t c = cands[k];
            if (c < kAllocGranularity) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(c), aligned,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return p;
        }
    }
    // 3) anywhere (still safe: every jump we emit is absolute).
    return VirtualAlloc(nullptr, aligned, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

// ---------------------------------------------------------------------------
// Instruction decoder.
//
// Scope: the instruction forms that actually occur in function prologues plus
// the rest of the integer/SSE/x87/AVX surface we can classify confidently.
// Anything we do not understand yields len == 0 ("unsupported"), which makes
// Probe/Install refuse the hook -- that is always the safe answer.
// ---------------------------------------------------------------------------

struct Decoded {
    size_t len = 0;          // 0 == unsupported / truncated
    bool relocatable = false;
    bool terminal = false;   // ret/jmp/int3/ud2/...: control never falls through
};

struct DecodeState {
    const uint8_t* code = nullptr;
    size_t maxLen = 0;       // capped at kMaxInsnLen by the caller
    size_t pos = 0;
    bool opsize16 = false;   // 66 prefix
    bool addr32 = false;     // 67 prefix
    bool rexW = false;       // REX.W
    bool relocatable = true; // cleared for RIP-relative operands
    bool terminal = false;   // set for instructions that end the basic block
};

bool Have(const DecodeState& s, size_t n) { return s.pos + n <= s.maxLen; }

// Immediate size for "imm16/32/64 depending on operand size" operands.
size_t ImmZ(const DecodeState& s) { return s.opsize16 ? 2u : 4u; }

// Decode ModRM (+SIB +displacement). Returns bytes consumed or 0 on error.
size_t DecodeModRM(DecodeState& s) {
    if (!Have(s, 1)) return 0;
    const uint8_t modrm = s.code[s.pos];
    const unsigned mod = static_cast<unsigned>(modrm >> 6);
    const unsigned rm = static_cast<unsigned>(modrm & 7);
    size_t n = 1;
    if (mod != 3) {
        if (rm == 4) {
            // SIB byte
            if (!Have(s, 2)) return 0;
            n = 2;
            const uint8_t sib = s.code[s.pos + 1];
            if (mod == 0 && (sib & 7) == 5) n += 4;  // disp32 without base
        } else if (mod == 0 && rm == 5) {
            // [rip + disp32]: PC-relative, cannot be copied verbatim.
            s.relocatable = false;
            n += 4;
        }
        if (mod == 1) n += 1;
        else if (mod == 2) n += 4;
    }
    if (!Have(s, n)) return 0;
    s.pos += n;
    return n;
}

bool SkipImm(DecodeState& s, size_t n) {
    if (!Have(s, n)) return false;
    s.pos += n;
    return true;
}

// Two-byte (0F xx) opcode map. `vex` selects the VEX-encoded flavour of the
// same map (same ModRM/immediate shapes, no legacy relative branches).
bool Decode0FMap(DecodeState& s, bool vex) {
    if (!Have(s, 1)) return false;
    const uint8_t op2 = s.code[s.pos++];

    // jcc rel32 (0F 80..8F) - relative branch, legacy only.
    if (op2 >= 0x80 && op2 <= 0x8F) {
        if (vex) return false;
        if (!SkipImm(s, 4)) return false;
        s.relocatable = false;
        return true;
    }
    // Opcodes with neither ModRM nor immediate.
    switch (op2) {
        case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0E:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
        case 0x35: case 0x37: case 0x77: case 0xA0: case 0xA1: case 0xA2:
        case 0xA8: case 0xA9: case 0xAA:
            return true;
        case 0x0B:  // ud2: traps, nothing after it is reachable
            s.terminal = true;
            return true;
        default:
            break;
    }
    if (op2 >= 0xC8 && op2 <= 0xCF) return true;  // bswap reg

    // Opcodes carrying an imm8 after ModRM.
    switch (op2) {
        case 0x0F:  // 3DNow! (ModRM + imm8 suffix)
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0xA4: case 0xAC: case 0xBA:
        case 0xC2: case 0xC4: case 0xC5: case 0xC6:
            if (!DecodeModRM(s)) return false;
            return SkipImm(s, 1);
        default:
            break;
    }
    // Everything else in the 0F map is ModRM-only.
    return DecodeModRM(s) != 0;
}

// VEX (C4/C5) prefixed instruction: 2-byte or 3-byte VEX + opcode + operands.
bool DecodeVEX(DecodeState& s, uint8_t lead) {
    unsigned map = 1;  // 1 = 0F map (implied by C5)
    if (lead == 0xC5) {
        if (!Have(s, 1)) return false;
        s.pos += 1;
    } else {
        if (!Have(s, 2)) return false;
        map = static_cast<unsigned>(s.code[s.pos] & 0x1F);
        s.pos += 2;
    }
    if (!Have(s, 1)) return false;
    // The VEX-mapped opcode byte selects the operand shape; for the 0F map it is
    // consumed by Decode0FMap itself, for the 0F38/0F3A maps right here.
    switch (map) {
        case 1:  // 0F map
            return Decode0FMap(s, /*vex=*/true);
        case 2:  // 0F38 map: ModRM only
            s.pos += 1;
            return DecodeModRM(s) != 0;
        case 3:  // 0F3A map: ModRM + imm8
            s.pos += 1;
            if (!DecodeModRM(s)) return false;
            return SkipImm(s, 1);
        default:
            return false;
    }
}

// Main decoder: one instruction at `code`, bounded by maxLen (<= 15).
Decoded Decode(const uint8_t* code, size_t maxLen) {
    Decoded out;  // len == 0 means unsupported
    if (code == nullptr || maxLen == 0) return out;

    const size_t cap = (maxLen < kMaxInsnLen) ? maxLen : kMaxInsnLen;
    DecodeState s;
    s.code = code;
    s.maxLen = cap;

    // ---- legacy prefixes (any order, at most 14 of them) -------------------
    size_t prefixes = 0;
    for (;;) {
        if (!Have(s, 1)) return out;
        bool isPrefix = true;
        switch (code[s.pos]) {
            case 0x66: s.opsize16 = true; break;
            case 0x67: s.addr32 = true; break;
            case 0xF0: case 0xF2: case 0xF3:
            case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65:
                break;
            default: isPrefix = false; break;
        }
        if (!isPrefix) break;
        ++s.pos;
        if (++prefixes > 14) return out;  // malformed
    }

    // ---- REX (necessarily the byte right before the opcode) ----------------
    if (Have(s, 1) && (code[s.pos] & 0xF0) == 0x40) {
        s.rexW = (code[s.pos] & 0x08) != 0;
        ++s.pos;
    }
    if (!Have(s, 1)) return out;

    const uint8_t op = code[s.pos++];

    auto done = [&](bool branchReloc) -> Decoded {
        out.len = s.pos;
        out.relocatable = branchReloc && s.relocatable;
        out.terminal = s.terminal;
        return out;
    };

    // ---- 0F escape ---------------------------------------------------------
    if (op == 0x0F) {
        if (!Have(s, 1)) return out;
        const uint8_t op2 = code[s.pos];
        if (op2 == 0x38) {          // three-byte map 0F 38 xx: ModRM only
            s.pos += 2;
            if (!DecodeModRM(s)) return out;
            return done(true);
        }
        if (op2 == 0x3A) {          // three-byte map 0F 3A xx: ModRM + imm8
            s.pos += 2;
            if (!DecodeModRM(s)) return out;
            if (!SkipImm(s, 1)) return out;
            return done(true);
        }
        if (!Decode0FMap(s, /*vex=*/false)) return out;
        return done(true);
    }

    // ---- 00..3F: add/or/adc/sbb/and/sub/xor/cmp families -------------------
    if (op < 0x40) {
        const unsigned lo = op & 7u;
        if (lo <= 3u) {
            if (!DecodeModRM(s)) return out;
        } else if (lo == 4u) {
            if (!SkipImm(s, 1)) return out;   // AL, imm8
        } else if (lo == 5u) {
            if (!SkipImm(s, ImmZ(s))) return out;  // rAX, imm16/32
        } else {
            return out;                      // 0x06/0x07/0x0E/... invalid in long mode
        }
        return done(true);
    }

    if (op >= 0x50 && op <= 0x5F) return done(true);  // push/pop r64

    if (op == 0x63) {                                 // movsxd
        if (!DecodeModRM(s)) return out;
        return done(true);
    }
    if (op == 0x68) {                                 // push imm32
        if (!SkipImm(s, ImmZ(s))) return out;
        return done(true);
    }
    if (op == 0x69) {                                 // imul r, r/m, imm32
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, ImmZ(s))) return out;
        return done(true);
    }
    if (op == 0x6A) {                                 // push imm8
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0x6B) {                                 // imul r, r/m, imm8
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op >= 0x70 && op <= 0x7F) {                   // jcc rel8
        if (!SkipImm(s, 1)) return out;
        return done(false);
    }
    if (op == 0x80) {                                 // group1 r/m8, imm8
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0x81) {                                 // group1 r/m, imm16/32
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, ImmZ(s))) return out;
        return done(true);
    }
    if (op == 0x82) return out;                       // invalid in long mode
    if (op == 0x83) {                                 // group1 r/m, imm8
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op >= 0x84 && op <= 0x8F) {                   // test/xchg/mov/lea/pop r/m
        if (!DecodeModRM(s)) return out;
        return done(true);
    }
    if (op >= 0x90 && op <= 0x99) return done(true);   // nop/xchg/cbw..cqo
    if (op == 0x9A) return out;                       // callf ptr16:32 invalid in long mode
    if (op >= 0x9B && op <= 0x9F) return done(true);   // fwait/pushfq/popfq/sahf/lahf
    if (op >= 0xA0 && op <= 0xA3) {                   // mov al/eax, moffs
        if (!SkipImm(s, s.addr32 ? 4u : 8u)) return out;
        return done(true);
    }
    if (op >= 0xA4 && op <= 0xA7) return done(true);   // movs/cmps
    if (op == 0xA8) {
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0xA9) {
        if (!SkipImm(s, ImmZ(s))) return out;
        return done(true);
    }
    if (op >= 0xAA && op <= 0xAF) return done(true);   // stos/lods/scas
    if (op >= 0xB0 && op <= 0xB7) {                   // mov r8, imm8
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op >= 0xB8 && op <= 0xBF) {                   // mov r32/64, imm32/64
        if (!SkipImm(s, s.rexW ? 8u : ImmZ(s))) return out;
        return done(true);
    }
    if (op == 0xC0 || op == 0xC1) {                   // group2 r/m, imm8
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0xC2) {                                 // ret imm16
        if (!SkipImm(s, 2)) return out;
        s.terminal = true;
        return done(true);
    }
    if (op == 0xC3) {                                 // ret
        s.terminal = true;
        return done(true);
    }
    if (op == 0xC4 || op == 0xC5) {                   // VEX
        if (!DecodeVEX(s, op)) return out;
        return done(true);
    }
    if (op == 0xC6) {                                 // group11 r/m8, imm8
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0xC7) {                                 // group11 r/m, imm16/32
        if (!DecodeModRM(s)) return out;
        if (!SkipImm(s, ImmZ(s))) return out;
        return done(true);
    }
    if (op == 0xC8) {                                 // enter imm16, imm8
        if (!SkipImm(s, 3)) return out;
        return done(true);
    }
    if (op == 0xC9) return done(true);                // leave
    if (op == 0xCA) {                                 // retf imm16
        if (!SkipImm(s, 2)) return out;
        s.terminal = true;
        return done(true);
    }
    if (op == 0xCB) {                                 // retf
        s.terminal = true;
        return done(true);
    }
    if (op == 0xCC) {                                 // int3 (usually inter-function padding)
        s.terminal = true;
        return done(true);
    }
    if (op == 0xCD) {                                 // int imm8
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0xCE) return out;                       // into invalid in long mode
    if (op == 0xCF) {                                 // iret
        s.terminal = true;
        return done(true);
    }
    if (op >= 0xD0 && op <= 0xD3) {                   // group2 r/m, 1/cl
        if (!DecodeModRM(s)) return out;
        return done(true);
    }
    if (op == 0xD4 || op == 0xD5 || op == 0xD6) return out;  // invalid in long mode
    if (op == 0xD7) return done(true);                // xlatb
    if (op >= 0xD8 && op <= 0xDF) {                   // x87: always ModRM
        if (!DecodeModRM(s)) return out;
        return done(true);
    }
    if (op >= 0xE0 && op <= 0xE3) {                   // loopne/loope/loop/jrcxz rel8
        if (!SkipImm(s, 1)) return out;
        return done(false);
    }
    if (op >= 0xE4 && op <= 0xE7) {                   // in/out imm8
        if (!SkipImm(s, 1)) return out;
        return done(true);
    }
    if (op == 0xE8 || op == 0xE9) {                   // call/jmp rel32
        if (!SkipImm(s, 4)) return out;
        if (op == 0xE9) s.terminal = true;             // jmp: no fall-through
        return done(false);
    }
    if (op == 0xEA) return out;                       // jmpf ptr16:32 invalid in long mode
    if (op == 0xEB) {                                 // jmp rel8
        if (!SkipImm(s, 1)) return out;
        s.terminal = true;
        return done(false);
    }
    if (op >= 0xEC && op <= 0xEF) return done(true);   // in/out dx
    if (op == 0xF1) return done(true);                // int1
    if (op == 0xF4) {                                 // hlt
        s.terminal = true;
        return done(true);
    }
    if (op == 0xF5) return done(true);                // cmc
    if (op == 0xF6 || op == 0xF7) {                   // group3
        if (!Have(s, 1)) return out;
        const unsigned reg = static_cast<unsigned>((code[s.pos] >> 3) & 7);
        if (!DecodeModRM(s)) return out;
        if (reg <= 1) {                               // /0 test, /1 test (immediate forms)
            if (!SkipImm(s, op == 0xF6 ? 1u : ImmZ(s))) return out;
        }
        return done(true);
    }
    if (op >= 0xF8 && op <= 0xFD) return done(true);   // clc/stc/cli/sti/cld/std
    if (op == 0xFE || op == 0xFF) {                   // group4 / group5
        if (!Have(s, 1)) return out;
        const unsigned grp = static_cast<unsigned>((code[s.pos] >> 3) & 7);
        if (!DecodeModRM(s)) return out;
        // FF /3 callf, /4 jmp r/m, /5 jmpf: control never falls through.
        if (op == 0xFF && (grp == 3 || grp == 4 || grp == 5)) s.terminal = true;
        return done(true);
    }
    if (op == 0x62) return out;                       // EVEX (AVX-512) unsupported
    return out;
}

// Walk whole instructions from `code` until at least `minBytes` are covered.
// Every instruction must decode AND be relocatable; total <= kMaxStolen.
//
// The terminal-instruction rule is what makes this safe on real binaries: if a
// function ends (ret/jmp/int3/ud2) before minBytes is covered, the remaining
// bytes belong to the NEXT function, and overwriting them would hijack an
// unrelated entry point. Refusing is the only correct answer.
bool DecodePrologue(const uint8_t* code, size_t minBytes, size_t& stolen, std::string& why) {
    stolen = 0;
    size_t total = 0;
    while (total < minBytes) {
        if (total >= kMaxStolen) {
            why = "prologue needs more than kMaxStolen (" + std::to_string(kMaxStolen) +
                  ") bytes";
            return false;
        }
        const Decoded d = Decode(code + total, kMaxStolen - total);
        if (d.len == 0) {
            why = "unsupported/malformed instruction at prologue offset " + std::to_string(total) +
                  " (byte " + Hex(code[total]) + ")";
            return false;
        }
        if (!d.relocatable) {
            why = "instruction at prologue offset " + std::to_string(total) +
                  " is not relocatable (RIP-relative operand or relative branch)";
            return false;
        }
        total += d.len;
        if (d.terminal && total < minBytes) {
            why = "function ends at prologue offset " + std::to_string(total) +
                  " (ret/jmp/int3) before " + std::to_string(minBytes) +
                  " bytes could be stolen; refusing to overwrite the next function";
            return false;
        }
    }
    stolen = total;
    return true;
}

// Is every page of [addr, addr+n) committed and readable?
//
// Deliberately uses VirtualQuery rather than SEH: this file is built with
// MinGW-w64 GCC, which has no __try/__except, and hook.cpp is intentionally
// independent of src/mem (the offline hooktest links it standalone). A
// caller-supplied target (HookRva / HookSignature) can point anywhere - including
// at the last bytes of a mapped region - and a raw read there faults on the
// ModThread and kills the process.
bool RangeReadable(uintptr_t addr, size_t n) {
    if (addr == 0 || n == 0) return false;
    const uintptr_t end = addr + n;
    if (end < addr) return false;  // overflow
    uintptr_t at = addr;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD p = mbi.Protect;
        if ((p & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const DWORD base = p & 0xFF;  // strip PAGE_NOCACHE / PAGE_WRITECOMBINE
        const bool ok = (base == PAGE_READONLY || base == PAGE_READWRITE ||
                         base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
                         base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY);
        if (!ok) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) return false;  // no forward progress
        at = next;
    }
    return true;
}

// Copy `n` bytes from a possibly-invalid address into `dst`.
// Returns the number of bytes copied (0 when the range is not fully readable).
size_t SafeCopyBytes(const void* src, void* dst, size_t n) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(src);
    if (!RangeReadable(addr, n)) return 0;
    std::memcpy(dst, src, n);
    return n;
}

// Write the 14-byte absolute jump at `at` into an already-writable buffer.
void EmitAbsJump(uint8_t* at, uintptr_t dest) {
    at[0] = 0xFF;
    at[1] = 0x25;
    at[2] = 0x00;
    at[3] = 0x00;
    at[4] = 0x00;
    at[5] = 0x00;
    for (int i = 0; i < 8; ++i) at[6 + i] = static_cast<uint8_t>((dest >> (8 * i)) & 0xFF);
}

// Build the byte patch for `stolen` bytes at `src` jumping to `dest`.
// `dest` must already be rel32-reachable when `stolen < 14`.
bool BuildPatch(uintptr_t src, uintptr_t dest, size_t stolen, uint8_t* patch) {
    if (stolen < kRelJumpLen || stolen > kMaxStolen || patch == nullptr) return false;
    memset(patch, 0x90, stolen);  // NOP padding
    if (stolen >= kAbsJumpLen) {
        EmitAbsJump(patch, dest);
        return true;
    }
    int32_t rel = 0;
    if (!Rel32Fits(src, dest, rel)) return false;
    patch[0] = 0xE9;
    patch[1] = static_cast<uint8_t>(rel & 0xFF);
    patch[2] = static_cast<uint8_t>((rel >> 8) & 0xFF);
    patch[3] = static_cast<uint8_t>((rel >> 16) & 0xFF);
    patch[4] = static_cast<uint8_t>((rel >> 24) & 0xFF);
    return true;
}

// Core of WriteJump (assumes the lock is held by the caller).
bool WriteJumpLocked(uintptr_t src, uintptr_t dest, size_t stolen, InstallResult& out) {
    if (src == 0 || dest == 0) {
        out.reason = "WriteJump: null src/dest";
        return false;
    }
    if (stolen < kRelJumpLen || stolen > kMaxStolen) {
        out.reason = "WriteJump: stolen byte count out of range (" + std::to_string(stolen) + ")";
        return false;
    }
    // Save the original bytes first: Remove() needs them even if the write fails.
    out.original.assign(reinterpret_cast<const uint8_t*>(src),
                        reinterpret_cast<const uint8_t*>(src) + stolen);

    uintptr_t effectiveDest = dest;
    if (stolen < kAbsJumpLen) {
        int32_t rel = 0;
        if (!Rel32Fits(src, dest, rel)) {
            // Need a relay within +-2 GB of src that holds the absolute jump.
            void* relay = AllocNear(src, PageSize());
            if (relay == nullptr) {
                out.reason = "WriteJump: no free page near target for a relay";
                return false;
            }
            EmitAbsJump(static_cast<uint8_t*>(relay), dest);
            FlushInstructionCache(GetCurrentProcess(), relay, kAbsJumpLen);
            effectiveDest = reinterpret_cast<uintptr_t>(relay);
            RememberRelay(src, relay);
        }
    }

    uint8_t patch[kMaxStolen];
    if (!BuildPatch(src, effectiveDest, stolen, patch)) {
        out.reason = "WriteJump: cannot encode a jump for the given byte count";
        return false;
    }

    // Defensive: refuse if the prologue changed since we decoded it.
    uint8_t current[kMaxStolen] = {};
    if (SafeCopyBytes(reinterpret_cast<const void*>(src), current, stolen) != stolen) {
        out.reason = "WriteJump: target bytes are no longer readable";
        return false;
    }
    if (memcmp(current, out.original.data(), stolen) != 0) {
        out.reason = "WriteJump: target bytes changed during install (concurrent patch?)";
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(src), stolen, PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        out.reason = "WriteJump: VirtualProtect(RWX) failed, error " +
                     std::to_string(GetLastError());
        return false;
    }
    memcpy(reinterpret_cast<void*>(src), patch, stolen);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(src), stolen);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(src), stolen, oldProtect, &ignored);
    out.stolenBytes = stolen;
    out.ok = true;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

size_t InstructionLength(const uint8_t* code, size_t maxLen) {
    return Decode(code, maxLen).len;
}

bool InstructionRelocatable(const uint8_t* code, size_t len) {
    if (code == nullptr || len == 0) return false;
    const Decoded d = Decode(code, len);
    // Must be exactly one complete instruction of the given size.
    if (d.len == 0 || d.len != len) return false;
    return d.relocatable;
}

bool Probe(const void* target, size_t& stolenBytes, std::string& whyNot) {
    stolenBytes = 0;
    whyNot.clear();
    if (target == nullptr) {
        whyNot = "null target";
        return false;
    }
    uint8_t code[kMaxStolen] = {};
    if (SafeCopyBytes(target, code, kMaxStolen) != kMaxStolen) {
        whyNot = "target prologue is not readable";
        return false;
    }
    // Report what Install would steal: prefer the 14-byte absolute-jump form,
    // fall back to the 5-byte rel32 form when the prologue is shorter.
    std::string why14;
    if (DecodePrologue(code, kAbsJumpLen, stolenBytes, why14)) return true;
    stolenBytes = 0;
    if (DecodePrologue(code, kRelJumpLen, stolenBytes, whyNot)) return true;
    stolenBytes = 0;
    return false;
}

bool WriteJump(uintptr_t src, uintptr_t dest, size_t stolen, InstallResult& out) {
    out.ok = false;
    Lock();
    const bool ok = WriteJumpLocked(src, dest, stolen, out);
    Unlock();
    out.ok = ok;
    return ok;
}

bool Install(void* target, void* detour, void** trampoline, InstallResult& result,
             std::string* whyNot) {
    result = InstallResult();
    if (trampoline != nullptr) *trampoline = nullptr;

    auto fail = [&](const std::string& msg) -> bool {
        result.ok = false;
        result.reason = msg;
        if (whyNot != nullptr) *whyNot = msg;
        return false;
    };

    if (target == nullptr || detour == nullptr || trampoline == nullptr)
        return fail("invalid argument (null target/detour/trampoline)");
    if (target == detour) return fail("target and detour are the same address");

    Lock();

    if (FindEntry(reinterpret_cast<uintptr_t>(target)) >= 0) {
        Unlock();
        return fail("target is already hooked");
    }

    // ---- 1. pick the steal size: prefer the 14-byte absolute form ----------
    // Decode a fault-guarded local copy, never the target address directly: a target
    // near the end of a mapped region would fault inside the decoder.
    uint8_t code[kMaxStolen] = {};
    if (SafeCopyBytes(target, code, kMaxStolen) != kMaxStolen) {
        Unlock();
        return fail("target prologue is not readable (bad HookRva/HookSignature, or the "
                    "address is at the end of a mapped region)");
    }
    size_t stolen = 0;
    std::string why14;
    bool useAbsForm = DecodePrologue(code, kAbsJumpLen, stolen, why14);
    if (!useAbsForm) {
        std::string why5;
        if (!DecodePrologue(code, kRelJumpLen, stolen, why5)) {
            Unlock();
            return fail("prologue not hookable: " + why5);
        }
    }

    // ---- 2. reserve a trampoline near the target ---------------------------
    void* const trampMem = AllocNear(reinterpret_cast<uintptr_t>(target), PageSize());
    if (trampMem == nullptr) {
        Unlock();
        return fail("out of executable memory for the trampoline");
    }

    // ---- 3. determine the effective destination for the jump --------------
    uintptr_t effectiveDest = reinterpret_cast<uintptr_t>(detour);
    void* relayMem = nullptr;
    if (!useAbsForm) {
        int32_t rel = 0;
        if (!Rel32Fits(reinterpret_cast<uintptr_t>(target), effectiveDest, rel)) {
            relayMem = AllocNear(reinterpret_cast<uintptr_t>(target), PageSize());
            if (relayMem == nullptr) {
                VirtualFree(trampMem, 0, MEM_RELEASE);
                Unlock();
                return fail("detour is out of +-2 GB and no relay page could be allocated");
            }
            uint8_t* r = static_cast<uint8_t*>(relayMem);
            EmitAbsJump(r, reinterpret_cast<uintptr_t>(detour));
            FlushInstructionCache(GetCurrentProcess(), relayMem, kAbsJumpLen);
            DWORD ro = 0;
            VirtualProtect(relayMem, PageSize(), PAGE_EXECUTE_READ, &ro);
            effectiveDest = reinterpret_cast<uintptr_t>(relayMem);
        }
    }

    // ---- 4. build the trampoline: stolen bytes + absolute jump back --------
    {
        uint8_t* t = static_cast<uint8_t*>(trampMem);
        memcpy(t, code, stolen);
        EmitAbsJump(t + stolen, reinterpret_cast<uintptr_t>(target) + stolen);
        FlushInstructionCache(GetCurrentProcess(), trampMem, stolen + kAbsJumpLen);
        // Executable, not writable, once we are done writing.
        DWORD ro = 0;
        VirtualProtect(trampMem, PageSize(), PAGE_EXECUTE_READ, &ro);
    }

    // ---- 5. publish the trampoline BEFORE the patch goes live --------------
    // The detour reads this pointer; if the prologue were patched first there would
    // be a window in which the game calls the detour while *trampoline is still null
    // (the caller only stores it after Install returns). Publish early and clear it
    // again on every failure path below.
    *trampoline = trampMem;

    // ---- 6. patch the prologue --------------------------------------------
    if (!WriteJumpLocked(reinterpret_cast<uintptr_t>(target), effectiveDest, stolen, result)) {
        *trampoline = nullptr;
        VirtualFree(trampMem, 0, MEM_RELEASE);
        if (relayMem != nullptr) VirtualFree(relayMem, 0, MEM_RELEASE);
        Unlock();
        return fail(result.reason.empty() ? "failed to write the jump" : result.reason);
    }

    // ---- 7. register the hook ---------------------------------------------
    int slot = -1;
    for (size_t i = 0; i < kMaxHooks; ++i) {
        if (!g_entries[i].active) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        // Should not happen (we allowed 64 hooks), but restore rather than
        // leaving a patch behind without a registry entry.
        DWORD old = 0;
        if (VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(target, result.original.data(), stolen);
            FlushInstructionCache(GetCurrentProcess(), target, stolen);
            DWORD ignored = 0;
            VirtualProtect(target, stolen, old, &ignored);
        }
        *trampoline = nullptr;
        VirtualFree(trampMem, 0, MEM_RELEASE);
        if (relayMem != nullptr) VirtualFree(relayMem, 0, MEM_RELEASE);
        Unlock();
        return fail("hook registry full");
    }

    Entry& e = g_entries[slot];
    e.target = reinterpret_cast<uintptr_t>(target);
    e.detour = reinterpret_cast<uintptr_t>(detour);
    e.trampoline = trampMem;
    e.relay = relayMem;
    e.stolen = stolen;
    memcpy(e.original, result.original.data(), stolen);
    e.active = true;

    result.ok = true;
    result.stolenBytes = stolen;
    result.reason = useAbsForm ? "absolute 14-byte jump" : "rel32 5-byte jump";
    if (whyNot != nullptr) whyNot->clear();

    Unlock();
    return true;
}

bool Remove(void* target, const InstallResult& installed) {
    if (target == nullptr) return false;
    Lock();

    const int idx = FindEntry(reinterpret_cast<uintptr_t>(target));
    if (idx < 0) {
        Unlock();
        return false;  // idempotent: nothing to do
    }
    Entry& e = g_entries[idx];

    // The registry copy is authoritative; `installed.original` is only a
    // sanity reference for the caller. Be defensive if it disagrees.
    const size_t stolen = e.stolen;
    const uint8_t* original = e.original;
    if (!installed.original.empty() && installed.original.size() == stolen)
        original = installed.original.data();

    bool restored = false;
    DWORD oldProtect = 0;
    if (VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(target, original, stolen);
        FlushInstructionCache(GetCurrentProcess(), target, stolen);
        DWORD ignored = 0;
        VirtualProtect(target, stolen, oldProtect, &ignored);
        restored = true;
    }

    // Only release the trampoline if the prologue was actually restored. If the
    // restore failed the 14-byte jump is STILL LIVE and every future call would run
    // into the detour, whose trampoline we are about to free: that is a
    // use-after-free followed by a spin on a null trampoline (the game thread hangs).
    // Leaking one page in that (already catastrophic) case is the safe trade.
    if (restored) {
        if (e.trampoline != nullptr) VirtualFree(e.trampoline, 0, MEM_RELEASE);
        if (e.relay != nullptr) VirtualFree(e.relay, 0, MEM_RELEASE);
        if (void* standalone = TakeRelay(e.target)) VirtualFree(standalone, 0, MEM_RELEASE);
    } else if (e.trampoline != nullptr) {
        // Keep the trampoline mapped so a still-patched prologue cannot jump into
        // freed memory. Drop only the registry entry that pointed at it.
        e.trampoline = nullptr;
    }

    e = Entry();
    Unlock();
    return restored;
}

}  // namespace acz::hook
