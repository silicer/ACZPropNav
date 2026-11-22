// ACZPropNav - runtime discovery of the game's missile parameter tables and of the
// live missile entities that use them. See docs/ARCHITECTURE.md ("Discovery
// contract") for the algorithm; this header is the contract between src/mem and
// src/core.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/acz_common.h"
#include "mem/mem.h"

namespace acz::game {

// The five tunables of the AC8 mod, in the same order as the INI.
enum class Property {
    HomingForesightAmount = 0,
    MaxHomingAngle,
    MaxRotationAngle,
    LifeTime,
    LockonRange,
    Count
};
const char* PropertyName(Property p);
const char* PropertyIniName(Property p); // "HomingForesightAmount", ...
bool PropertyFromIniName(const std::string& name, Property& out);

struct KnownValue {
    Property prop = Property::MaxHomingAngle;
    float value = 0.f;
    float tolerance = 0.02f;
};

// Vanilla values of one missile model, as shipped in docs/VANILLA.md.
struct ModelSignature {
    std::string modelKey;               // "msl", "qaam", "laam", ...
    std::vector<KnownValue> values;     // >= 2 entries to be useful
};

struct DiscoveryOptions {
    std::vector<ModelSignature> models; // >= 1; 2+ enables cross-model validation
    uint32_t windowBefore = 0x40;       // bytes scanned before an anchor hit
    uint32_t windowAfter = 0x80;        // bytes scanned after an anchor hit
    size_t maxAnchorHits = 20000;       // per signature value
    size_t maxRows = 256;               // stop after this many verified rows
    int minScore = 3;                   // hits * matched properties
    bool scanImageData = true;          // include .data/.rdata of the main module
    bool scanPrivate = true;            // include MEM_PRIVATE writable regions
    // Optional hard scope: when non-empty, ONLY these byte ranges are considered
    // (empty = every writable region selected by the flags above).  Used by tests
    // and by a targeted rescan after the game reloads its data, and it keeps the
    // locator away from memory the caller already knows is not game data.
    std::vector<std::pair<uintptr_t, size_t>> onlyRanges;
};

// One parameter row found in memory: rowAddr + where each property lives inside
// the row. `slots` is ordered by Property; only found properties are present.
struct PropertySlot {
    Property prop = Property::MaxHomingAngle;
    uint32_t offset = 0;      // byte offset from rowAddr
    float foundValue = 0.f;
};

struct ParamRow {
    uintptr_t rowAddr = 0;
    std::string modelKey;
    std::vector<PropertySlot> slots;
    int score = 0;
    bool verified = false;    // survived cross-model validation
};

// A layout hypothesis shared by all rows of one model.
struct LayoutHypothesis {
    std::string modelKey;
    std::vector<PropertySlot> slots; // offsets only (foundValue unused)
    int supportedRows = 0;
    int modelsMatched = 0;
    int score = 0;
    bool verified = false;
};

// -------- stage 1: parameter tables ---------------------------------------
// Fills `rows` (verified rows only, or all when options allow) and appends
// human-readable diagnostics ("4 anchor hits, best hypothesis ...").
struct ParamTableResult {
    std::vector<ParamRow> rows;
    std::vector<LayoutHypothesis> hypotheses;
    uintptr_t tableBase = 0;   // lowest verified row, for logging / ref scans
    uint32_t stride = 0;       // common row spacing, 0 if unknown
};
bool LocateParameterTables(const DiscoveryOptions& opt, ParamTableResult& out,
                           std::vector<std::string>& diag);

// -------- stage 2: entities that reference a row --------------------------
struct EntityCandidate {
    uintptr_t addr = 0;        // entity base
    uintptr_t rowPtrAddr = 0;  // address of the field holding the row pointer
    uintptr_t rowAddr = 0;
    std::string modelKey;
};
bool FindEntities(const std::vector<ParamRow>& rows, std::vector<EntityCandidate>& out,
                  size_t maxEntities, std::vector<std::string>& diag);

// -------- stage 3: motion correlation inside an entity --------------------
// Slot offsets are relative to the entity base. `verified` means the vectors
// were confirmed across two samples with dt seconds between them.
struct EntityLayout {
    uint32_t posOff = 0;
    uint32_t velOff = 0;
    uint32_t targetPosOff = 0;
    uint32_t targetVelOff = 0;
    bool hasTarget = false;
    bool verified = false;
    float speed = 0.f;        // observed missile speed, units/s
    float targetSpeed = 0.f;
    std::string diag;
};

// Sample the entity twice `dt` seconds apart and correlate the moving vectors.
bool CorrelateMotion(uintptr_t entity, float dt, EntityLayout& out);

// Snapshot of one entity at a given tick, ready for the guidance core.
struct EntitySnapshot {
    uintptr_t entity = 0;
    Vec3 pos, vel, targetPos, targetVel;
    bool valid = false;
};
bool Snapshot(const EntityCandidate& e, const EntityLayout& layout, EntitySnapshot& out);

// -------- code references (used by the hook fallback adapter) -------------
struct CodeRefSite {
    uintptr_t insnAddr = 0;   // address of the RIP-relative instruction
    uintptr_t target = 0;     // address referenced
};
void FindCodeReferences(uintptr_t lo, uintptr_t hi, std::vector<CodeRefSite>& out,
                        size_t maxHits);

} // namespace acz::game
