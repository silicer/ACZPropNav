// ACZPropNav - runtime discovery of missile parameter tables and of the live
// missile entities that reference them.
//
// The game is Steam-packed and undocumented: no static offsets exist.  Instead we
// look for the *values* the game loaded into memory and for the structure around
// them (docs/ARCHITECTURE.md, "Discovery contract"):
//
//   stage 1  LocateParameterTables
//            a) one global byte pass per distinct vanilla value -> anchor hits
//            b) for each anchor, describe the window around it as "which other
//               properties of the same model match at which relative offset"
//               (a 64-bit mask per property per anchor address)
//            c) vote pair-wise over those masks to build layout hypotheses, then
//               extend each hypothesis with the remaining properties
//            d) validate a hypothesis against a second model's values at the
//               SAME offsets -> `verified`; derive tableBase/stride
//   stage 2  FindEntities      -> heap objects that hold a pointer to a row
//   stage 3  CorrelateMotion   -> (pos, vel, targetPos, targetVel) slots by
//                                 double-sampling and matching |dpos|/dt ~ |vel|
//   stage 3c FindCodeReferences-> RIP-relative instructions that touch the table
//
// Everything is bounded: caps on anchor hits, hypotheses, rows and entities, and
// every memory read goes through acz::mem (fault-guarded).
//
// Notes / deliberate choices
// --------------------------
// * We exclude anchor hits that land inside the caller's own ModelSignature
//   storage.  Otherwise the mod's own lookup table is structurally
//   indistinguishable from the game's parameter table (same values, same layout)
//   and would be "discovered" as game data - and then written to.
// * Ambiguous signatures are rejected up front: if two vanilla values of one
//   model are within each other's tolerance (e.g. LifeTime 9.0 and 9.0) the
//   layout cannot be inferred from values alone.
// * A hypothesis is only `verified` when a second model's values match at the
//   same offsets; single-model matches are reported but never verified.  This is
//   what makes it safe to write into the game with RequireVerifiedOffsets=1.
#include "mem/discovery.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // QueryPerformanceCounter / Sleep for the two-sample motion probe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace acz::game {

// ===========================================================================
// Property <-> name mapping (declared in discovery.h; owned by this module)
// ===========================================================================

namespace {

constexpr int kPropCount = static_cast<int>(Property::Count);

const char* const kPropNames[kPropCount] = {
    "HomingForesightAmount",
    "MaxHomingAngle",
    "MaxRotationAngle",
    "LifeTime",
    "LockonRange",
};

// Priority used when two values of the same model cannot be told apart: keep the
// seeker/layout properties (they are the distinctive ones) and drop the fog.
int PropPriority(Property p) {
    switch (p) {
        case Property::MaxHomingAngle:      return 0;
        case Property::MaxRotationAngle:    return 1;
        case Property::LifeTime:            return 2;
        case Property::LockonRange:         return 3;
        case Property::HomingForesightAmount: return 4;
        default:                            return 5;
    }
}

}  // namespace

const char* PropertyName(Property p) {
    const int i = static_cast<int>(p);
    return (i >= 0 && i < kPropCount) ? kPropNames[i] : "?";
}

const char* PropertyIniName(Property p) { return PropertyName(p); }

bool PropertyFromIniName(const std::string& name, Property& out) {
    std::string n;
    n.reserve(name.size());
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '_' || c == '-') continue;
        n.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    for (int i = 0; i < kPropCount; ++i) {
        std::string k;
        for (const char* p = kPropNames[i]; *p; ++p) {
            const char c = *p;
            k.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        }
        if (k == n) {
            out = static_cast<Property>(i);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Internals
// ===========================================================================

namespace {

constexpr int kMaxWindowSlots = 64;     // per-anchor window, 4-byte granularity
constexpr size_t kHardAnchorCap = 8192; // per model, after dedupe
constexpr size_t kMaxHypotheses = 256;
constexpr uint32_t kMemPrivate = MEM_PRIVATE;
constexpr uint32_t kMemImage = MEM_IMAGE;
constexpr uint32_t kMaxRowSpan = 0x200u;

bool FloatMatches(float v, float ref, float tol) {
    // Written as two comparisons so NaN never matches.
    const float d = v - ref;
    return d <= tol && d >= -tol;
}

void PushDiag(std::vector<std::string>& diag, const std::string& line) {
    std::string s = line;
    if (s.size() > 200) {
        s.resize(197);
        s += "...";
    }
    diag.push_back(s);
}

struct Range {
    uintptr_t lo = 0;
    uintptr_t hi = 0;
};

bool InRanges(const std::vector<Range>& ranges, uintptr_t a) {
    for (const Range& r : ranges) {
        if (a >= r.lo && a < r.hi) return true;
    }
    return false;
}

struct SigEntry {
    Property prop = Property::MaxHomingAngle;
    float value = 0.f;
    float tol = 0.f;
    int group = -1;   // index into the shared value-group table
};

struct ModelSig {
    std::string key;
    std::vector<SigEntry> entries;
};

const SigEntry* FindEntry(const ModelSig& sig, int prop) {
    for (const SigEntry& e : sig.entries) {
        if (static_cast<int>(e.prop) == prop) return &e;
    }
    return nullptr;
}

// One anchor address plus, for every property of the model, which window slots
// around that address hold a matching value.  Bit s <=> byte offset s*4-before.
struct HitWin {
    uintptr_t addr = 0;
    uint64_t mask[kPropCount] = {};
    int32_t lo = 0;   // first valid window byte, relative to addr (prefix may be unreadable)
    int32_t hi = 0;   // one past the last valid window byte (relative)
};

struct ModelHits {
    std::vector<HitWin> hits;
};

struct Group {
    float value = 0.f;
    float tol = 0.f;
    std::vector<uintptr_t> hits;
};

struct CrossRow {
    std::string modelKey;
    uintptr_t rowAddr = 0;
};

struct Hyp {
    std::string key;
    std::string modelKey;
    int off[kPropCount] = {};   // -1 = property absent, else byte offset from row base
    int slots = 0;
    int support = 0;            // distinct rows matching the hypothesis
    int score = 0;              // support * slots
    bool verified = false;
    std::vector<std::string> validatedBy;
    std::vector<uintptr_t> rows;
    std::vector<CrossRow> crossRows;
};

std::string HypKey(const std::string& model, const int off[kPropCount]) {
    std::string k = model;
    k += '|';
    char buf[32];
    for (int p = 0; p < kPropCount; ++p) {
        if (off[p] < 0) continue;
        std::snprintf(buf, sizeof(buf), "%d:%d,", p, off[p]);
        k += buf;
    }
    return k;
}

std::string DescribeHyp(const Hyp& h) {
    std::string s;
    char buf[64];
    for (int p = 0; p < kPropCount; ++p) {
        if (h.off[p] < 0) continue;
        std::snprintf(buf, sizeof(buf), "%s+0x%X ", PropertyName(static_cast<Property>(p)), h.off[p]);
        s += buf;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// Signature sanitising
// ---------------------------------------------------------------------------

void SanitizeSignatures(const DiscoveryOptions& opt, std::vector<ModelSig>& sigs,
                        std::vector<std::string>& diag) {
    sigs.clear();
    for (const ModelSignature& m : opt.models) {
        ModelSig sig;
        sig.key = m.modelKey.empty() ? "?" : m.modelKey;

        for (const KnownValue& kv : m.values) {
            const int pi = static_cast<int>(kv.prop);
            if (pi < 0 || pi >= kPropCount) {
                PushDiag(diag, sig.key + ": ignored property with invalid enum value");
                continue;
            }
            if (!std::isfinite(kv.value)) {
                PushDiag(diag, sig.key + ": ignored non-finite value for " + PropertyName(kv.prop));
                continue;
            }
            float tol = kv.tolerance;
            if (!(tol >= 0.f) || !std::isfinite(tol)) tol = 0.f;
            // A tolerance comparable to the value makes every float in a window
            // match; that destroys the layout signal, so refuse the entry.
            if (std::fabs(kv.value) > 1.f && tol > 0.25f * std::fabs(kv.value)) {
                PushDiag(diag, sig.key + ": tolerance too loose for " + PropertyName(kv.prop) +
                                   " (every float would match)");
                continue;
            }
            bool dup = false;
            for (const SigEntry& e : sig.entries) {
                if (e.prop == kv.prop) dup = true;
            }
            if (dup) {
                PushDiag(diag, sig.key + ": duplicate " + PropertyName(kv.prop) + " ignored");
                continue;
            }
            SigEntry e;
            e.prop = kv.prop;
            e.value = kv.value;
            e.tol = tol;
            sig.entries.push_back(e);
        }

        // Distinguishability: values of one model must be separable by value
        // alone, otherwise a window match is ambiguous.  Keep the highest
        // priority entries; drop the ones that overlap an already kept value.
        std::stable_sort(sig.entries.begin(), sig.entries.end(),
                         [](const SigEntry& a, const SigEntry& b) {
                             const int pa = PropPriority(a.prop), pb = PropPriority(b.prop);
                             if (pa != pb) return pa < pb;
                             return a.value < b.value;
                         });
        std::vector<SigEntry> kept;
        for (const SigEntry& e : sig.entries) {
            bool clash = false;
            for (const SigEntry& k : kept) {
                if (std::fabs(e.value - k.value) <= e.tol + k.tol) {
                    clash = true;
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "%s: %s=%.3f not distinguishable from %s=%.3f, dropped",
                                  sig.key.c_str(), PropertyName(e.prop), e.value,
                                  PropertyName(k.prop), k.value);
                    PushDiag(diag, buf);
                    break;
                }
            }
            if (!clash) kept.push_back(e);
        }
        sig.entries.swap(kept);
        // Keep entries ordered by property for deterministic behaviour.
        std::stable_sort(sig.entries.begin(), sig.entries.end(),
                         [](const SigEntry& a, const SigEntry& b) {
                             return static_cast<int>(a.prop) < static_cast<int>(b.prop);
                         });

        if (sig.entries.size() < 2) {
            PushDiag(diag, sig.key + ": fewer than 2 distinguishable values, model skipped");
            continue;
        }
        sigs.push_back(sig);
    }
}

// ---------------------------------------------------------------------------
// Region selection
// ---------------------------------------------------------------------------

std::vector<mem::Region> CollectScanRegions(const DiscoveryOptions& opt) {
    std::vector<mem::Region> all;
    mem::EnumRegions(all);
    std::vector<mem::Region> out;
    for (const mem::Region& r : all) {
        if (!r.readable || !r.writable) continue;
        if (r.size < 0x1000) continue;
        if (r.type == kMemImage) {
            if (!opt.scanImageData) continue;
        } else {
            // MEM_PRIVATE and writable MEM_MAPPED: the game's loaded tables live
            // either in .data (image) or in its own heap allocations.
            if (!opt.scanPrivate) continue;
        }
        if (opt.onlyRanges.empty()) {
            out.push_back(r);
            continue;
        }
        // Hard scope: clip the region to every caller-provided byte range that it
        // overlaps.  Keeping the clip 4-byte aligned is what FindFloatRuns needs.
        const uintptr_t regionEnd = r.base + r.size;
        for (const std::pair<uintptr_t, size_t>& range : opt.onlyRanges) {
            if (range.second == 0) continue;
            const uintptr_t lo = std::max<uintptr_t>(r.base, (range.first + 3u) & ~static_cast<uintptr_t>(3u));
            const uintptr_t rangeEnd = range.first + range.second;
            const uintptr_t hi = std::min<uintptr_t>(regionEnd, rangeEnd & ~static_cast<uintptr_t>(3u));
            if (hi <= lo) continue;
            mem::Region clipped = r;
            clipped.base = lo;
            clipped.size = static_cast<size_t>(hi - lo);
            out.push_back(clipped);
        }
    }
    return out;
}

std::vector<Range> BuildSelfExclusions(const DiscoveryOptions& opt, const std::vector<ModelSig>& sigs,
                                       const std::vector<Group>& groups) {
    // Values that *we ourselves* hold must never be mistaken for game data.  In
    // production this matters twice:
    //   - opt.models comes from the caller (built from docs/VANILLA.md), and
    //   - the sanitised per-model entries / value groups are allocated right
    //     before scanning (their layout of vanilla values is, by definition,
    //     table-shaped).
    // The mod's own lookup table must never be "discovered" - and then written to.
    std::vector<Range> out;
    auto add = [&out](const void* p, size_t bytes) {
        if (!p || bytes == 0) return;
        Range r;
        r.lo = reinterpret_cast<uintptr_t>(p);
        r.hi = r.lo + bytes;
        out.push_back(r);
    };
    add(opt.models.data(), opt.models.size() * sizeof(ModelSignature));
    for (const ModelSignature& m : opt.models) {
        add(m.values.data(), m.values.size() * sizeof(KnownValue));
    }
    for (const ModelSig& s : sigs) {
        add(s.entries.data(), s.entries.size() * sizeof(SigEntry));
    }
    add(groups.data(), groups.size() * sizeof(Group));
    return out;
}

// ---------------------------------------------------------------------------
// Per-anchor window masks
// ---------------------------------------------------------------------------

int SlotCount(uint32_t before, uint32_t after) {
    const int slots = static_cast<int>((before + after) / 4u);
    return slots > kMaxWindowSlots ? kMaxWindowSlots : (slots > 0 ? slots : 1);
}

void BuildHitWin(uintptr_t addr, uint32_t before, uint32_t after, int slots,
                 const ModelSig& sig, HitWin& out) {
    out = HitWin{};
    out.addr = addr;
    const size_t windowBytes = static_cast<size_t>(before) + after;
    if (windowBytes > 256 || slots * 4 > static_cast<int>(windowBytes)) return;
    // Stack buffer on purpose: a heap copy of the window would itself look like a
    // parameter table to a later discovery pass.
    uint8_t buf[256] = {};

    bool prefixOk = (before == 0);
    if (before != 0) {
        prefixOk = (addr > before) && mem::ReadBytes(addr - before, buf, before);
    }
    if (!mem::ReadBytes(addr, buf + before, after)) return;  // anchor unreadable
    out.lo = prefixOk ? -static_cast<int32_t>(before) : 0;
    out.hi = static_cast<int32_t>(after);

    for (int s = 0; s < slots; ++s) {
        const int32_t rel = static_cast<int32_t>(s * 4) - static_cast<int32_t>(before);
        if (rel < out.lo || rel > out.hi - 4) continue;
        float f = 0.f;
        std::memcpy(&f, buf + static_cast<size_t>(s) * 4, sizeof(f));
        for (const SigEntry& e : sig.entries) {
            if (FloatMatches(f, e.value, e.tol)) {
                out.mask[static_cast<int>(e.prop)] |= (1ull << s);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Hypothesis evaluation: which rows of this model are explained by `h`?
// ---------------------------------------------------------------------------

void EvaluateHyp(Hyp& h, const ModelHits& mh, const ModelSig& sig, uint32_t before,
                 uint32_t after, int anchorSlot, int slots) {
    std::vector<uintptr_t> bases;
    for (const HitWin& hit : mh.hits) {
        for (int pa = 0; pa < kPropCount; ++pa) {
            if (h.off[pa] < 0) continue;
            if ((hit.mask[pa] & (1ull << anchorSlot)) == 0) continue;  // not an anchor for pa
            if (static_cast<uintptr_t>(h.off[pa]) > hit.addr) continue;
            const uintptr_t rowBase = hit.addr - static_cast<uintptr_t>(h.off[pa]);
            bool ok = true;
            for (int q = 0; q < kPropCount && ok; ++q) {
                if (h.off[q] < 0) continue;
                const int rel = h.off[q] - h.off[pa];
                bool checked = false;
                if (rel >= hit.lo && rel <= hit.hi - 4 && (rel % 4) == 0) {
                    const int slot = anchorSlot + rel / 4;
                    if (slot >= 0 && slot < slots) {
                        checked = true;
                        if ((hit.mask[q] & (1ull << slot)) == 0) ok = false;
                    }
                }
                if (ok && !checked) {
                    const SigEntry* e = FindEntry(sig, q);
                    float f = 0.f;
                    if (!e || !mem::ReadFloat(rowBase + static_cast<uintptr_t>(h.off[q]), f) ||
                        !FloatMatches(f, e->value, e->tol)) {
                        ok = false;
                    }
                }
            }
            if (ok) bases.push_back(rowBase);
        }
    }
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    h.rows = bases;
    h.support = static_cast<int>(bases.size());
    h.score = h.support * h.slots;
}

// ---------------------------------------------------------------------------
// Hypothesis generation (pairwise offset voting + extension)
// ---------------------------------------------------------------------------

void GenerateHypotheses(const ModelSig& sig, const ModelHits& mh, uint32_t before,
                        uint32_t after, int anchorSlot, int slots, std::vector<Hyp>& hyps) {
    for (size_t ai = 0; ai < sig.entries.size(); ++ai) {
        const int pa = static_cast<int>(sig.entries[ai].prop);
        std::vector<size_t> anchors;
        for (size_t h = 0; h < mh.hits.size(); ++h) {
            if ((mh.hits[h].mask[pa] & (1ull << anchorSlot)) != 0) anchors.push_back(h);
        }
        if (anchors.empty()) continue;

        for (size_t bi = 0; bi < sig.entries.size(); ++bi) {
            if (bi == ai) continue;
            const int pb = static_cast<int>(sig.entries[bi].prop);

            // Vote for the offset of pb relative to an anchor at pa.
            int votes[kMaxWindowSlots] = {};
            for (size_t h : anchors) {
                uint64_t m = mh.hits[h].mask[pb];
                for (int s = 0; s < slots; ++s) {
                    if ((m & (1ull << s)) != 0) ++votes[s];
                }
            }
            int best = 0;
            for (int s = 0; s < slots; ++s) {
                if (s == anchorSlot) continue;
                if (votes[s] > best) best = votes[s];
            }
            if (best <= 0) continue;

            // Every offset that ties for the best support is a candidate.  With a
            // single row per model even the correct offset only gets one vote, so
            // vote count alone cannot pick it: generate the nearest offsets (by
            // |relative| distance) and let row evaluation + cross-model
            // validation decide which layout is the table.
            std::vector<int> cand;
            for (int s = 0; s < slots; ++s) {
                if (s == anchorSlot) continue;
                if (votes[s] <= 0) continue;
                cand.push_back(s);
            }
            std::stable_sort(cand.begin(), cand.end(), [&votes, anchorSlot](int a, int b) {
                if (votes[a] != votes[b]) return votes[a] > votes[b];
                const int ra = (a - anchorSlot) * 4;
                const int rb = (b - anchorSlot) * 4;
                const int aa = ra < 0 ? -ra : ra;
                const int ab = rb < 0 ? -rb : rb;
                if (aa != ab) return aa < ab;
                return ra < rb;
            });
            const size_t candLimit = std::min<size_t>(cand.size(), 16);
            for (size_t ci = 0; ci < candLimit; ++ci) {
                const int sB = cand[ci];

                std::vector<size_t> support;
                for (size_t h : anchors) {
                    if ((mh.hits[h].mask[pb] & (1ull << sB)) != 0) support.push_back(h);
                }
                if (support.empty()) continue;

                int raw[kPropCount];
                for (int p = 0; p < kPropCount; ++p) raw[p] = -1;
                raw[pa] = 0;
                raw[pb] = (sB - anchorSlot) * 4;

                // Extend with the remaining properties: pick the offset that the
                // majority of the supporting rows agree on.
                const int need = std::max(1, static_cast<int>((support.size() + 1) / 2));
                for (size_t ci = 0; ci < sig.entries.size(); ++ci) {
                    const int pc = static_cast<int>(sig.entries[ci].prop);
                    if (pc == pa || pc == pb) continue;
                    int votesC[kMaxWindowSlots] = {};
                    for (size_t h : support) {
                        uint64_t m = mh.hits[h].mask[pc];
                        for (int s2 = 0; s2 < slots; ++s2) {
                            if ((m & (1ull << s2)) != 0) ++votesC[s2];
                        }
                    }
                    int bestC = 0;
                    int bestS = -1;
                    for (int s2 = 0; s2 < slots; ++s2) {
                        if (s2 == anchorSlot) continue;
                        if (votesC[s2] > bestC) {
                            bestC = votesC[s2];
                            bestS = s2;
                        }
                    }
                    if (bestS >= 0 && bestC >= need) raw[pc] = (bestS - anchorSlot) * 4;
                }

                // Normalise so that the lowest property of the hypothesis is at
                // offset 0 (that address becomes the reported row address).
                int minOff = 0;
                for (int p = 0; p < kPropCount; ++p) {
                    if (raw[p] >= 0 && raw[p] < minOff) minOff = raw[p];
                }
                int off[kPropCount];
                int count = 0;
                int maxOff = 0;
                for (int p = 0; p < kPropCount; ++p) {
                    off[p] = (raw[p] >= 0) ? raw[p] - minOff : -1;
                    if (off[p] >= 0) {
                        ++count;
                        if (off[p] > maxOff) maxOff = off[p];
                    }
                }
                if (count < 2) continue;
                if (static_cast<uint32_t>(maxOff) > kMaxRowSpan) continue;

                const std::string key = HypKey(sig.key, off);
                bool dup = false;
                for (const Hyp& existing : hyps) {
                    if (existing.key == key) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                Hyp h;
                h.key = key;
                h.modelKey = sig.key;
                std::memcpy(h.off, off, sizeof(h.off));
                h.slots = count;
                EvaluateHyp(h, mh, sig, before, after, anchorSlot, slots);
                if (h.support > 0) hyps.push_back(h);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Table coherence filter
// ---------------------------------------------------------------------------
//
// A real parameter table is internally consistent: consecutive rows are laid out
// at a common stride and one row's fields must fit inside that stride (otherwise
// neighbouring rows would overlap).  Coincidental table-shaped copies of the same
// vanilla values elsewhere in the process - the mod's own diagnostics, a stale
// copy from an earlier probe, another addon's config, or the test's decoy block -
// violate one of those properties.
//
// Only the largest coherent chain inside one address cluster is kept; fewer than
// two coherent rows means the values cannot be trusted as a table.

struct CoherentItem {
    uintptr_t addr = 0;
    uint32_t span = 0;
};

// `addrs[i]` / `spans[i]` describe row i.  Returns the indices of the largest
// coherent chain and the stride that chain uses.
std::vector<size_t> CoherentChain(const std::vector<uintptr_t>& addrs,
                                  const std::vector<uint32_t>& spans, uint32_t& strideOut) {
    strideOut = 0;
    if (addrs.size() < 2 || addrs.size() != spans.size()) return {};

    std::vector<CoherentItem> items;
    items.reserve(addrs.size());
    for (size_t i = 0; i < addrs.size(); ++i) {
        CoherentItem it;
        it.addr = addrs[i];
        it.span = spans[i];
        items.push_back(it);
    }
    std::sort(items.begin(), items.end(),
              [](const CoherentItem& a, const CoherentItem& b) { return a.addr < b.addr; });

    size_t bestKept = 0;
    uint32_t bestStride = 0;
    std::vector<CoherentItem> best;

    for (size_t start = 0; start < items.size();) {
        // A cluster = rows that plausibly belong to the same blob/allocation.
        size_t end = start + 1;
        while (end < items.size() && items[end].addr - items[end - 1].addr <= 0x2000) ++end;
        const size_t n = end - start;

        if (n >= 2) {
            std::vector<uint32_t> gaps;
            std::vector<size_t> gapCounts;
            for (size_t i = start; i + 1 < end; ++i) {
                const uintptr_t d = items[i + 1].addr - items[i].addr;
                if (d < 4 || d > 0x1000 || (d % 4) != 0) continue;
                bool found = false;
                for (size_t g = 0; g < gaps.size(); ++g) {
                    if (gaps[g] == static_cast<uint32_t>(d)) {
                        ++gapCounts[g];
                        found = true;
                    }
                }
                if (!found) {
                    gaps.push_back(static_cast<uint32_t>(d));
                    gapCounts.push_back(1);
                }
            }
            std::vector<uint32_t> strides;
            if (n == 2) {
                strides = gaps;  // a two-row table has exactly one candidate stride
            } else {
                for (size_t g = 0; g < gaps.size(); ++g) {
                    if (gapCounts[g] >= 2) strides.push_back(gaps[g]);
                }
                if (strides.empty()) strides = gaps;
            }

            for (uint32_t stride : strides) {
                if (stride < 4) continue;
                std::vector<CoherentItem> kept;
                for (size_t i = start; i < end; ++i) {
                    if (items[i].span > stride) continue;  // row does not fit the stride
                    bool neighbour = false;
                    if (i > start) {
                        const uintptr_t d = items[i].addr - items[i - 1].addr;
                        if ((d % stride) == 0 && (d / stride) <= 8 && items[i - 1].span <= stride) {
                            neighbour = true;
                        }
                    }
                    if (!neighbour && i + 1 < end) {
                        const uintptr_t d = items[i + 1].addr - items[i].addr;
                        if ((d % stride) == 0 && (d / stride) <= 8 && items[i + 1].span <= stride) {
                            neighbour = true;
                        }
                    }
                    if (neighbour) kept.push_back(items[i]);
                }
                if (kept.size() > bestKept) {
                    bestKept = kept.size();
                    bestStride = stride;
                    best = kept;
                }
            }
        }
        start = end;
    }

    if (best.size() < 2) return {};
    std::vector<size_t> indices;
    indices.reserve(best.size());
    for (const CoherentItem& item : best) {
        for (size_t i = 0; i < addrs.size(); ++i) {
            if (addrs[i] == item.addr) {
                indices.push_back(i);
                break;
            }
        }
    }
    strideOut = bestStride;
    return indices;
}

std::vector<ParamRow> SelectCoherentTable(const std::vector<ParamRow>& in, uint32_t& strideOut) {
    strideOut = 0;
    if (in.size() < 2) return {};
    std::vector<uintptr_t> addrs;
    std::vector<uint32_t> spans;
    addrs.reserve(in.size());
    spans.reserve(in.size());
    for (const ParamRow& r : in) {
        addrs.push_back(r.rowAddr);
        uint32_t span = 4;
        for (const PropertySlot& s : r.slots) {
            if (s.offset + 4 > span) span = s.offset + 4;
        }
        spans.push_back(span);
    }
    const std::vector<size_t> kept = CoherentChain(addrs, spans, strideOut);
    if (kept.size() < 2) return {};
    std::vector<ParamRow> out;
    out.reserve(kept.size());
    for (size_t idx : kept) out.push_back(in[idx]);
    std::sort(out.begin(), out.end(),
              [](const ParamRow& a, const ParamRow& b) { return a.rowAddr < b.rowAddr; });
    return out;
}
// ---------------------------------------------------------------------------
// Cross-model validation
// ---------------------------------------------------------------------------

// Checks `h`'s offsets against model `mb` at every row candidate of `mb`.
// Returns true if at least one row matches on >= 2 overlapping properties.
bool ValidateAgainstModel(Hyp& h, const ModelSig& mb, const ModelHits& mbHits,
                          int anchorSlot, int slots) {
    int overlap[kPropCount];
    int overlapCount = 0;
    for (int p = 0; p < kPropCount; ++p) {
        if (h.off[p] < 0) continue;
        if (!FindEntry(mb, p)) continue;
        overlap[overlapCount++] = p;
    }
    if (overlapCount < 3) return false;

    for (const HitWin& hit : mbHits.hits) {
        for (int k = 0; k < overlapCount; ++k) {
            const int q = overlap[k];
            if ((hit.mask[q] & (1ull << anchorSlot)) == 0) continue;
            if (static_cast<uintptr_t>(h.off[q]) > hit.addr) continue;
            const uintptr_t rowBase = hit.addr - static_cast<uintptr_t>(h.off[q]);
            bool ok = true;
            for (int k2 = 0; k2 < overlapCount && ok; ++k2) {
                const int r = overlap[k2];
                const SigEntry* e = FindEntry(mb, r);
                if (!e) {
                    ok = false;
                    break;
                }
                const int rel = h.off[r] - h.off[q];
                bool checked = false;
                if (rel >= hit.lo && rel <= hit.hi - 4 && (rel % 4) == 0) {
                    const int slot = anchorSlot + rel / 4;
                    if (slot >= 0 && slot < slots) {
                        checked = true;
                        if ((hit.mask[r] & (1ull << slot)) == 0) ok = false;
                    }
                }
                if (ok && !checked) {
                    float f = 0.f;
                    if (!mem::ReadFloat(rowBase + static_cast<uintptr_t>(h.off[r]), f) ||
                        !FloatMatches(f, e->value, e->tol)) {
                        ok = false;
                    }
                }
            }
            if (ok) {
                bool dup = false;
                for (const CrossRow& existing : h.crossRows) {
                    if (existing.rowAddr == rowBase && existing.modelKey == mb.key) dup = true;
                }
                if (!dup) {
                    CrossRow cr;
                    cr.modelKey = mb.key;
                    cr.rowAddr = rowBase;
                    h.crossRows.push_back(cr);
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Stage 3 helpers
// ---------------------------------------------------------------------------

constexpr uint32_t kEntityWindow = 0x400;

struct VecCand {
    uint32_t off = 0;
    Vec3 p0, p1, vel;
    float speed = 0.f;
    bool velSlot = false;
    uint32_t velOff = 0;
};

bool ReadEntityBlock(uintptr_t base, std::vector<uint8_t>& buf) {
    buf.resize(kEntityWindow);
    return mem::ReadBytes(base, buf.data(), buf.size());
}

Vec3 LoadVec3(const uint8_t* p) {
    Vec3 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void AnalyzeBlock(const uint8_t* b0, const uint8_t* b1, float dt, std::vector<VecCand>& out) {
    out.clear();
    if (!(dt > 0.f)) return;
    for (uint32_t o = 0; o + 12 <= kEntityWindow; o += 4) {
        const Vec3 p0 = LoadVec3(b0 + o);
        const Vec3 p1 = LoadVec3(b1 + o);
        if (!mem::IsFinite(p0) || !mem::IsFinite(p1)) continue;
        if (Length(p1) >= 200000.f) continue;
        const Vec3 d = p1 - p0;
        const float speed = Length(d) / dt;
        if (!(speed >= 50.f && speed <= 3000.f)) continue;  // not a missile-speed vector

        VecCand c;
        c.off = o;
        c.p0 = p0;
        c.p1 = p1;
        c.vel = d * (1.f / dt);
        c.speed = speed;

        // A real position slot is usually accompanied by the velocity vector it
        // is integrated with: look for a vec3 that matches d/dt within 25%.
        const float len = Length(c.vel);
        if (len >= 1e-3f) {
            for (uint32_t s = 0; s + 12 <= kEntityWindow; s += 4) {
                if (s < o + 12 && s + 12 > o) continue;  // skip the same vec3
                const Vec3 sv = LoadVec3(b0 + s);
                if (!mem::IsFinite(sv)) continue;
                const float sl = Length(sv);
                if (std::fabs(sl - len) > 0.25f * len) continue;
                Vec3 a, b;
                if (!Normalize(sv, a) || !Normalize(c.vel, b)) continue;
                if (Dot(a, b) < 0.968f) continue;  // ~15 degrees
                c.velSlot = true;
                c.velOff = s;
                break;
            }
        }
        out.push_back(c);
    }
}

// Local copy of the RIP-relative instruction matcher (mem.cpp keeps its own
// because mem.h cannot expose it without an interface change).  Keep in sync.
bool IsLegacyPrefix(uint8_t b) {
    switch (b) {
        case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3:
        case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65:
            return true;
        default:
            return false;
    }
}
bool IsRexPrefix(uint8_t b) { return (b & 0xF0u) == 0x40u; }

bool IsRipOpcode(uint8_t op) {
    switch (op) {
        case 0x01: case 0x03: case 0x09: case 0x0B: case 0x11: case 0x13:
        case 0x19: case 0x1B: case 0x21: case 0x23: case 0x29: case 0x2B:
        case 0x31: case 0x33: case 0x39: case 0x3B: case 0x63: case 0x85:
        case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
        case 0x8F: case 0xFF:
            return true;
        default:
            return false;
    }
}

bool IsSseOpcode(uint8_t op) {
    switch (op) {
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x28: case 0x29: case 0x2E: case 0x2F:
        case 0x51: case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x6E: case 0x6F: case 0x7E: case 0x7F: case 0xD6:
            return true;
        default:
            return false;
    }
}

bool DecodeRipInsn(const uint8_t* p, size_t avail, size_t& dispOff, size_t& insnLen) {
    dispOff = 0;
    insnLen = 0;
    size_t i = 0;
    for (int guard = 0; i < avail && i < 6 && IsLegacyPrefix(p[i]); ++guard, ++i) {
        if (guard > 5) return false;
    }
    if (i < avail && IsRexPrefix(p[i])) ++i;
    if (i >= avail) return false;
    if (p[i] == 0x0F) {
        if (i + 3 > avail || !IsSseOpcode(p[i + 1])) return false;
        const uint8_t modrm = p[i + 2];
        if ((modrm & 0xC7u) != 0x05u) return false;
        dispOff = i + 3;
        insnLen = dispOff + 4;
        return avail >= insnLen;
    }
    if (!IsRipOpcode(p[i])) return false;
    if (i + 2 > avail) return false;
    const uint8_t modrm = p[i + 1];
    if ((modrm & 0xC7u) != 0x05u) return false;
    dispOff = i + 2;
    insnLen = dispOff + 4;
    return avail >= insnLen;
}

}  // namespace

// ===========================================================================
// Stage 1: parameter tables
// ===========================================================================

bool LocateParameterTables(const DiscoveryOptions& opt, ParamTableResult& out,
                           std::vector<std::string>& diag) {
    out = ParamTableResult{};

    // ---- 1. usable signatures -------------------------------------------
    std::vector<ModelSig> sigs;
    SanitizeSignatures(opt, sigs, diag);
    if (sigs.empty()) {
        PushDiag(diag, "locate: no usable model signature");
        return false;
    }

    // ---- 2. regions to scan ---------------------------------------------
    const std::vector<mem::Region> regions = CollectScanRegions(opt);
    {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "locate: %zu models, %zu writable regions",
                      sigs.size(), regions.size());
        PushDiag(diag, buf);
    }
    if (regions.empty()) {
        PushDiag(diag, "locate: no scannable writable regions");
        return false;
    }

    // ---- 3. one pass per distinct value ---------------------------------
    std::vector<Group> groups;
    for (ModelSig& sig : sigs) {
        for (SigEntry& e : sig.entries) {
            uint32_t vb = 0, tb = 0;
            std::memcpy(&vb, &e.value, sizeof(vb));
            std::memcpy(&tb, &e.tol, sizeof(tb));
            int found = -1;
            for (size_t g = 0; g < groups.size(); ++g) {
                uint32_t gv = 0, gt = 0;
                std::memcpy(&gv, &groups[g].value, sizeof(gv));
                std::memcpy(&gt, &groups[g].tol, sizeof(gt));
                if (gv == vb && gt == tb) {
                    found = static_cast<int>(g);
                    break;
                }
            }
            if (found < 0) {
                Group g;
                g.value = e.value;
                g.tol = e.tol;
                groups.push_back(g);
                found = static_cast<int>(groups.size() - 1);
            }
            e.group = found;
        }
    }
    for (Group& g : groups) {
        for (const mem::Region& r : regions) {
            if (g.hits.size() >= opt.maxAnchorHits) break;
            mem::FindFloatRuns(r, &g.value, &g.tol, 1, g.hits, opt.maxAnchorHits);
        }
    }

    // ---- 4. window geometry ---------------------------------------------
    uint32_t before = opt.windowBefore & ~3u;
    uint32_t after = opt.windowAfter & ~3u;
    if (after < 4) after = 4;
    while ((before + after) / 4u > static_cast<uint32_t>(kMaxWindowSlots)) {
        if (after >= before + 4 && after > 4) {
            after -= 4;
        } else if (before >= 4) {
            before -= 4;
        } else {
            break;
        }
    }
    const int slots = SlotCount(before, after);
    const int anchorSlot = static_cast<int>(before / 4);

    // ---- 5. anchor windows per model ------------------------------------
    const std::vector<Range> selfRanges = BuildSelfExclusions(opt, sigs, groups);
    std::vector<ModelHits> modelHits(sigs.size());
    for (size_t mi = 0; mi < sigs.size(); ++mi) {
        const ModelSig& sig = sigs[mi];
        std::vector<uintptr_t> addrs;
        for (const SigEntry& e : sig.entries) {
            for (uintptr_t a : groups[e.group].hits) {
                if (!InRanges(selfRanges, a)) addrs.push_back(a);
            }
        }
        std::sort(addrs.begin(), addrs.end());
        addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
        if (addrs.size() > kHardAnchorCap) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s: anchor cap %zu/%zu (too many coincidental floats)",
                          sig.key.c_str(), kHardAnchorCap, addrs.size());
            PushDiag(diag, buf);
            addrs.resize(kHardAnchorCap);
        }
        for (uintptr_t a : addrs) {
            HitWin w;
            BuildHitWin(a, before, after, slots, sig, w);
            if (w.hi > w.lo) modelHits[mi].hits.push_back(w);
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: %zu anchors in window [-0x%X,+0x%X]",
                      sig.key.c_str(), modelHits[mi].hits.size(), before, after);
        PushDiag(diag, buf);
    }

    // ---- 6. hypotheses ---------------------------------------------------
    std::vector<Hyp> hyps;
    for (size_t mi = 0; mi < sigs.size(); ++mi) {
        GenerateHypotheses(sigs[mi], modelHits[mi], before, after, anchorSlot, slots, hyps);
    }
    std::stable_sort(hyps.begin(), hyps.end(), [](const Hyp& a, const Hyp& b) {
        const bool ka = a.off[static_cast<int>(Property::MaxHomingAngle)] >= 0 ||
                        a.off[static_cast<int>(Property::MaxRotationAngle)] >= 0;
        const bool kb = b.off[static_cast<int>(Property::MaxHomingAngle)] >= 0 ||
                        b.off[static_cast<int>(Property::MaxRotationAngle)] >= 0;
        if (ka != kb) return ka;
        if (a.score != b.score) return a.score > b.score;
        if (a.slots != b.slots) return a.slots > b.slots;
        return a.support > b.support;
    });
    if (hyps.size() > kMaxHypotheses) hyps.resize(kMaxHypotheses);

    std::vector<Hyp> kept;
    for (Hyp& h : hyps) {
        if (h.score < opt.minScore) continue;
        if (h.slots < 3) continue;  // 2-property layouts are routinely coincidences
        // Fields must not share a struct word.
        bool overlap = false;
        for (int a = 0; a < kPropCount && !overlap; ++a) {
            if (h.off[a] < 0) continue;
            for (int b = a + 1; b < kPropCount && !overlap; ++b) {
                if (h.off[b] < 0) continue;
                const int d = h.off[a] > h.off[b] ? h.off[a] - h.off[b] : h.off[b] - h.off[a];
                if (d < 4) overlap = true;
            }
        }
        if (!overlap) kept.push_back(h);
    }
    hyps.swap(kept);
    if (hyps.empty()) {
        PushDiag(diag, "locate: no layout hypothesis reached minScore");
        return false;
    }

    // ---- 7. cross-model validation --------------------------------------
    const bool crossModel = sigs.size() >= 2;
    size_t rejectedIncoherent = 0;
    for (Hyp& h : hyps) {
        if (!crossModel) continue;
        for (size_t mi = 0; mi < sigs.size(); ++mi) {
            if (sigs[mi].key == h.modelKey) continue;
            ValidateAgainstModel(h, sigs[mi], modelHits[mi], anchorSlot, slots);
        }
        if (h.crossRows.empty()) continue;

        // A cross-model value match alone is not enough: the rows we would report
        // must form ONE coherent table - same address cluster, a common stride
        // large enough to hold a row, and the hypothesis' own rows and the
        // cross-model rows at the same relative slots.  Anything that does not fit
        // (a value copy elsewhere in the heap, the test's decoy block, ...) is
        // dropped instead of contaminating the table.
        uint32_t span = 4;
        for (int p = 0; p < kPropCount; ++p) {
            if (h.off[p] >= 0 && static_cast<uint32_t>(h.off[p]) + 4 > span) {
                span = static_cast<uint32_t>(h.off[p]) + 4;
            }
        }
        std::vector<uintptr_t> addrs;
        std::vector<uint32_t> spans;
        for (uintptr_t b : h.rows) {
            addrs.push_back(b);
            spans.push_back(span);
        }
        for (const CrossRow& cr : h.crossRows) {
            addrs.push_back(cr.rowAddr);
            spans.push_back(span);
        }
        uint32_t stride = 0;
        const std::vector<size_t> keptIdx = CoherentChain(addrs, spans, stride);
        std::vector<uintptr_t> keptAddrs;
        for (size_t idx : keptIdx) keptAddrs.push_back(addrs[idx]);

        std::vector<uintptr_t> ownRows;
        for (uintptr_t b : h.rows) {
            if (std::find(keptAddrs.begin(), keptAddrs.end(), b) != keptAddrs.end()) ownRows.push_back(b);
        }
        std::vector<CrossRow> crossRows;
        std::vector<std::string> validatedBy;
        for (const CrossRow& cr : h.crossRows) {
            if (std::find(keptAddrs.begin(), keptAddrs.end(), cr.rowAddr) == keptAddrs.end()) continue;
            crossRows.push_back(cr);
            if (std::find(validatedBy.begin(), validatedBy.end(), cr.modelKey) == validatedBy.end()) {
                validatedBy.push_back(cr.modelKey);
            }
        }
        if (!ownRows.empty() && !crossRows.empty() && keptAddrs.size() >= 2) {
            h.rows = ownRows;
            h.crossRows = crossRows;
            h.validatedBy = validatedBy;
            h.verified = true;
        } else {
            h.verified = false;
            h.validatedBy.clear();
            h.crossRows.clear();
            ++rejectedIncoherent;
        }
    }
    if (rejectedIncoherent > 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "locate: %zu cross-model hypotheses rejected (rows not a coherent table)",
                      rejectedIncoherent);
        PushDiag(diag, buf);
    }

    // ---- 8. rows ---------------------------------------------------------
    std::vector<ParamRow> rows;
    auto addRow = [&](uintptr_t base, const std::string& modelKey, const int off[kPropCount],
                      int score, bool verified, const ModelSig* valueSource) {
        if (rows.size() >= opt.maxRows) return;
        for (const ParamRow& existing : rows) {
            if (existing.rowAddr == base && existing.modelKey == modelKey) return;
        }
        ParamRow r;
        r.rowAddr = base;
        r.modelKey = modelKey;
        r.score = score;
        r.verified = verified;
        for (int p = 0; p < kPropCount; ++p) {
            if (off[p] < 0) continue;
            PropertySlot s;
            s.prop = static_cast<Property>(p);
            s.offset = static_cast<uint32_t>(off[p]);
            float f = 0.f;
            if (mem::ReadFloat(base + static_cast<uintptr_t>(off[p]), f)) {
                s.foundValue = f;
            } else if (valueSource) {
                const SigEntry* e = FindEntry(*valueSource, p);
                if (e) s.foundValue = e->value;
            }
            r.slots.push_back(s);
        }
        rows.push_back(r);
    };

    const ModelSig* firstModel = &sigs.front();
    // One layout per model: the verified hypothesis that explains the most rows x
    // properties wins.  A phase-shifted sub-layout (e.g. the same struct anchored
    // one field later, missing MaxRotationAngle) scores lower and is discarded, as
    // is any hypothesis that lost the cross-model/coherence checks.
    for (const ModelSig& sig : sigs) {
        const Hyp* best = nullptr;
        for (const Hyp& h : hyps) {
            if (!h.verified || h.modelKey != sig.key) continue;
            if (!best || h.score > best->score ||
                (h.score == best->score && h.slots > best->slots) ||
                (h.score == best->score && h.slots == best->slots && h.support > best->support)) {
                best = &h;
            }
        }
        if (!best) continue;
        for (uintptr_t b : best->rows) addRow(b, best->modelKey, best->off, best->score, true, &sig);
        for (const CrossRow& cr : best->crossRows) {
            const ModelSig* src = nullptr;
            for (const ModelSig& s : sigs) {
                if (s.key == cr.modelKey) src = &s;
            }
            addRow(cr.rowAddr, cr.modelKey, best->off, best->score, true, src);
        }
    }

    const bool anyVerified = std::any_of(rows.begin(), rows.end(),
                                         [](const ParamRow& r) { return r.verified; });
    if (!anyVerified) {
        // Nothing survived cross-model validation: report the best candidates so
        // the user can hand the log back, but they stay unverified (and therefore
        // must not be written to by the patcher).
        for (const Hyp& h : hyps) {
            const ModelSig* own = nullptr;
            for (const ModelSig& s : sigs) {
                if (s.key == h.modelKey) own = &s;
            }
            if (!own) own = firstModel;
            for (uintptr_t b : h.rows) addRow(b, h.modelKey, h.off, h.score, false, own);
        }
    }

    // Keep verified rows only when we have them; otherwise the (unverified)
    // candidates.
    std::vector<ParamRow> finalRows;
    for (const ParamRow& r : rows) {
        if (anyVerified && !r.verified) continue;
        finalRows.push_back(r);
    }
    std::stable_sort(finalRows.begin(), finalRows.end(),
                     [](const ParamRow& a, const ParamRow& b) {
                         if (a.verified != b.verified) return a.verified;
                         return a.rowAddr < b.rowAddr;
                     });
    if (finalRows.size() > opt.maxRows) finalRows.resize(opt.maxRows);

    // ---- 8b. table coherence --------------------------------------------
    uint32_t tableStride = 0;
    bool coherenceOk = true;
    if (anyVerified) {
        std::vector<ParamRow> coherent = SelectCoherentTable(finalRows, tableStride);
        if (coherent.size() >= 2) {
            finalRows.swap(coherent);
        } else {
            // A single cross-model hit, or rows that cannot be laid out as a table,
            // is not safe enough to be written to.
            coherenceOk = false;
            for (ParamRow& r : finalRows) r.verified = false;
            PushDiag(diag,
                     "locate: verified rows failed the table-coherence check "
                     "(rows overlap / no common stride); reporting as unverified");
        }
    }
    {
        std::vector<ParamRow> uniqueRows;
        for (const ParamRow& r : finalRows) {
            bool dup = false;
            for (const ParamRow& existing : uniqueRows) {
                if (existing.rowAddr == r.rowAddr && existing.modelKey == r.modelKey) dup = true;
            }
            if (!dup) uniqueRows.push_back(r);
        }
        finalRows.swap(uniqueRows);
    }
    out.rows = finalRows;

    // ---- 9. table base / stride -----------------------------------------
    if (!out.rows.empty()) {
        out.tableBase = out.rows.front().rowAddr;  // rows are sorted by address
        if (coherenceOk && out.rows.size() >= 3 && tableStride >= 4) out.stride = tableStride;
    }

    // ---- 10. hypotheses reported to the caller --------------------------
    for (const Hyp& h : hyps) {
        if (h.verified) {
            LayoutHypothesis lh;
            lh.modelKey = h.modelKey;
            for (int p = 0; p < kPropCount; ++p) {
                if (h.off[p] < 0) continue;
                PropertySlot s;
                s.prop = static_cast<Property>(p);
                s.offset = static_cast<uint32_t>(h.off[p]);
                lh.slots.push_back(s);
            }
            lh.supportedRows = h.support;
            lh.modelsMatched = 1 + static_cast<int>(h.validatedBy.size());
            lh.score = h.score;
            lh.verified = true;
            out.hypotheses.push_back(lh);
        }
    }

    // ---- 11. diagnostics -------------------------------------------------
    int shown = 0;
    for (const Hyp& h : hyps) {
        if (shown >= 5) break;
        char buf[256];
        std::string by;
        for (const std::string& k : h.validatedBy) {
            if (!by.empty()) by += ",";
            by += k;
        }
        std::snprintf(buf, sizeof(buf), "%s: hyp %s support %d score %d verified=%s%s%s",
                      h.modelKey.c_str(), DescribeHyp(h).c_str(), h.support, h.score,
                      h.verified ? "yes" : "no", by.empty() ? "" : " (", by.c_str());
        std::string line(buf);
        if (!by.empty()) line += ")";
        PushDiag(diag, line);
        ++shown;
    }
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "locate: %zu rows (%zu verified), base=0x%llX stride=0x%X, %zu hypotheses",
                      out.rows.size(),
                      static_cast<size_t>(std::count_if(out.rows.begin(), out.rows.end(),
                                                        [](const ParamRow& r) { return r.verified; })),
                      static_cast<unsigned long long>(out.tableBase), out.stride,
                      out.hypotheses.size());
        PushDiag(diag, buf);
    }
    return !out.rows.empty();
}

// ===========================================================================
// Stage 2: entities referencing a verified row
// ===========================================================================

bool FindEntities(const std::vector<ParamRow>& rows, std::vector<EntityCandidate>& out,
                  size_t maxEntities, std::vector<std::string>& diag) {
    out.clear();
    if (rows.empty() || maxEntities == 0) {
        PushDiag(diag, "entities: no parameter rows to search for");
        return false;
    }

    const bool anyVerified = std::any_of(rows.begin(), rows.end(),
                                         [](const ParamRow& r) { return r.verified; });

    struct Target {
        uintptr_t lo = 0;
        uintptr_t hi = 0;
        uintptr_t row = 0;
        std::string key;
    };
    std::vector<Target> targets;
    for (const ParamRow& r : rows) {
        if (anyVerified && !r.verified) continue;
        uint32_t span = 8;
        for (const PropertySlot& s : r.slots) {
            const uint32_t end = s.offset + 4;
            if (end > span) span = end;
        }
        if (span < 8) span = 8;
        Target t;
        t.lo = r.rowAddr;
        t.hi = r.rowAddr + span;
        t.row = r.rowAddr;
        t.key = r.modelKey;
        targets.push_back(t);
    }
    if (targets.empty()) {
        PushDiag(diag, "entities: no verified rows to search for");
        return false;
    }

    // Entities live in the game's private heap.  Only MEM_PRIVATE writable
    // regions are scanned (the header contract); MEM_IMAGE .data is stage 1's
    // job and is not where per-missile objects live.
    std::vector<mem::Region> regions;
    mem::EnumRegions(regions);

    // Our own bookkeeping memory contains row addresses and must not be reported
    // as a game entity.  Skip exact ranges (not whole regions: the game's heap
    // may share a region with the CRT heap).
    std::vector<Range> selfRanges;
    {
        Range r;
        r.lo = reinterpret_cast<uintptr_t>(rows.data());
        r.hi = r.lo + rows.size() * sizeof(ParamRow);
        selfRanges.push_back(r);
        r.lo = reinterpret_cast<uintptr_t>(targets.data());
        r.hi = r.lo + targets.size() * sizeof(Target);
        selfRanges.push_back(r);
        char stackProbe = 0;
        (void)stackProbe;
    }
    char stackProbe = 0;
    const uintptr_t stackMarker = reinterpret_cast<uintptr_t>(&stackProbe);

    out.reserve(maxEntities);
    {
        Range r;
        r.lo = reinterpret_cast<uintptr_t>(out.data());
        r.hi = r.lo + out.capacity() * sizeof(EntityCandidate);
        selfRanges.push_back(r);
    }

    std::vector<uint8_t> buf(64 * 1024);
    const size_t kStep = buf.size() - 8;  // 8-byte overlap, 8-aligned step

    for (const mem::Region& reg : regions) {
        if (!reg.readable || !reg.writable) continue;
        if (reg.type != kMemPrivate) continue;
        if (stackMarker >= reg.base && stackMarker < reg.base + reg.size) continue;

        for (size_t off = 0; off < reg.size;) {
            const size_t len = std::min(buf.size(), reg.size - off);
            const bool last = (off + len >= reg.size);
            if (!mem::ReadBytes(reg.base + off, buf.data(), len)) {
                if (last) break;
                off += len;
                off = (off + 7u) & ~static_cast<size_t>(7u);
                continue;
            }
            const size_t limit = last ? len : (len >= 8 ? len - 8 : 0);
            for (size_t p = 0; p + 8 <= limit; p += 8) {
                if (out.size() >= maxEntities) break;
                uint64_t v = 0;
                std::memcpy(&v, buf.data() + p, 8);
                if (!v) continue;
                const uintptr_t field = reg.base + off + p;
                if (InRanges(selfRanges, field)) continue;
                for (const Target& t : targets) {
                    if (static_cast<uintptr_t>(v) >= t.lo && static_cast<uintptr_t>(v) < t.hi) {
                        EntityCandidate c;
                        c.addr = field & ~static_cast<uintptr_t>(0xF);  // convention: 16-byte base
                        c.rowPtrAddr = field;
                        c.rowAddr = t.row;
                        c.modelKey = t.key;
                        out.push_back(c);
                        break;
                    }
                }
            }
            if (last) break;
            off += kStep;
        }
        if (out.size() >= maxEntities) break;
    }

    {
        char buf2[200];
        std::snprintf(buf2, sizeof(buf2), "entities: %zu pointer candidates for %zu rows (%s)",
                      out.size(), targets.size(), anyVerified ? "verified only" : "unverified");
        PushDiag(diag, buf2);
        size_t shown = 0;
        for (const EntityCandidate& c : out) {
            if (shown++ >= 12) {
                PushDiag(diag, "entities: ... (further candidates omitted)");
                break;
            }
            char line[160];
            std::snprintf(line, sizeof(line), "entities: %s row=0x%llX ptr@0x%llX base~0x%llX",
                          c.modelKey.c_str(), static_cast<unsigned long long>(c.rowAddr),
                          static_cast<unsigned long long>(c.rowPtrAddr),
                          static_cast<unsigned long long>(c.addr));
            PushDiag(diag, line);
        }
    }
    return !out.empty();
}

// ===========================================================================
// Stage 3: motion correlation
// ===========================================================================

bool CorrelateMotion(uintptr_t entity, float dt, EntityLayout& out) {
    out = EntityLayout{};
    if (!entity || !(dt > 0.f)) {
        out.diag = "bad entity/dt";
        return false;
    }

    std::vector<uint8_t> e0, e1;
    if (!ReadEntityBlock(entity, e0)) {
        out.diag = "entity not readable";
        return false;
    }

    // Follow up to four pointers that look like a reference to another object
    // (the target entity).  Read both samples inside the same time window.
    std::vector<uintptr_t> ptrs;
    for (uint32_t p = 0; p + 8 <= kEntityWindow && ptrs.size() < 4; p += 8) {
        uint64_t v = 0;
        std::memcpy(&v, e0.data() + p, 8);
        if (!v) continue;
        const uintptr_t q = static_cast<uintptr_t>(v);
        if (q >= entity && q < entity + kEntityWindow) continue;  // self reference
        if (!mem::IsReadable(q, 0x40)) continue;
        bool dup = false;
        for (uintptr_t existing : ptrs) {
            if (existing == q) dup = true;
        }
        if (!dup) ptrs.push_back(q);
    }
    std::vector<std::vector<uint8_t>> t0(ptrs.size()), t1(ptrs.size());
    for (size_t i = 0; i < ptrs.size(); ++i) ReadEntityBlock(ptrs[i], t0[i]);

    // Wait for `dt`, but use the measured wall-clock interval for the velocity
    // estimate: Sleep() granularity must not bias the result by >25%.
    LARGE_INTEGER freq{};
    LARGE_INTEGER tStart{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);
    double elapsed = 0.0;
    for (;;) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        elapsed = static_cast<double>(now.QuadPart - tStart.QuadPart) /
                  static_cast<double>(freq.QuadPart ? freq.QuadPart : 1);
        if (elapsed >= static_cast<double>(dt)) break;
        Sleep(1);
    }

    if (!ReadEntityBlock(entity, e1)) {
        out.diag = "entity not readable on second sample";
        return false;
    }
    for (size_t i = 0; i < ptrs.size(); ++i) ReadEntityBlock(ptrs[i], t1[i]);

    float measured = static_cast<float>(elapsed);
    if (!(measured > 0.f) || measured > 10.f * dt) measured = dt;

    std::vector<VecCand> cands;
    AnalyzeBlock(e0.data(), e1.data(), measured, cands);
    if (cands.empty()) {
        out.diag = "no moving vec3 (50..3000 units/s) inside entity";
        return false;
    }

    // A 12-byte window that starts one float early ("[pad, x, y]") moves just as
    // consistently as the real vector, and its shifted neighbour can even look
    // like a matching velocity slot.  Disambiguate with structure, in order:
    //   - a real position slot is accompanied by a velocity slot at all,
    //   - real vectors sit on 16-byte-aligned offsets (struct/array boundary),
    //   - the real position has the larger coordinate magnitude,
    //   - and only then the higher speed.
    auto preferred = [](const VecCand& a, const VecCand& b) -> bool {
        if (a.velSlot != b.velSlot) return a.velSlot;
        const bool a16 = (a.off & 0xFu) == 0;
        const bool b16 = (b.off & 0xFu) == 0;
        if (a16 != b16) return a16;
        const float la = Length(a.p0);
        const float lb = Length(b.p0);
        if (std::fabs(la - lb) > 0.05f * (la + lb + 1e-3f)) return la > lb;
        return a.speed > b.speed;
    };

    // ---- missile position: fast candidate that has a matching velocity slot ---
    float bestSpeed = 0.f;
    for (const VecCand& c : cands) {
        if (c.velSlot && c.speed > bestSpeed) bestSpeed = c.speed;
    }
    int posIdx = -1;
    bool velKnown = false;
    if (bestSpeed > 0.f) {
        // Near-ties are the shifted-window case; judge them with `preferred`.
        const float floorSpeed = bestSpeed * 0.90f;
        for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
            if (!cands[i].velSlot || cands[i].speed < floorSpeed) continue;
            if (posIdx < 0 || preferred(cands[i], cands[posIdx])) posIdx = i;
        }
        velKnown = true;
    }
    if (posIdx < 0) {  // no velocity slot anywhere: best speed alone
        for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
            if (posIdx < 0 || cands[i].speed > cands[posIdx].speed) posIdx = i;
        }
    }
    const VecCand& mp = cands[posIdx];
    out.posOff = mp.off;
    out.velOff = velKnown ? mp.velOff : 0;
    out.speed = mp.speed;

    // ---- target: slower vector, ideally accompanied by its own velocity slot
    int tgtIdx = -1;
    for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
        if (i == posIdx) continue;
        const VecCand& c = cands[i];
        if (c.speed > 1200.f) continue;                        // too fast for a target
        if (c.speed > mp.speed * 0.95f) continue;              // target must be slower
        const int32_t delta = static_cast<int32_t>(c.off) - static_cast<int32_t>(mp.off);
        if (delta > -12 && delta < 12) continue;               // overlaps the position vector
        if (tgtIdx < 0 || preferred(cands[i], cands[tgtIdx])) tgtIdx = i;
    }

    bool hasTarget = (tgtIdx >= 0);
    bool targetEntityFound = false;
    uintptr_t targetEntity = 0;
    if (hasTarget) {
        const VecCand& tc = cands[tgtIdx];
        out.targetPosOff = tc.off;
        out.targetVelOff = tc.velSlot ? tc.velOff : 0;
        out.targetSpeed = tc.speed;
    } else {
        // Fallback: follow a pointer to the target entity and look for its motion
        // there.  We cannot store its address in EntityLayout (no field for it),
        // so this only confirms that a target exists; Snapshot() needs an
        // in-entity target vector and will report invalid in this case.
        for (size_t i = 0; i < ptrs.size() && !targetEntityFound; ++i) {
            if (t0[i].size() != kEntityWindow || t1[i].size() != kEntityWindow) continue;
            std::vector<VecCand> tcands;
            AnalyzeBlock(t0[i].data(), t1[i].data(), measured, tcands);
            for (const VecCand& c : tcands) {
                if (c.speed <= 1200.f && c.velSlot) {
                    targetEntityFound = true;
                    targetEntity = ptrs[i];
                    out.targetSpeed = c.speed;
                    break;
                }
            }
        }
        hasTarget = targetEntityFound;
    }

    out.hasTarget = hasTarget;
    out.verified = hasTarget && out.posOff != 0 && out.targetPosOff != 0 &&
                   out.velOff != 0 && out.targetVelOff != 0;

    char line[200];
    std::snprintf(line, sizeof(line),
                  "pos+0x%X(vel+0x%X,%s) speed=%.0f target+0x%X(vel+0x%X) tSpeed=%.0f dt=%.3f%s%s",
                  out.posOff, out.velOff, velKnown ? "found" : "synthesized", out.speed,
                  out.targetPosOff, out.targetVelOff, out.targetSpeed, measured,
                  out.verified ? " verified" : "",
                  (!hasTarget ? " no target" : (targetEntity ? " targetEntity" : "")));
    out.diag = line;
    if (targetEntity) {
        char extra[96];
        std::snprintf(extra, sizeof(extra), " targetEntity=0x%llX",
                      static_cast<unsigned long long>(targetEntity));
        out.diag += extra;
    }
    return out.verified;
}

bool Snapshot(const EntityCandidate& e, const EntityLayout& layout, EntitySnapshot& out) {
    out = EntitySnapshot{};
    out.entity = e.addr;
    if (!e.addr || !layout.verified) return false;
    // Conventions: pos may live at offset 0; targetPos/targetVel offset 0 means
    // "unknown" (an entity never starts with its target vector).
    if (layout.targetPosOff == 0) return false;

    Vec3 pos, vel, targetPos, targetVel;
    if (!mem::ReadVec3(e.addr + layout.posOff, pos)) return false;
    if (!mem::ReadVec3(e.addr + layout.targetPosOff, targetPos)) return false;
    if (layout.velOff != 0) mem::ReadVec3(e.addr + layout.velOff, vel);
    if (layout.targetVelOff != 0) mem::ReadVec3(e.addr + layout.targetVelOff, targetVel);

    if (!mem::IsFinite(pos) || !mem::IsFinite(targetPos) || !mem::IsFinite(vel) ||
        !mem::IsFinite(targetVel)) {
        return false;
    }
    if (Length(pos) >= 200000.f || Length(targetPos) >= 200000.f) return false;
    if (Length(vel) > 5000.f || Length(targetVel) > 2000.f) return false;

    out.pos = pos;
    out.vel = vel;
    out.targetPos = targetPos;
    out.targetVel = targetVel;
    out.valid = true;
    return true;
}

// ===========================================================================
// Stage 3c: code references to the discovered table
// ===========================================================================

void FindCodeReferences(uintptr_t lo, uintptr_t hi, std::vector<CodeRefSite>& out,
                        size_t maxHits) {
    out.clear();
    if (hi <= lo || maxHits == 0) return;

    mem::ModuleInfo mm;
    if (!mem::MainModule(mm) || mm.size == 0) return;

    std::vector<mem::Region> regions;
    mem::EnumRegions(regions);
    for (const mem::Region& r : regions) {
        if (out.size() >= maxHits) return;
        if (!r.readable || !r.executable) continue;
        // Only the main module's code is interesting (that is the game's own
        // instruction stream; other modules cannot legally reference its data).
        if (r.base + r.size <= mm.base || r.base >= mm.base + mm.size) continue;

        std::vector<uintptr_t> insns;
        mem::CollectRipRelativeRefs(r, lo, hi, insns, maxHits - out.size());
        for (uintptr_t insn : insns) {
            if (out.size() >= maxHits) return;
            uint8_t raw[16] = {};
            if (!mem::ReadBytes(insn, raw, sizeof(raw))) continue;
            size_t dispOff = 0, insnLen = 0;
            if (!DecodeRipInsn(raw, sizeof(raw), dispOff, insnLen)) continue;
            int32_t disp = 0;
            std::memcpy(&disp, raw + dispOff, sizeof(disp));
            const int64_t t = static_cast<int64_t>(insn) + static_cast<int64_t>(insnLen) +
                              static_cast<int64_t>(disp);
            if (t <= 0) continue;
            const uintptr_t target = static_cast<uintptr_t>(t);
            if (target < lo || target >= hi) continue;
            CodeRefSite site;
            site.insnAddr = insn;
            site.target = target;
            out.push_back(site);
        }
    }
}

}  // namespace acz::game
