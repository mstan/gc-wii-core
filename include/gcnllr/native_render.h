/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_RENDER_H
#define GC_WII_NATIVE_RENDER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Native scene submission, independent of the console FIFO. The existing
 * host GPU service owns startup, scanout, copies and shutdown. All calls run
 * on its recording owner; callers drain prior FIFO work before this interface.
 * This first material family is unlit vertex color, optionally multiplied by
 * one texture. Other material families must be implemented explicitly. */
#define GC_WII_NATIVE_RENDER_ABI 1u
#define GC_WII_NATIVE_BLEND 1u
#define GC_WII_NATIVE_DEPTH_TEST 2u
#define GC_WII_NATIVE_DEPTH_WRITE 4u

typedef struct {
    float position[3], color[4], uv[2];
} GcWiiNativeVertex;

typedef struct {
    const GcWiiNativeVertex* vertices;
    const uint16_t* indices;
    uint32_t vertex_count, index_count;
} GcWiiNativeMesh;

/* Physical memory identity and tiled resource format describe asset storage,
 * not commands to replay. The consumer resolves these bytes synchronously at
 * submission; retained GPU resources follow the existing memory epoch rules. */
typedef struct {
    uint32_t address, size, format, width, height;
    uint32_t palette_address, palette_entries, palette_format;
    uint32_t wrap_s, wrap_t, linear;
} GcWiiNativeTexture;

typedef struct {
    uint32_t flags;
    GcWiiNativeTexture texture; /* size zero: vertex color only */
} GcWiiNativeMaterial;

typedef struct {
    uint64_t mesh;
    GcWiiNativeMaterial material;
    /* Row-major model-view 3x4 and projection 4x4. */
    float model_view[12], projection[16];
    /* Logical EFB pixels and normalized depth range. */
    float x, y, width, height, near_depth, far_depth;
    int32_t clip_x, clip_y, clip_width, clip_height;
} GcWiiNativeDraw;

typedef struct {
    uint64_t mesh_uploads, draws, rejected, live_meshes, uploaded_vertex_bytes;
} GcWiiNativeRenderStatus;

typedef struct {
    uint32_t abi;
    /* Copies and converts geometry once. Zero is an invalid handle. Handles
     * survive frames, never survive service shutdown, and are never recycled. */
    uint64_t (*create_mesh)(const GcWiiNativeMesh*);
    int (*destroy_mesh)(uint64_t);
    int (*draw)(const GcWiiNativeDraw*);
    void (*status)(GcWiiNativeRenderStatus*);
} GcWiiNativeRenderApi;

typedef const GcWiiNativeRenderApi* (*GcWiiNativeRenderGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
