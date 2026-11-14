/* SPDX-License-Identifier: MIT */
#ifndef GCN_DEBUG_COST_RING_H
#define GCN_DEBUG_COST_RING_H

#include "common/types.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_COST_RING_CAPACITY 2048u
#define GCN_COST_THREAD_CAPACITY 16u
#ifndef GCN_COST_RING_ENABLED
#define GCN_COST_RING_ENABLED 1
#endif
#ifndef GCN_COST_THREAD_FAST
#define GCN_COST_THREAD_FAST 0
#endif

typedef enum {
    GCN_COST_HOST,
    GCN_COST_DISPATCH,
    GCN_COST_GUEST,
    GCN_COST_INVALIDATE,
    GCN_COST_FIFO,
    GCN_COST_TIME,
    GCN_COST_VIDEO,
    GCN_COST_DSP,
    GCN_COST_AI,
    GCN_COST_DI,
    GCN_COST_IOS,
    GCN_COST_IPC,
    GCN_COST_GRAPHICS,
    GCN_COST_BUCKET_COUNT
} GcnCostBucket;

typedef struct {
    u32 thread;
    u64 cycles;
} GcnCostThread;

/* Measured coarse partitions cover the complete simulation-thread interval.
 * Execution includes demand-driven device observations inside native code.
 * The finer sampled buckets below are a separate, non-additive breakdown. */
typedef enum {
    GCN_COST_OWNER_HOST,GCN_COST_OWNER_EXECUTION,GCN_COST_OWNER_DEVICES,
    GCN_COST_OWNER_GRAPHICS,GCN_COST_OWNER_COUNT
} GcnCostOwner;

typedef struct {
    u64 field,guest_cycles,wall_ns,turns,sampled_turns;
    u64 sampled_ns[GCN_COST_BUCKET_COUNT];
    u64 measured_ns[GCN_COST_OWNER_COUNT];
    GcnCostThread threads[GCN_COST_THREAD_CAPACITY];
    u64 no_thread_cycles,other_thread_cycles,idle_skipped_cycles;
    u32 thread_count;
} GcnCostField;

/* Frequent scopes are sampled once per 257 turns. Recorded nanoseconds are
 * raw exclusive sample totals, with both denominators retained per field.
 * Readers must report the sampling estimate and its residual against wall
 * time. No rescaling to force agreement with wall time occurs here. */
extern bool gcn_cost_sampling;
unsigned gcn_cost_boundary(unsigned bucket);

static inline unsigned gcn_cost_enter(GcnCostBucket bucket) {
#if GCN_COST_RING_ENABLED
    return gcn_cost_sampling ? gcn_cost_boundary((unsigned)bucket) : GCN_COST_HOST;
#else
    (void)bucket;
    return GCN_COST_HOST;
#endif
}

static inline void gcn_cost_leave(unsigned previous) {
#if GCN_COST_RING_ENABLED
    if(gcn_cost_sampling)gcn_cost_boundary(previous);
#else
    (void)previous;
#endif
}

/* A compiled-OFF provider keeps every exported operation clock-free and
 * inert even before init; JSON explicitly reports disabled with no fields. */
void gcn_cost_init(u64 guest_cycles);
void gcn_cost_begin_turn(u64 guest_cycles,u32 current_thread);
void gcn_cost_end_turn(u64 guest_cycles);
void gcn_cost_owner_boundary(GcnCostOwner);
/* Account thread changes inside an event-owner turn without adding clocks. */
void gcn_cost_thread_boundary(u64 guest_cycles,u32 current_thread);
/* Optional inline observation of the canonical turn origin/thread. Only the
 * last unchanged-thread timestamp is pending; all cycle totals stay in the
 * existing ledger. A NULL provider retains the raw API (including OFF). */
typedef struct {
    u64 cycles,pending_cycles;
    u32 thread;
    bool pending;
} GcnCostThreadState;
extern GcnCostThreadState* const gcn_cost_thread_state;
#if GCN_COST_THREAD_FAST
static inline void gcn_cost_thread_boundary_fast(u64 cycles,u32 thread) {
#if GCN_COST_RING_ENABLED
    GcnCostThreadState* state=gcn_cost_thread_state;
    if(state && thread==state->thread &&
       cycles>=(state->pending?state->pending_cycles:state->cycles)) {
        state->pending_cycles=cycles;state->pending=true;
        return;
    }
#endif
    gcn_cost_thread_boundary(cycles,thread);
}
#else
#define gcn_cost_thread_boundary_fast gcn_cost_thread_boundary
#endif
void gcn_cost_note_idle(u64 skipped_cycles);
void gcn_cost_finish_field(u64 field,u64 guest_cycles);
void gcn_cost_write_json(FILE*,u32 limit);

#ifdef __cplusplus
}
#endif
#endif
