/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_RESOLUTION_H
#define GC_WII_NATIVE_RESOLUTION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_RESOLUTION_ABI 1u
/* Owner-thread operation between frames. Guest EFB coordinates, copy sizes
 * and CPU observations remain in their original units. Existing immutable
 * display generations retain their own raster resolution. */
typedef struct {
    uint32_t abi;
    int (*set_scale)(uint32_t scale);
    int (*raster_size)(uint32_t* width,uint32_t* height);
} GcWiiNativeResolutionApi;
typedef const GcWiiNativeResolutionApi* (*GcWiiNativeResolutionGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
