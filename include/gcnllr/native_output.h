/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_OUTPUT_H
#define GC_WII_NATIVE_OUTPUT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_OUTPUT_ABI 1u
/* Optional immutable-output ownership contract. The consumer must install
 * own_copy, pending_read and prepare_read, and guard EVERY CPU/host/DMA access
 * to owned memory, including raw compiled pointers. GPU work never writes RAM.
 * This seals and acknowledges earlier host submission; it does not wait for
 * GPU execution, publish RAM bytes or emit a PE notification. Actual memory
 * observations must synchronously materialize the retained generation.
 * NativeFrame1 remains the physical completion interface in this profile. */
typedef struct {uint32_t abi;int (*submit)(void);} GcWiiNativeOutputApi;
typedef const GcWiiNativeOutputApi* (*GcWiiNativeOutputGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
