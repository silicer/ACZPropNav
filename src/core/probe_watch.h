// ACZPropNav - "Run B" hardware read-watch probe (INI [runtime] HwWatch = 1).
//
// Read-only follow-up to Run A (core/probe.h). Run A proved that a pool node's
// +0x28 points at the flat .wpb parameter record P of the firing weapon:
//   P-0x10 data_off=0x10, P+0x10 scope=1600, P+0x14 range=3000, P+0x24 timeStep=4,
//   P+0x2C lead=40, P+0x44 speed=1000, P+0x48 type=2, P+0x49 attack=40,
//   P+0x4A startAng=90.
// Run B answers the last question: WHICH CODE reads P+0x2C (lead) / P+0x4A
// (startAng) while a plain MSL flies.  It does that with x86-64 hardware data
// breakpoints (Dr0..Dr3) on four fields of P, armed one-shot on every game thread:
// the vectored exception handler records {slot, Rip, registers, thread} and clears
// the debug registers of the faulting thread, and the tick re-arms every ~200 ms.
//
// No game memory is written. The only side effect is Dr0..Dr3/Dr7 on other
// threads while the measurement window is open; Finish()/Shutdown() clear them.
//
// It also captures (read-only) the live WeaponModel allocation from the pool
// allocator, and logs the moving world object/part and the plain-MSL pool nodes
// so the flying missile's object is identified at the same time.
#pragma once

#include <cstdint>

#include "hook/hook.h"

namespace acz {

class ProbeWatch {
public:
    // seconds = measurement window (INI [runtime] HwWatchSeconds, default 25).
    // installModelHook=false when the Run A probe already owns the allocator hook
    // (both probes must never patch the same prologue).
    void Start(uintptr_t exeBase, int seconds, bool installModelHook);
    void Tick();
    void Shutdown();
    bool Active() const { return active_; }
    bool Finished() const { return finished_; }

private:
    bool ResolveChain();       // MC -> objArray -> playerObj -> part -> fire -> P
    void LogChain();
    void LogPBlock();
    void StartArming(double now);
    void ArmAllThreads(bool disarm);
    void DrainHits();
    void MoveSnapshotTick(double now);
    void PoolNodesLog();
    void AllocCaptureTick();
    void DumpWeaponModelPayload(uintptr_t obj, uint32_t size);
    void TickInstallHook(double now);
    bool InstallAllocHook();
    void CheckEngineFrame(double now);
    void LogResolveFailure();
    void VtableScan();
    void Heartbeat(double now);
    void Finish(const char* reason);

    bool active_ = false;
    uintptr_t base_ = 0;
    size_t moduleSize_ = 0;
    int seconds_ = 25;
    double start_ = 0.0;

    bool pFound_ = false;
    bool armed_ = false;
    bool finished_ = false;
    double lastResolveTry_ = 0.0;
    int resolveTries_ = 0;
    double armedAt_ = 0.0;
    double lastArmSweep_ = 0.0;
    int armSweeps_ = 0;
    int armFailures_ = 0;
    int threadsArmed_ = 0;
    void* vehHandle_ = nullptr;

    // resolved chain
    uintptr_t mc_ = 0;
    uintptr_t mcFromS_ = 0;
    uintptr_t objArray_ = 0;
    uintptr_t simBase_ = 0;
    uintptr_t playerObj_ = 0;
    uintptr_t part_ = 0;
    uintptr_t fire_ = 0;
    uintptr_t P_ = 0;
    int playerIdx_ = -1;
    int objCount_ = 0;
    int usedIdx_ = -1;
    uint8_t pBytes_[0x60] = {};
    float pScope_ = 0.f, pRange_ = 0.f, pTimeStep_ = 0.f, pLead_ = 0.f;
    uint32_t pSpeed_ = 0, pDataOff_ = 0;
    uint8_t pType_ = 0, pAttack_ = 0, pStartAng_ = 0;

    // hits (ring itself lives in the .cpp, shared with the VEH)
    bool hitSeen_[512] = {};
    struct RipEntry {
        uint64_t rip = 0;
        int slot = 0;
        int count = 0;
        uint64_t rax = 0, rcx = 0, rdx = 0, r8 = 0, r9 = 0, rsp = 0;
        uint32_t tid = 0;
    };
    static constexpr int kMaxRips = 64;
    RipEntry rips_[kMaxRips];
    int ripCount_ = 0;
    int rawHitsLogged_ = 0;
    long long totalHitsSeen_ = 0;

    // movers (world objects)
    static constexpr int kMaxObj = 32;
    double lastMoveSnap_ = 0.0;
    int moveSnaps_ = 0;
    struct PrevObj {
        uint64_t obj = 0;
        float pos[4] = {};
        bool valid = false;
    };
    PrevObj prev_[kMaxObj];

    // plain-MSL pool nodes (filtered by node+0x28 == P)
    uintptr_t poolNodes_[8] = {};
    int poolNodeCount_ = 0;

    // WeaponModel allocation capture (own allocator hook; only caller 0xC767D)
    bool hookWanted_ = false;
    bool hookInstalled_ = false;
    bool hookGaveUp_ = false;
    double lastHookTry_ = 0.0;
    int hookTries_ = 0;
    void* hookTarget_ = nullptr;
    hook::InstallResult hookResult_;
    bool allocSeen_[256] = {};
    int allocEvents_ = 0;
    uintptr_t wmDumped_[8] = {};
    int wmDumpedCount_ = 0;
    int wmDumpRows_ = 0;

    // engine frame counter (the dispatcher's only writer is exe+0x33ED94): proves
    // whether FUN_1401B9A80 runs at all, i.e. whether the plain MSL uses it.
    uint32_t engineFrame0_ = 0;
    uint32_t engineFrameNow_ = 0;
    double frameCheckAt_ = 0.0;
    bool frameSampleTaken_ = false;
    bool frameChecked_ = false;
    bool frameFrozen_ = false;

    // vtable presence scan (one-shot, run before the measurement window)
    bool vtableScanned_ = false;

    double lastHeartbeat_ = 0.0;
};

} // namespace acz
