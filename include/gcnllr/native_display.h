/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_DISPLAY_H
#define GC_WII_NATIVE_DISPLAY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_DISPLAY_ABI 1u
/* Owner-thread display aspect for subsequent VI presentations. This changes
 * only the host presentation viewport, never EFB/XFB storage or window bounds.
 * (0,0) restores the source image's aspect. Invalid ratios have no effects. */
typedef struct {
    uint32_t abi;
    int (*set_aspect)(uint32_t numerator,uint32_t denominator);
} GcWiiNativeDisplayApi;
typedef const GcWiiNativeDisplayApi* (*GcWiiNativeDisplayGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
