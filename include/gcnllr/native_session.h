/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_SESSION_H
#define GC_WII_NATIVE_SESSION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_SESSION_ABI 1u
/* Optional owner-thread title handoff. The consumer must keep the module
 * loaded until its following start succeeds or cancellation completes.
 * retain_next_stop returns zero without effects when retention is unavailable.
 * A successful stop still retires every guest resource and memory binding;
 * only the physical host window/device survive the following start.
 * cancel_retention also releases a stopped, retained host on failed handoff.
 * window_identity is diagnostic identity, not a user-facing device name. */
typedef struct {
    uint32_t abi;
    int (*retain_next_stop)(void);
    void (*cancel_retention)(void);
    uint64_t (*window_identity)(void);
} GcWiiNativeSessionApi;
typedef const GcWiiNativeSessionApi* (*GcWiiNativeSessionGetApi)(uint32_t abi);
#ifdef __cplusplus
}
#endif
#endif
