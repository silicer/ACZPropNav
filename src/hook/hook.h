// ACZPropNav - minimal, conservative x86-64 inline hook engine.
//
// Deliberately conservative: if the first few instructions of the target cannot
// be safely relocated (RIP-relative operand, relative branch, unknown encoding),
// Install() FAILS and logs why instead of corrupting the game. A failed hook is
// always recoverable; a bad one crashes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acz::hook {

// Maximum bytes stolen from the target prologue. 14 = worst case for
// "mov rax, imm64 ; jmp rax" plus alignment.
constexpr size_t kMaxStolen = 32;

struct InstallResult {
    bool ok = false;
    size_t stolenBytes = 0;         // instruction-aligned bytes copied to the trampoline
    std::vector<uint8_t> original;  // original prologue bytes (for Remove)
    std::string reason;             // failure / diagnostic text
};

// Length of the instruction at `code`, or 0 if unsupported by the tiny decoder.
size_t InstructionLength(const uint8_t* code, size_t maxLen);

// True if the instruction may be relocated verbatim into a trampoline
// (no RIP-relative operand, no relative branch, no EIP-relative memory op).
bool InstructionRelocatable(const uint8_t* code, size_t len);

// Write an absolute jump to `dest` at `src`; `stolen` bytes are consumed, the
// remainder padded with NOPs. Saves original bytes into result.original.
bool WriteJump(uintptr_t src, uintptr_t dest, size_t stolen, InstallResult& out);

// Full install: builds a trampoline in a nearby executable allocation so that
// `*trampoline` points at a callable copy of the original function start.
bool Install(void* target, void* detour, void** trampoline, InstallResult& result,
             std::string* whyNot = nullptr);

bool Remove(void* target, const InstallResult& installed);

// Convenience: does the prologue look hookable at all? (used by Probe mode)
bool Probe(const void* target, size_t& stolenBytes, std::string& whyNot);

} // namespace acz::hook
