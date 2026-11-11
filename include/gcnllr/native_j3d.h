/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_J3D_H
#define GC_WII_NATIVE_J3D_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_J3D_ABI 1u
#define GC_WII_J3D_MATERIAL_COLORS 1u
#define GC_WII_J3D_REGISTER_COLORS 2u
#define GC_WII_J3D_CONSTANT_COLORS 4u
#define GC_WII_J3D_TEXTURE_INDICES 8u
#define GC_WII_J3D_LIGHTING 16u
#define GC_WII_J3D_FOG 32u
typedef struct GcWiiJ3DChannel {
    uint8_t enabled, material_source, ambient_source, diffuse, attenuation, light_mask;
    uint8_t reserved[2];
} GcWiiJ3DChannel;
typedef struct GcWiiJ3DLight {
    int32_t color[4];
    float cosatt[4], distatt[4], position[4], direction[4];
} GcWiiJ3DLight;
/* These are host rendering inputs, with row-major matrices and logical EFB
 * coordinates. The title adapter supplies current animation/instance values;
 * resources are converted once, rather than reissuing the original commands. */
typedef struct GcWiiJ3DDraw {
    uint64_t resource;
    uint32_t mesh, material;
    float projection[16];
    float positions[10][12], normals[10][9], texture_matrices[8][12];
    GcWiiJ3DLight lights[8];
    float cached_normal[3], cached_binormal[3], cached_tangent[3];
    float x, y, width, height, near_depth, far_depth;
    int32_t clip_x, clip_y, clip_width, clip_height;
    uint32_t position_mask, normal_mask, texture_matrix_mask, light_mask;
    /* Asset recipes that omit culling require an explicit caller value. */
    uint32_t cull_mode;
    uint32_t material_inputs;
    /* Zero preserves the original all-four-row override. Otherwise bits 0..3
     * select ambient0/ambient1/material0/material1 independently. A material
     * diff that updates only material colors supplies mask 0xC. */
    uint32_t material_color_mask;
    int32_t material_colors[4][4], register_colors[4][4], constant_colors[4][4];
    uint16_t texture_indices[8];
    GcWiiJ3DChannel channels[4]; /* color0, color1, alpha0, alpha1 */
    uint32_t channel_count;
    /* J3DFogInfo's declarative recipe, not a graphics command stream. */
    uint8_t fog_recipe[44];
} GcWiiJ3DDraw;
typedef struct GcWiiJ3DStatus {
    uint64_t resources, meshes, draws, rejected;
    char error[192];
} GcWiiJ3DStatus;
typedef struct GcWiiNativeJ3DApi {
    uint32_t abi;
    uint64_t (*create_resource)(uint32_t guest_address);
    int (*destroy_resource)(uint64_t);
    int (*draw)(const GcWiiJ3DDraw*);
    void (*status)(GcWiiJ3DStatus*);
} GcWiiNativeJ3DApi;
#ifdef __cplusplus
}
#endif
#endif
