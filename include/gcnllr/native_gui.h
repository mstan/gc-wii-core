/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_GUI_H
#define GC_WII_NATIVE_GUI_H
#include "native_render.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_GUI_ABI 1u
/* A host GUI material: remap a texture between byte color endpoints,
 * clamp to byte color, then modulate by the interpolated pane color. With
 * no texture, use the high endpoint times pane color. No command/register
 * stream is accepted. Both endpoints must be in 0..255; wider source colors
 * require a separately qualified material family. */
typedef struct {
    int32_t low[4],high[4];
} GcWiiNativeGuiMaterial;
typedef struct {
    GcWiiNativeDraw draw;
    GcWiiNativeGuiMaterial color;
} GcWiiNativeGuiDraw;
/* Meshes/resources and recording ownership are shared with RenderAPI1. */
typedef struct {
    uint32_t abi;
    int (*draw)(const GcWiiNativeGuiDraw*);
} GcWiiNativeGuiApi;
typedef const GcWiiNativeGuiApi* (*GcWiiNativeGuiGetApi)(uint32_t);

/* Three-image GUI background: clamp the primary RGB range remap, then add
 * each weighted overlay in order with byte saturation after each addition.
 * Alpha is color.high[3]. Pane tint is not part of this material family.
 * Overlay coordinates are affine transforms of the retained primary UV.
 * Existing GUI/Render/Frame API1 layouts are independent and unchanged. */
#define GC_WII_NATIVE_GUI_LAYERS_ABI 1u
typedef struct {
    GcWiiNativeDraw draw;
    GcWiiNativeGuiMaterial color;
    GcWiiNativeTexture overlays[2];
    int32_t weights[2][4];
    float uv[2][2][3];
} GcWiiNativeGuiLayersDraw;
typedef struct {
    uint32_t abi;
    int (*draw)(const GcWiiNativeGuiLayersDraw*);
} GcWiiNativeGuiLayersApi;
typedef const GcWiiNativeGuiLayersApi* (*GcWiiNativeGuiLayersGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
