/* SPDX-License-Identifier: MIT */
#ifndef GCN_CPU_NATIVE_CANDIDATE_H
#define GCN_CPU_NATIVE_CANDIDATE_H
#include "cpu/native_code.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Keep broad dispatch eligibility without observing every data store. A
 * cached positive OR negative identity must call gcn_native_code_watch for
 * all bytes it depends on before publication. Explicit invalidation and
 * content-only dirtiness retain their distinct existing semantics.
 * Registration/reset invalidate the full candidate range.
 * Existing watches, REL observers and dirty/CRC fences are never removed.
 * The original registration API retains its full-range watch behavior. */
void gcn_native_code_set_candidate_scoped(u32 physical_start,u32 physical_end,
    GcnNativeCandidateFn call,GcnNativeCandidateInvalidateFn invalidate);
#ifdef __cplusplus
}
#endif
#endif
