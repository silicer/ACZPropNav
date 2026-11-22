// ACZPropNav - applies the configuration to the running game.
//
// Two layers, both driven by ACZPropNav.ini:
//
//  1. Static parameter patch (always on, safe): the discovery engine locates the
//     game's missile parameter rows and we write MaxHomingAngle / MaxRotationAngle /
//     LifeTime / LockonRange into them. This alone already changes the missiles.
//
//  2. Guidance override (proportional navigation): HomingForesightAmount cannot be
//     expressed as a static parameter, because the vanilla game does not lead its
//     targets at all. Once the missile update function is known (calibrated hook,
//     see docs/CALIBRATION.md) or the missile entities are correlated, every update
//     is re-steered toward the PN intercept point, rate-limited by MaxRotationAngle
//     and gated by MaxHomingAngle — exactly the semantics of the AC8 mod.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/probe.h"
#include "core/probe_watch.h"
#include "hook/hook.h"
#include "mem/discovery.h"

namespace acz {

struct PatchStats {
    int anchorHits = 0;
    int hypotheses = 0;
    int rowsFound = 0;
    int rowsVerified = 0;
    int rowsWritten = 0;
    int fieldsWritten = 0;
    int entitiesFound = 0;
    int entitiesVerified = 0;
    bool guidanceActive = false;
    long long guidanceCalls = 0;
    long long guidanceApplied = 0;
    long long guidanceSkipped = 0;
    std::string guidanceDetail;
};

class Patcher {
public:
    Patcher();
    ~Patcher();

    // gameDir: the ACZero folder (for diagnostics only; the log is already open).
    bool Start(const Config& cfg, const std::wstring& gameDir);

    // Called by the mod tick thread (default 8 ms).
    void Tick();

    // One-shot deep dump to ACZPropNav.key.log.
    void Probe();

    void Shutdown();

    const PatchStats& Stats() const { return stats_; }

    // Called from the calibrated guidance detour (see patcher.cpp).
    void OnGuidanceHookHit(uintptr_t self);

    // Called from the PN integrator hook. `slot` is the engine object passed in
    // rdx; `caller` is the return address inside the game (which call site reached
    // the integrator). docs/GUIDANCE_ASM.md §2/§10.
    void OnMissileSlotUpdated(uintptr_t slot, uintptr_t caller);

private:
    bool InstallIntegratorHook(bool quiet = false);
    void RunDiscovery(bool force = false);
    // Exact-record scan: the shipping missile records are loaded verbatim from
    // data/mis/mpl/*.wpb, so we can find them by their byte pattern and patch
    // the documented field offsets directly (docs/VANILLA.md). Falls back to the
    // value-correlator when the pattern is not present (e.g. a data mod changed
    // the records).
    bool ScanVanillaRecords();
    // SAFE probe (Probe=1): temporarily hook the missile dispatcher and read only
    // the controller object the game itself passes in. Earlier versions scanned
    // heaps for WeaponModel instances and chased pointers found that way - that
    // crashed the game (docs/VERIFICATION.md §8), so those probes are gone.
    void ProbeViaDispatcher();
    // Probe: observe the missile dispatcher (exe+0x9DBB0) with a safe detour that
    // forwards rcx/rdx/r8/r9 unchanged. Logs each controller's RTTI class and live
    // mask, and dumps the slots of any controller that has live missiles - this is
    // how we find out whether the plain MSL has its own controller in this loop.
    void ProbeDispatcher();
    bool dispProbe_ = false;
    int dispCalls_ = 0;
public:
    // Called from the probe detour (dispatcher observer).
    void OnDispatcherObserved(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4);
private:
    // Read-only live probe of the projectile pool that holds plain-MSL missiles
    // (docs/MSL_GUIDANCE.md): singleton at exe+0x3219A0, live intrusive list at
    // S+0x20, 0x70-byte nodes with links at +0x00/+0x08. Samples several frames and
    // reports which 3-float triples advance (position) vs. move by a constant vector
    // (velocity) - that is what identifies the MSL guidance fields without a hook.
    void ProbeProjectilePool();
    void StartPoolProbe();
    // Route A (docs/MSL_GUIDANCE.md): the EFF_MIS_POSDIR ring holds {pos, dir} for
    // every visible missile each frame. We locate the ring object by its RTTI vtable
    // and sample its raw contents (read-only) to learn the exact entry layout and to
    // watch MSL's per-frame position/direction - without any hook.
    void StartRingProbe();
    // Read-only probe for the HUD lock-on indicator (CIndicatorLockonInfo, vtable
    // rva 0x2ADDD0). If it holds the player's locked target object, we can read the
    // target's position/velocity for player missiles without resolving indices.
    // One scan + one level of guarded pointer inspection, no chasing.
    void ProbeLockIndicator();
    // Automated "who writes this missile node" finder: arms a PAGE_GUARD on the page
    // holding a live node field, records the faulting instruction (RIP) in a VEH
    // handler, and reports it as exe+RVA. Replaces a debugger watchpoint.
    void ArmPageGuard(uintptr_t addr, size_t len);
    void TickPageGuard();
    void DisarmPageGuard();
    bool guardArmed_ = false;
    uintptr_t guardPage_ = 0;
    size_t guardPageSize_ = 0;
    uint32_t guardOldProtect_ = 0;
    uintptr_t guardWatchAddr_ = 0;
    size_t guardWatchLen_ = 0;
    void* guardHandler_ = nullptr;
    uintptr_t guardRip_ = 0;
    uintptr_t guardFault_ = 0;
    int guardHits_ = 0;
    void ProbeRingSample();
    bool ringProbeActive_ = false;
    int ringSamples_ = 0;
    int ringCycles_ = 0;
    double ringLastSample_ = 0.0;
    std::vector<uintptr_t> ringObjects_;
    std::vector<std::vector<float>> ringPrev_;
    bool poolProbeActive_ = false;
    int poolSamples_ = 0;
    int poolCycles_ = 0;
    double poolLastSample_ = 0.0;
    struct PoolNodeSample {
        uintptr_t node = 0;
        float f[32] = {};  // last full node image (floats)
        uint8_t state = 0;
        int samples = 0;
    };
    std::vector<PoolNodeSample> poolNodes_;
    // Diagnostic helpers kept for reference only; NOT called any more because they
    // dereference addresses found by value/vtable scanning (see §8).
    void ProbeRttiClasses();
    void ProbeValueNeighborhoods();
    void ProbeMissileSlots();
    // Resolve a "48 8B 05 ?? ?? ??" style byte pattern inside the main module's
    // executable sections. Returns 0 when not found.
    uintptr_t ResolveSignature(const std::string& pattern);
    void ApplyStaticParams();
    void DiscoverEntities();
    bool InstallGuidanceHook();
    void TearDownGuidance();
    void ApplyGuidanceToEntity(uintptr_t entity, const char* source);
    void LogRows() const;
    bool ResolveRowSettings(const game::ParamRow& row, Side side, const FieldSet*& out) const;
    float LastWritten(uintptr_t addr) const;
    void RememberWrite(uintptr_t addr, float value);

    Config cfg_;
    PatchStats stats_;
    std::wstring gameDir_;

    // "Run A" live probe (INI [runtime] Probe = 1): read-only survey of the
    // plain-MSL pipeline (core/probe.h, docs/PROBE_RUN_A.md).  Driven from Tick().
    ProbeModel probe_;                     // active only when Probe = 1
    // "Run B" hardware read-watch probe (INI [runtime] HwWatch = 1): finds which
    // code reads the live .wpb `lead`/`startAng` fields (docs/PROBE_RUN_B.md).
    ProbeWatch watch_;                     // active only when HwWatch = 1

    std::vector<game::ParamRow> rows_;
    std::vector<std::string> discoveryDiag_;
    std::vector<game::EntityCandidate> entities_;
    std::vector<std::pair<uintptr_t, float>> lastWrites_;  // address -> value we wrote
    int staleRows_ = 0;                                    // rows whose data was reloaded

    game::EntityLayout layout_;
    bool layoutVerified_ = false;
    uintptr_t pendingEntity_ = 0;
    double pendingStamp_ = 0.0;

    bool hookInstalled_ = false;
    void* hookTarget_ = nullptr;          // the address we patched (for hook::Remove)
    hook::InstallResult hookResult_;      // original bytes / stolen length
    bool probeHookMode_ = false;  // Probe: dispatcher hooked for observation only
    int probeHookCalls_ = 0;
    bool probeIdleLogged_ = false;
    int probeFramesLive_ = 0;                 // frames where at least one missile was alive
    std::vector<uintptr_t> probeCtrlDumped_;  // controllers whose body we already dumped
    // The static-parameter path only runs when the INI declares [vanilla] values.
    // On the shipping build the .wpb records are re-serialised into engine structs
    // (docs/RUNTIME_PARAMS.md), so scanning for them is a guaranteed miss - and an
    // expensive one (multi-GB, tens of seconds). Opt-in only.
    bool staticPatch_ = false;

    // --- plain-MSL survey: who feeds EFF_MIS_POSDIR? (docs/MSL_TARGET.md) ----
    // The EFF_MIS_POSDIR ring receives {pos, dir} for every missile each frame, so
    // the call site that pushes it IS inside the per-frame advance of a flying
    // missile. exe+0x3B300 / 0x3B370 are the two MIS_POSDIR ring-A "Add" methods
    // (vtable slots 2 and 3 of the ring at missionControl+0x240). We detour them
    // read-only: run the original body through the trampoline, then log the return
    // address (= the game function that computed this frame's missile motion).
    void RingSurveyTick(double now);
    bool InstallRingSurvey();
public:
    // Called from the ring detours. `caller` is the game's return address.
    void OnRingPushed(uintptr_t ring, uintptr_t idx, uintptr_t src, uintptr_t caller);
private:
    bool ringSurveyWanted_ = false;
    bool ringSurveyInstalled_ = false;
    double ringSurveyLastTry_ = 0.0;
    int ringSurveyTries_ = 0;
    // Shared with the detour, which may in principle run on more than one thread:
    // fixed arrays + Interlocked only (no vector growth, no locks, no allocator in
    // the hot path -- a fault taken while holding a lock would hang the game).
    struct RingCallerInfo {
        volatile long long caller = 0;   // 0 = free slot
        volatile long long count = 0;
        volatile long logged = 0;
        volatile long long lastSrc = 0;
        volatile long uniqueSrc = 0;
    };
    static constexpr int kRingCallerSlots = 32;
    RingCallerInfo ringCallers_[kRingCallerSlots];
    volatile long ringCallerCount_ = 0;
    static constexpr int kRingSrcSlots = 128;
    volatile long long ringSrcSeen_[kRingSrcSlots] = {};
    volatile long ringSrcCount_ = 0;
    volatile long long ringPushCount_ = 0;
    double lastRingSummary_ = 0.0;
    // --- proportional-navigation integrator hook (docs/GUIDANCE_ASM.md §10) ---
    void* pnThunk_ = nullptr;   // RWX stub: call trampoline, then our handler
    bool pnHooked_ = false;
    // Our DLL is loaded while acz.exe's imports are being resolved, i.e. BEFORE the
    // SteamStub stub decrypts .text. The hook therefore has to wait for the real
    // bytes to appear; Tick() retries every 200 ms until they do.
    bool pendingHook_ = false;
    double lastHookTry_ = 0.0;
    int hookTries_ = 0;
    struct SlotHistory {
        uintptr_t slot = 0;
        Vec3 aim;         // target position seen last frame
        Vec3 tgtVel;      // target velocity from the last full-frame sample
        float age = 0.f;  // slot+0x54: engine-maintained age in seconds (= frame clock)
        float dtLast = 1.f / 60.f;  // frame period measured from `age`
        double stamp = 0.0;
    };
    std::vector<SlotHistory> slotHistory_;  // keyed by MissileSlot address
    long long pnFrames_ = 0;
    int pnSlotsSeen_ = 0;
    int pnLaunches_ = 0;   // slot age reset / first sighting = a new missile
    double lastPnSummary_ = 0.0;
    // Is `slot` really a missile slot inside a weapon-controller array? The shared
    // integrator is also reached for non-missile objects, and writing those corrupts
    // the game (observed as wrong HUD numbers), so every slot must pass this test.
    bool IsMissileSlot(uintptr_t slot, std::string* className);
    std::vector<std::pair<uintptr_t, int>> slotClassCache_;  // slot -> 1 missile / 0 not
    std::vector<std::pair<uintptr_t, int>> callersSeen_;     // caller address -> count
    int pnNonMissile_ = 0;                                   // integrator calls on non-missiles
    int pnBadGeometry_ = 0;                                  // skipped: tGo saturated
    long long pnIntegratorCalls_ = 0;                        // every entry into the hook
    double lastHookCheck_ = 0.0;                             // watchdog timer
    double lastHeartbeat_ = 0.0;                             // stats heartbeat timer
    double lastHookAnomaly_ = 0.0;                           // throttle for odd byte reads
    int hookReinstalls_ = 0;                                 // watchdog reinstall attempts

    double startStamp_ = 0.0;
    double lastDiscovery_ = 0.0;
    double lastEntityScan_ = 0.0;
    double lastParamPatch_ = 0.0;
    double discoveryBackoff_ = 3.0;  // grows while nothing is found (scan is expensive)
    int scanAttempts_ = 0;           // full discovery passes so far
    bool lightScanOnly_ = false;     // after several misses only the cheap byte scan runs
};

} // namespace acz
