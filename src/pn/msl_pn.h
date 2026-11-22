// ACZPropNav - plain-MSL proportional navigation (Run B result).
//
// The plain standard missile is steered by FUN_14013ECA0 (RVA 0x13ECA0): it is the
// WORLD weapon manager's per-frame tick (ctrl = world+0x6A130, one call per frame)
// and it loops the 16 live-missile slots (mask u32 at ctrl+0x6E8, slot =
// ctrl+0x158 + i*0x58).  Run B's hardware watch proved that this function reads the
// live .wpb record fields P+0x2C (lead) and P+0x4A (startAng) every frame, and that
// it integrates each missile's position.  The game recomputes the per-frame
// displacement (slot+0x20..0x2C) every frame and adds it to the position
// (slot+0x00..0x0C), so steering = post-update position correction: move the
// missile to where the PN displacement (same speed, limited turn) would have put it.
//
// Guidance (STEP 2): true lambda-dot proportional navigation with navigation
// constant N = 4, blended with a one-frame pure-pursuit demand by
// HomingForesightAmount: 0 = pure pursuit (vanilla-like chase), 1 = full PN
// (see pn/msl_steer.h::ComputePn).  A soft rmin = max(2 frames of travel, 25 m)
// singularity guard replaces the old terminal range/tGo hardcodes.
//
// Strategy (STEP 1B): hook the function entry (15-byte verbatim-relocatable
// prologue), run the original through the trampoline first (so vanilla integration,
// lifetime, seeker cone and target bookkeeping stay intact), then for every live slot
// whose owner is the player (or an enemy) and whose configuration asks for
// HomingForesightAmount:
//   * vGame is read straight from slot+0x20..0x28 (the step the game just applied);
//   * the only per-slot state is a unit `dir` = the heading we commanded last frame,
//     used as the guidance basis (dir * |vGame|), so the PN rotation accumulates;
//     the per-slot cache is keyed by the SLOT ROW index (0..15), NOT the owner
//     (obj,part): the launcher attributes every missile of one aircraft to the same
//     owner pair, so an owner-keyed entry collided between two live slots and made
//     one of two missiles overshoot and circle (fixed 2026-10-27; see msl_pn.cpp);
//   * slot+0x00..0x08 receives p + (vPN - vGame) (pos.w +0x0C stays 1.0f);
//   * the slot ORIENTATION is taken over too: slot+0x10 is Euler angles (3 floats +
//     4th), rotated by the game's own FUN_1402036D0(out, in, rotvec) with the rotvec
//     from the game's heading to the PN direction, writing +0x10/+0x14/+0x18 only;
//   * the matrix forward-axis convention and rotation-vector frame/sign are
//     auto-calibrated once per run on scratch copies with FUN_140202FA0; if that is
//     ambiguous the orientation writes are disabled and position steering remains.
// slot+0x20 is refreshed for intra-frame consistency.  Read-only for everything
// else; DryRun computes and logs without writing.
#pragma once

#include <cstdint>

#include "core/config.h"

namespace acz {
namespace msl_pn {

// Step 1B: the game's Euler helpers (pure math, called from the post-update hook).
//   FUN_140202FA0(float out_mat16[16], const float euler[4])
//   FUN_1402036D0(float out_euler[4], const float in_euler[4], const float rotvec[3])
// Verified offline against work/acz_copy.exe.unpacked.exe; the runtime byte check in
// msl_pn.cpp uses these constants and disables the orientation path on a mismatch.
constexpr uint32_t kRvaEulerToMatrix = 0x202FA0;
constexpr uint32_t kRvaRotateEuler = 0x2036D0;
constexpr uint8_t kEulerToMatrixHead[21] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89,
                                            0x7C, 0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC,
                                            0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
constexpr uint8_t kRotateEulerHead[18] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89,
                                          0x7C, 0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC,
                                          0x48, 0x83, 0xEC, 0x70};

// True when the INI asks for plain-MSL PN: GuidanceOverride=1, at least one side
// has a positive HomingForesightAmount for "msl", and neither diagnostic probe
// (Probe=1 / HwWatch=1) owns this run.
bool Wanted(const Config& cfg);

// Arm the deferred hook on FUN_14013ECA0 and log the amounts in use. Called once
// from Patcher::Start; the actual Install happens from Tick (retry every 200 ms
// until SteamStub has decrypted .text, 60 s cap).
void Configure(const Config& cfg, uintptr_t exeBase);

// Called from Patcher::Tick: deferred install, queued per-correction detail lines,
// and the 2 s summary. Never logs from inside the hook itself.
void Tick();

// Restore the patched prologue (Patcher::Shutdown).
void Shutdown();

} // namespace msl_pn
} // namespace acz
