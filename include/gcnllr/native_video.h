/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_VIDEO_H
#define GC_WII_NATIVE_VIDEO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_VIDEO_ABI 1u
/* Post-scanout presentation only: guest EFB/XFB bytes remain unchanged.
 * 0 restores normal output; 1 selects the Wii SDK's 25% idle-dim preset.
 * Called on the machine owner. The next presentation captures this state,
 * including queued/owned copies.
 * Other controls/presets must not be silently approximated. */
typedef struct {
    uint32_t abi;
    int (*idle_dimming)(uint32_t enabled);
} GcWiiNativeVideoApi;
typedef const GcWiiNativeVideoApi* (*GcWiiNativeVideoGetApi)(uint32_t abi);
#ifdef __cplusplus
}
#endif
#endif
