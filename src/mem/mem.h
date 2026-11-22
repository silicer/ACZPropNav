// ACZPropNav - SEH-safe process memory primitives, region enumeration and scanners.
//
// Everything here is designed to be safe to run inside a live, running game:
//  - every dereference goes through __try/__except,
//  - scans only touch committed, readable memory that VirtualQueryEx reports,
//  - writes re-protect the page, write, restore, and are size/alignment checked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/acz_common.h"

namespace acz::mem {

struct ModuleInfo {
    uintptr_t base = 0;
    size_t size = 0;
    std::wstring path;
};

struct Region {
    uintptr_t base = 0;
    size_t size = 0;
    uint32_t protect = 0;   // PAGE_* constant
    uint32_t state = 0;     // MEM_COMMIT etc.
    uint32_t type = 0;      // MEM_IMAGE / MEM_PRIVATE / MEM_MAPPED
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool image = false;
};

// The process we operate on (defaults to the current process).
bool SetProcess(void* hProcess);
void* Process();
bool MainModule(ModuleInfo& out);

// Enumerate committed regions. `minSize` filters tiny allocations.
void EnumRegions(std::vector<Region>& out, size_t minSize = 0x1000);
std::vector<Region> WritableRegions(size_t minSize = 0x1000);
std::vector<Region> ExecutableRegions(size_t minSize = 0x1000);

// ---- SEH-safe primitive access -------------------------------------------
bool IsReadable(uintptr_t addr, size_t size);
bool ReadBytes(uintptr_t addr, void* dst, size_t size);
bool WriteBytes(uintptr_t addr, const void* src, size_t size);
bool MakeWritable(uintptr_t addr, size_t size, uint32_t& oldProtect);
bool RestoreProtect(uintptr_t addr, size_t size, uint32_t oldProtect);

template <typename T>
inline bool Read(uintptr_t addr, T& out) { return ReadBytes(addr, &out, sizeof(T)); }
template <typename T>
inline bool Write(uintptr_t addr, const T& v) { return WriteBytes(addr, &v, sizeof(T)); }

inline bool ReadFloat(uintptr_t addr, float& out) { return Read<float>(addr, out); }
inline bool ReadVec3(uintptr_t addr, Vec3& out) { return ReadBytes(addr, &out, sizeof(Vec3)); }
inline bool WriteVec3(uintptr_t addr, const Vec3& v) { return WriteBytes(addr, &v, sizeof(Vec3)); }
inline bool IsFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// ---- scanning -------------------------------------------------------------
// Scan a single readable range for an exact byte pattern. `mask` uses 'x' for
// match and '?' for wildcard (one char per pattern byte). Stops after maxHits.
void ScanBytes(const Region& r, const uint8_t* pattern, const char* mask, size_t len,
               std::vector<uintptr_t>& hits, size_t maxHits);

// Convenience: scan every readable region (optionally only writable or only
// executable ones).
void ScanAll(bool writableOnly, bool executableOnly, const uint8_t* pattern, const char* mask,
             size_t len, std::vector<uintptr_t>& hits, size_t maxHitsPerRange);

// Find a run of `count` consecutive float32 values equal to `values[i]` within
// abs tolerance `tol`, 4-byte aligned. Used to locate missile parameter tables.
void FindFloatRuns(const Region& r, const float* values, const float* tols, int count,
                   std::vector<uintptr_t>& hits, size_t maxHits);

// Collect every x86-64 instruction operand address of the form
//   48 8B 05 <disp32>      mov rax, [rip+disp]     / 4C 8B xx <disp32> etc.
//   48 8D 0D <disp32>      lea rcx, [rip+disp]
// i.e. RIP-relative references, and return the absolute target addresses.
// Used to find code that references a discovered parameter table.
void CollectRipRelativeRefs(const Region& code, uintptr_t tableLo, uintptr_t tableHi,
                            std::vector<uintptr_t>& refs, size_t maxHits);

// Best-effort module lookup for an address (for logging).
bool ModuleForAddress(uintptr_t addr, ModuleInfo& out);

std::string Hex(uintptr_t v);

} // namespace acz::mem
