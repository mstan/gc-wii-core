/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_VIEW_SCENE_H
#define GC_WII_NATIVE_VIEW_SCENE_H
#include <gcnllr/native_render.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_VIEW_SCENE_ABI 1u
/* Optional GPU-only margins for the next real XFB generation. Draws reuse
 * Render1 retained meshes and raw indexed assets, with an explicit physical
 * target viewport. Logical layout and physical raster dimensions are distinct;
 * unsupported/device-limited dimensions reject without silently reducing them.
 * Assets are consumed during submit under the existing immutable-resource
 * contract. No guest address is allocated and no CPU image is accepted.
 *
 * The offscreen target starts transparent. Caller draws opaque coverage/black
 * borders/backdrop and source tiles in their actual order; alpha zero excludes
 * finite room/panel pixels. Jobs must be untinted; tint is applied with the real
 * copy's integer vertical filter/YUY2 conversion. The center remains the actual
 * VI/NativeResolution image. EFB contents and guest copies remain unchanged.
 * A fresh GPU target is retained by the immutable XFB until it is retired. */
typedef struct {
    uint32_t width,height,stock_width,target_width,target_height;
    const GcWiiNativeDraw* draws;
    uint32_t count;
} GcWiiNativeViewScene;
typedef struct {
    uint32_t abi;
    int (*submit)(uint32_t xfb_address,const GcWiiNativeViewScene*,uint32_t tint);
} GcWiiNativeViewSceneApi;
typedef const GcWiiNativeViewSceneApi* (*GcWiiNativeViewSceneGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
