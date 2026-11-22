// ACZPropNav - configuration model, 100% source-compatible with AC8PropNav.ini syntax.
//
// Syntax (identical to the AC8 mod, see ACZPropNav.ini in the repo root):
//
//   [player]
//   msl = {                       ; block form
//       HomingForesightAmount = 0.7
//       LockonRange           =
//       MaxRotationAngle      =
//       MaxHomingAngle        =
//       LifeTime              =
//   }
//   qaam = 1.0                    ; legacy shorthand == only HomingForesightAmount
//
//   [enemy]
//   msl = { HomingForesightAmount = 0.3 }
//
//   ; comments start with ';' or '#', trailing comments ("value ; note") are allowed
//   ; blank value (or no '=')  => keep vanilla for that property
//   ; property names are case-insensitive
//   ; unknown property  => warning, ignored
//   ; unknown section   => warning, ignored  (extension sections below are known)
//
// Extension sections (documented as ACZPropNav additions; the AC8 syntax above
// keeps working unchanged when they are absent):
//
//   [runtime]
//   LogLevel     = 2      ; 0 silent, 1 normal, 2 verbose
//   DryRun       = 0      ; 1 = detect & log, never write to the game
//   Probe        = 0      ; 1 = one-shot memory probe, writes ACZPropNav.key.log
//   RingSurvey   = 0      ; 1 = read-only observation of the missile smoke-emitter
//                         ;     callers (finds the plain-MSL per-frame update)
//   TickHz       = 120    ; guidance override rate
//   RequireVerifiedOffsets = 1
//   GuidanceOverride = 1  ; 1 = apply real proportional-navigation lead when the
//                         ;     missile entities were located, 0 = static params only
//   EntityPosOffset     =   ; optional manual calibration (hex or decimal bytes)
//   EntityVelOffset     =
//   TargetPosOffset     =
//   TargetVelOffset     =
//   ForceOffsets        = 0 ; 1 = trust the manual offsets without verification
//
//   [vanilla]              ; same block syntax as [player]; declares the game's
//   msl = {                ; ORIGINAL values so the runtime scanner can find the
//       MaxHomingAngle    = 75     ; parameter tables by value. Only needed when
//       MaxRotationAngle  = 40     ; the built-in table (docs/VANILLA.md) is wrong
//       LifeTime          = 9      ; for an updated game build.
//   }
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acz {

enum class Side { Player = 0, Enemy = 1 };
const char* SideName(Side s);

// One missile model's overrides for one side. std::nullopt == "keep vanilla".
struct FieldSet {
    std::optional<float> homingForesightAmount; // 0..1    proportional lead, main parameter
    std::optional<float> maxHomingAngle;        // degrees seeker field of view
    std::optional<float> maxRotationAngle;      // deg/s   turn rate
    std::optional<float> lifeTime;              // seconds
    std::optional<float> lockonRange;           // metres (player only)

    bool Any() const {
        return homingForesightAmount || maxHomingAngle || maxRotationAngle || lifeTime || lockonRange;
    }
    int Count() const {
        return (homingForesightAmount ? 1 : 0) + (maxHomingAngle ? 1 : 0) + (maxRotationAngle ? 1 : 0) +
               (lifeTime ? 1 : 0) + (lockonRange ? 1 : 0);
    }
};

struct MissileModel {
    std::string key;      // lower-case model keyword, e.g. "msl", "4aam"
    FieldSet player;
    FieldSet enemy;
    int sourceLine = 0;
};

struct RuntimeSection {
    // Verbosity, and it must actually do something (it previously did not - see log.cpp).
    //   logLevel 0 = no log files at all
    //   logLevel 1 = default, player-facing: startup, hook status, warnings, errors only
    //   logLevel 2 = verbose: adds the per-2-second telemetry and per-correction lines
    int logLevel = 1;
    bool dryRun = false;
    bool probe = false;
    int tickHz = 120;
    bool requireVerifiedOffsets = true;
    bool guidanceOverride = true;
    std::optional<uint32_t> entityPosOffset;
    std::optional<uint32_t> entityVelOffset;
    std::optional<uint32_t> targetPosOffset;
    std::optional<uint32_t> targetVelOffset;
    bool forceOffsets = false;
    // How HomingForesightAmount is turned into the engine `lead` value.
    //   add      : lead = vanilla + amount * leadScale          (default)
    //   blend    : lead = vanilla * (1 - amount) + leadScale * amount
    //   absolute : lead = amount * leadScale
    float leadScale = 260.f;      // frames of lead added at amount = 1.0 (60 Hz)
    std::string leadMode = "add"; // add | blend | absolute
    // Navigation constant N for the plain-MSL true-PN law (docs/MSL_PN.md).
    //   3..5 typical; higher = more aggressive early turn ("more lead-like"),
    //   lower = smoother / closer to pure chase.  Plain-MSL path only.
    float pnGain = 4.0f;
    // Plain-MSL terminal hand-back radius, in metres.  >= 0.
    //   0 (default) = PN owns the whole flight, start to impact: the rmin guard in
    //       ComputePn is the only close-range handling needed (see docs/MSL_PN.md).
    //   >0 = inside this range the vanilla guidance finishes the approach.  Was
    //       hard-coded to 30 m as a workaround for the 2026-10-27 two-missile bug;
    //       that bug is fixed at the cache-key level, and this hand-back changes
    //       no engagement outcome, so it is now opt-in for A/B comparison only.
    float terminalZone = 0.f;
    // Diagnostic: observe the EFF_MIS_POSDIR ring writers (the tail-smoke emitters)
    // to learn which code advances a plain-MSL missile each frame (docs/MSL_PN.md).
    // Read-only: the detours run the original function first, then log.
    int ringSurvey = 0;   // 0 = off, 1 = hook exe+0x3B300 / 0x3B370 and log callers
    // Run B: hardware read-watch probe (docs/ARCHITECTURE.md). 0 = off.
    // Installs x86-64 data breakpoints on the live .wpb record to find which code
    // reads `lead` / `startAng` while a plain MSL flies. Read-only.
    int hwWatch = 0;          // 1 = enabled
    int hwWatchSeconds = 25;  // measurement window per run
    // Guided-hook calibration (see docs/CALIBRATION.md). All optional.
    std::optional<uint32_t> hookRva;        // module-relative address of the missile update
    std::string hookSignature;              // byte pattern "48 8B 05 ?? ?? ?? ??" fallback
    int hookArgIndex = 1;                   // 1 = rcx, 2 = rdx, 3 = r8, 4 = r9
    bool entityOverride = false;            // experimental: velocity rewrite without a hook
};

struct Config {
    std::string sourcePath;        // utf-8 path the config was read from
    std::vector<MissileModel> models;
    std::vector<MissileModel> vanilla;  // [vanilla] section: discovery signatures
    std::vector<std::string> warnings;  // non-fatal parse/semantic problems
    RuntimeSection runtime;
    bool loaded = false;

    // Parse `path`. Never throws. Returns false only when the file cannot be read
    // or is structurally unusable (wrong build marker etc.); recoverable problems
    // land in `warnings` and parsing continues.
    bool LoadFromFile(const std::wstring& path);

    // Resolve `typeName` (a game weapon identifier such as "MSL", "QAAM",
    // "MSSL_STD", ...) against the configured models for `side`.
    // Matching rules: case-insensitive; a configured key matches when it is a
    // substring of typeName; the LONGEST matching key wins (so "4aam" is never
    // stolen by "aam"); first declared model wins on equal length.
    // Returns nullptr when nothing matches.
    const FieldSet* Resolve(Side side, const std::string& typeName) const;

    // Human readable warnings joined with " | ".
    std::string WarningSummary() const;

    // Post-parse sanity clamps (HomingForesightAmount 0..1, angles 0..360,
    // rotation 0..720 deg/s, lifeTime 0.1..120 s, lockonRange 0..100000 m).
    // Clamped values are recorded as warnings.
    void Sanitize();
};

// Parse helpers exposed for the unit tests.
bool ParseFloatValue(const std::string& text, float& out);   // trims, allows "0.7", "1", "1.0f"
void NormalizePropertyName(std::string& name);               // lowercase, strips spaces/underscores

} // namespace acz
