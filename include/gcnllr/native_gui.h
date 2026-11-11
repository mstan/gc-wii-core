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
#ifdef __cplusplus
}
#endif
#endif
