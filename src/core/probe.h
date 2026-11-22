// ACZPropNav - "Run A" live probe (INI [runtime] Probe = 1).
//
// Read-only survey of the plain-MSL projectile pipeline.  It is the experiment
// specified in docs/PROBE_RUN_A.md: two observation hooks plus a Tick sampler.
//
//   * hook exe+0x201460  pool Allocate(rcx=pool, edx=size, r8d=align) -> rax
//     Records {size, returned ptr} only for the two caller return addresses we
//     care about (WeaponModel registration exe+0xC767D, mission singleton
//     exe+0x16E14B).  Everything else is a handful of instructions (two compares).
//
//   * hook exe+0x1B2C8  node pop.  The documented entry exe+0x1B2C0 is NOT
//     hookable (its 4th byte is `7F 03` = jg rel8, a relative branch; the engine
//     refuses non-relocatable prologues, and the function has no .pdata entry
//     either).  exe+0x1B2C0+8 is the jg's own target: 14 relocatable bytes, and
//     every non-null return of the function flows through it (the only paths that
//     skip it return NULL).  ABI there: rcx = the pool singleton, rax = node.
//     We record the node pointer and the caller's return address (one of the six
//     known spawn functions).
//
//   * Tick sampler (read-only, every mem::Read* is SEH guarded):
//       - global time-sorted node list at exe+0x33EDB0, once per second,
//       - up to 8 captured pool nodes x 12 samples, changed u32 offsets,
//       - singleton S = exe+0x3219A0 list heads / counts,
//       - 0x200-byte dump of the parameter object found at node+0x28,
//       - WeaponModel (vftable exe+0x2AE790) scan after the mission singleton
//         appears, and the R3 anchored-delta tally (one pass, bounded).
//
// Nothing here writes game memory except the two code patches themselves; no
// PAGE_GUARD, no pointer chasing, every address validated first.  The detour
// bodies never log and never allocate: fixed arrays + Interlocked only.
#pragma once

#include <cstdint>

#include "hook/hook.h"

namespace acz {

class ProbeModel {
public:
    // Called once from Patcher::Start when [runtime] Probe = 1.
    void Start(uintptr_t exeBase);
    // Called from Patcher::Tick (mod thread, ~120 Hz).  All logic lives here.
    void Tick();
    // Restores the code patches (Patcher::Shutdown).
    void Shutdown();
    bool Active() const { return active_; }

private:
    void TickInstall(double now);
    bool InstallAllocHook();
    bool InstallPopHook();
    bool InstallWorldHook();
    void DrainCaptures();
    void NodeSampleTick(double now);
    void SingletonSnapshot();
    void GlobalListTick(double now);
    void AddSampleNode(uintptr_t node);
    void DumpParamObject(uintptr_t node, uintptr_t paramPtr);
    void DumpMarkedObject(const char* tag, uintptr_t obj, uint32_t from, uint32_t to);
    void WeaponModelScan();
    void R3ScanAndTally();
    void Heartbeat(double now);

    bool active_ = false;
    uintptr_t base_ = 0;
    double start_ = 0.0;

    // --- deferred hook installation (SteamStub decrypts .text after load) -----
    double lastInstallTry_ = 0.0;
    int installTries_ = 0;
    bool allocHooked_ = false;
    bool popHooked_ = false;
    bool worldHooked_ = false;
    bool allocGaveUp_ = false;
    bool popGaveUp_ = false;
    bool worldGaveUp_ = false;
    bool allocRefusalLogged_ = false;
    bool popRefusalLogged_ = false;
    bool worldRefusalLogged_ = false;
    void* allocTarget_ = nullptr;
    void* popTarget_ = nullptr;
    void* worldTarget_ = nullptr;
    hook::InstallResult allocInstall_;
    hook::InstallResult popInstall_;
    hook::InstallResult worldInstall_;

    // --- capture drain --------------------------------------------------------
    static constexpr int kAllocCap = 256;
    static constexpr int kPopCap = 256;
    bool allocSeen_[kAllocCap] = {};
    bool popSeen_[kPopCap] = {};
    int allocLogged_ = 0;
    int popLogged_ = 0;
    bool allocOverflowLogged_ = false;
    bool popOverflowLogged_ = false;
    int allocEvents_ = 0;
    int popEvents_ = 0;
    bool singletonSeen_ = false;
    double singletonAt_ = 0.0;
    // Set when the flat .wpb record layout was confirmed at a pool node+0x28 pointer
    // (docs/WEAPON_MODEL_LAYOUT.md).  The WeaponModel scan and the R3 tally exist only
    // to *find* that layout, so both are skipped once it is proven.
    bool flatRecordSeen_ = false;

    // A WeaponModel allocation only gets its vftable written a few instructions
    // after Allocate returns, so the class test is deferred by ~0.2 s (R2).
    struct PendingWm {
        uintptr_t ptr = 0;
        uint32_t size = 0;
        double due = 0.0;
    };
    static constexpr int kPendingWm = 16;
    PendingWm pendingWm_[kPendingWm] = {};
    int pendingWmCount_ = 0;
    int wmVerified_ = 0;

    // --- node sampling --------------------------------------------------------
    static constexpr int kSampleNodes = 8;
    static constexpr int kSampleRounds = 12;
    uintptr_t sampleNodes_[kSampleNodes] = {};
    uint32_t samplePrev_[kSampleNodes][0x70 / 4] = {};
    bool sampleDead_[kSampleNodes] = {};
    int sampleNodeCount_ = 0;
    int sampleRound_ = 0;
    double lastSample_ = 0.0;

    // --- global list / world tick (exe+0x1B7B00: rcx = world, once per frame) ---
    int singletonSnaps_ = 0;
    int globalWalks_ = 0;
    int globalEmpty_ = 0;
    double lastGlobalWalk_ = 0.0;

    uintptr_t world_ = 0;          // last captured world pointer (revalidated per use)
    int worldSnaps_ = 0;           // logged snapshots (cap 120)
    int worldAttempts_ = 0;
    double lastWorldSnap_ = 0.0;
    bool worldUnreadableLogged_ = false;
    static constexpr int kWorldSnaps = 120;
    static constexpr int kWorldObjects = 64;  // scan cap (array bound for prev*[])
    uintptr_t prevObjPtr_[kWorldObjects] = {};
    float prevObjPos_[kWorldObjects][4] = {};
    bool prevObjValid_[kWorldObjects] = {};
    void WorldSnapshotTick(double now);

    // --- parameter objects seen at node+0x28 ----------------------------------
    static constexpr int kParamDumps = 8;
    uintptr_t paramPtrs_[kParamDumps] = {};
    int paramPtrCount_ = 0;
    int paramPtrOverflow_ = 0;

    // --- heavy one-shot scans -------------------------------------------------
    bool wmScanWanted_ = false;
    bool wmScanGaveUp_ = false;
    int wmScans_ = 0;
    double nextWmScan_ = 0.0;
    static constexpr int kWmDumps = 8;
    uintptr_t wmDumped_[kWmDumps] = {};
    int wmDumpedCount_ = 0;
    bool r3Wanted_ = true;   // scheduled at Start, may be re-scheduled by the event
    bool r3Done_ = false;
    double r3At_ = 0.0;

    double lastHeartbeat_ = 0.0;
    bool idleHintLogged_ = false;
};

} // namespace acz
