/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_CPU_OVERLAY_REGION_H
#define GCN_CPU_OVERLAY_REGION_H
#include "cpu/overlay_module.h"

/* Optional larger emitted body over one observed tuple of adjacent pages.
 * Each member names the existing content-keyed body in the SAME PC namespace.
 * The ordinary page catalog remains the compatibility path. The owner must
 * exclude pages with dispatch hooks, and forward its existing invalidation and
 * reset notifications here. Like overlay residency, this state belongs to the
 * CPU execution thread; publication does not permit concurrent mutation. */
typedef struct {
    u32 start, page_count;
    const GcnOverlayFn* members;
    GcnOverlayFn body;
    const GcnOverlayModule* module;
    const CPUState* cpu;
    /* Optional read-only owner boundary: pending interrupts, stop PCs, etc. */
    bool (*boundary)(const CPUState*,u32 target,u64 cycles,void* user);
    void* boundary_user;
    u64 generation;
    bool enabled, known, valid;
    u64 calls, crossings, declined_crossings;
} GcnOverlayRegion;

typedef struct {
    GcnOverlayRegion* region;
    const CPUState* cpu;
    u64 generation;
} GcnOverlayRegionInvocation;

/* Enabling is explicit and quiescent. A change retires prior invocations. */
void gcn_overlay_region_enable(GcnOverlayRegion*, bool enabled);
void gcn_overlay_region_invalidate(GcnOverlayRegion*, u32 address, u32 size);
bool gcn_overlay_region_call(GcnOverlayModule*, CPUState*,
                             GcnOverlayRegion*, u32 address);
GcnOverlayRegionInvocation gcn_overlay_region_invocation(void);

/* Emit this guard on EVERY cross-page entry, including local calls, shadow
 * returns and sequential flow. Entry-only attestation is insufficient: a store
 * or callback can invalidate another member while the current body runs.
 * On decline, generated code must publish the exact target PC and accumulated
 * cycle prefix and return to ordinary native dispatch without executing it. */
static inline bool gcn_overlay_region_cross(
    const GcnOverlayRegionInvocation* invocation, const CPUState* cpu,
    u32 target, u64 cycles, u64 deadline) {
    GcnOverlayRegion* region=invocation->region;
    if (!region) return false;
    const bool inside=target>=region->start &&
        (u64)target<(u64)region->start+(u64)region->page_count*4096u;
    bool allowed=invocation->cpu==cpu && region->cpu==cpu &&
        region->enabled && region->valid &&
        invocation->generation==region->generation && inside && !(target&3u) &&
        !cpu->exception && !cpu->halted && cycles<deadline;
    if (allowed && region->boundary)
        allowed=region->boundary(cpu,target,cycles,region->boundary_user);
    if (allowed) ++region->crossings;
    else ++region->declined_crossings;
    return allowed;
}
#endif
