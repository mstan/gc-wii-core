/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_RESIDENT_MEMORY_H
#define GCNLLR_RESIDENT_MEMORY_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define GCN_RESIDENT_MAX_RECORDS 4096u
#define GCN_RESIDENT_MAX_ROWS 65536u
#define GCN_RESIDENT_MAX_PAGES 1048576u

typedef struct GcnResidentMemory GcnResidentMemory;
typedef struct {
    uint32_t offset,row_bytes,rows,stride;
} GcnResidentRows;
typedef struct {
    uint64_t token,generation;
    GcnResidentRows destination;
} GcnResidentCopy;
typedef bool (*GcnResidentMaterialize)(void* user,const GcnResidentCopy* copy);
typedef void (*GcnResidentRelease)(void* user,const GcnResidentCopy* copy);
typedef struct {
    uint32_t memory_size,page_size,capacity;
    /* Dedicated caller-owned bytes, one per page. Only this manager writes
     * them. Nonzero means an owned active row touches the page; padding on
     * that page need not be owned. Flags/storage outlive successful destroy. */
    uint8_t* page_flags;
    uint32_t page_count;
    GcnResidentMaterialize materialize;
    GcnResidentRelease release;
    void* user;
} GcnResidentMemoryConfig;
typedef enum {
    GCN_RESIDENT_OK,
    GCN_RESIDENT_INVALID,
    GCN_RESIDENT_BUSY,
    GCN_RESIDENT_MATERIALIZE_FAILED,
    GCN_RESIDENT_GENERATION_EXHAUSTED
} GcnResidentError;
typedef struct {
    uint64_t publications,materializations,discards,evictions;
    uint64_t callback_failures,reentrant_rejections;
    uint32_t owned;
} GcnResidentMemoryStats;

/* One owner thread, except for the explicitly concurrent pending query below.
 * Offsets are normalized MEM1
 * offsets, never guest virtual addresses. page_size must be a power of two.
 * An empty manager is the eager/no-owned-record floor: valid accesses succeed
 * without calling either callback. Invalid/overflowing ranges are rejected. */
GcnResidentMemory* gcn_resident_memory_create(const GcnResidentMemoryConfig*);

/* token is nonzero and identifies an immutable retained copy. Ownership
 * transfers only on success; on failure the caller still owns the new token.
 * A token cannot appear twice among live records. Generations never repeat
 * within a manager. Callbacks may already have materialized older records
 * before a later failure; unresolved records always remain owned and flagged.
 * Partial overlaps materialize the old record. Fully covered predecessors
 * are discarded only when the new publication can commit successfully. */
bool gcn_resident_memory_publish(GcnResidentMemory*,uint64_t token,
                                 const GcnResidentRows*,uint64_t* generation);
/* Build-selected display ownership: retain untouched pieces of dense copies
 * instead of downloading a partially replaced image. The materializer MUST
 * use next_span below; original destination/source geometry remains intact.
 * Without GCNLLR_RESIDENT_SLICES this is the ordinary whole-copy floor. */
bool gcn_resident_memory_publish_slices(GcnResidentMemory*,uint64_t token,
                                      const GcnResidentRows*,uint64_t* generation);
/* First still-owned interval inside [offset,offset+size) for this token.
 * Owner-thread query, including from a materialization callback. Never write
 * outside these intervals: newer GPU results and CPU bytes may occupy holes.
 * Returning false means no live interval, not an error for a retired token. */
bool gcn_resident_memory_next_span(const GcnResidentMemory*,uint64_t token,
                                  uint32_t offset,uint32_t size,
                                  uint32_t* first,uint32_t* length);
bool gcn_resident_memory_prepare_read(GcnResidentMemory*,uint32_t offset,uint32_t size);
/* complete_overwrite promises every byte in the range WILL be overwritten
 * immediately after success, before any other access/publication. It permits
 * discarding a wholly covered record. Do not abandon that promised write.
 * False preserves copied bytes even if the caller never performs a store.
 * Partial stores materialize the whole overlapping record, preserving its
 * other active bytes. Padding-only accesses do not materialize that record. */
bool gcn_resident_memory_prepare_write(GcnResidentMemory*,uint32_t offset,uint32_t size,
                                     bool complete_overwrite);
bool gcn_resident_memory_flush(GcnResidentMemory*);
/* Complete exactly this live token. A previously retired token is a no-op;
 * never materialize a newer producer merely because its address overlaps. */
bool gcn_resident_memory_complete(GcnResidentMemory*,uint64_t token);
/* Failure leaves the manager, unresolved tokens and flags alive for retry.
 * Never expose stale RAM merely because flush/destroy failed. */
bool gcn_resident_memory_destroy(GcnResidentMemory*);

/* Concurrent-safe conservative query of a manager-owned C11 atomic page map.
 * True includes page-sharing padding and invalid arguments: ask the owner to
 * prepare/validate the actual access. False means no pending page was seen
 * during the query, not a lease against future publications. Callers still
 * need their normal producer/consumer ordering around the memory access.
 * The manager must outlive all queries; quiesce readers before destroy.
 * This is the only function, including queries, usable off the owner thread. */
bool gcn_resident_memory_pending(const GcnResidentMemory*,uint32_t offset,uint32_t size);

/* Callback contract: materialize must synchronously write ALL active rows
 * from this immutable token, preserving stride padding, and return success
 * only when those bytes are readable. Failure may have written some bytes;
 * ownership is retained so a retry can reconstruct the entire copy.
 * Materialization writes bypass this manager's access barriers. Mutation,
 * preparation, flush and destroy are rejected during either callback. No
 * guest/foreign memory access may run from a callback. For publish_slices,
 * write only next_span's surviving intervals of those original rows, including
 * on retry. Source offsets always refer to the unchanged original geometry.
 * release cannot fail;
 * it runs exactly once after successful materialization or discard. */

/* Queries copy metadata by value; token lifetime still ends at retirement.
 * copy_at enumerates owned records, not stable slots. find tests an active
 * byte, excluding stride padding. Owner queries are also allowed during callbacks. */
bool gcn_resident_memory_copy_at(const GcnResidentMemory*,uint32_t index,GcnResidentCopy*);
bool gcn_resident_memory_find(const GcnResidentMemory*,uint32_t offset,GcnResidentCopy*);
/* Exact owner-thread query of active bytes; unlike pending(), stride padding
 * sharing a flagged page is excluded. Invalid ranges return true. */
bool gcn_resident_memory_owned_range(const GcnResidentMemory*,uint32_t offset,uint32_t size);
GcnResidentMemoryStats gcn_resident_memory_stats(const GcnResidentMemory*);
GcnResidentError gcn_resident_memory_error(const GcnResidentMemory*);
#ifdef __cplusplus
}
#endif
#endif
