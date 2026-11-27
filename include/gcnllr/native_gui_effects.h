/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_GUI_EFFECTS_H
#define GC_WII_NATIVE_GUI_EFFECTS_H
#include "native_gui.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_GUI_EFFECTS_ABI 1u
/* Additional UVs are vertex-major: [vertex][extra_uv_count][2]. Conversion
 * copies all channels once; existing Render1 meshes still carry only UV0. */
typedef struct {
    GcWiiNativeMesh mesh;
    const float* extra_uv;
    uint32_t extra_uv_count;
} GcWiiNativeGuiMesh;
/* Named host materials, never an operation/register stream. Cutout retains
 * texture alpha above a byte cutoff, then applies pane alpha. Additive combines
 * two RGB images and uses pane alpha. Product multiplies primary/mask alpha
 * and opacity. These methods ignore pane RGB; existing API layouts are intact. */
typedef struct { GcWiiNativeGuiDraw image; uint32_t cutoff; } GcWiiNativeGuiCutoutDraw;
typedef struct {
    GcWiiNativeGuiDraw image;
    GcWiiNativeTexture addition;
    int32_t weight[4];
    float uv[2][3];
} GcWiiNativeGuiAddDraw;
typedef struct {
    GcWiiNativeGuiMaskDraw image;
    uint32_t secondary_uv; /* 1: retained UV1, 0: affine transform of UV0 */
    float primary_uv[2][3]; /* used with retained channels; image.uv maps UV1 */
} GcWiiNativeGuiProductDraw;
typedef struct {
    uint32_t abi;
    uint64_t (*create_mesh)(const GcWiiNativeGuiMesh*);
    int (*cutout)(const GcWiiNativeGuiCutoutDraw*);
    int (*add)(const GcWiiNativeGuiAddDraw*);
    int (*product)(const GcWiiNativeGuiProductDraw*);
} GcWiiNativeGuiEffectsApi;
typedef const GcWiiNativeGuiEffectsApi* (*GcWiiNativeGuiEffectsGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
