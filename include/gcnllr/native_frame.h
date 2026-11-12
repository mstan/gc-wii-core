/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_FRAME_H
#define GC_WII_NATIVE_FRAME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Typed EFB operations on the GPU recording owner. Startup, memory banks,
 * immutable copy ownership and VI remain owned by the existing GPU service.
 * Drain earlier machine ingress before entry; these calls neither construct
 * commands nor change the legacy frontend's registers. */
#define GC_WII_NATIVE_FRAME_ABI 1u
#define GC_WII_NATIVE_CLEAR_COLOR 1u
#define GC_WII_NATIVE_CLEAR_ALPHA 2u
#define GC_WII_NATIVE_CLEAR_DEPTH 4u
#define GC_WII_NATIVE_COPY_CLAMP_TOP 1u
#define GC_WII_NATIVE_COPY_CLAMP_BOTTOM 2u
#define GC_WII_NATIVE_COPY_CLEAR 4u
#define GC_WII_NATIVE_COMPLETE_DRAW_DONE 1u
#define GC_WII_NATIVE_COMPLETE_TOKEN 2u
#define GC_WII_NATIVE_COMPLETE_TOKEN_INTERRUPT 4u

typedef struct {
    uint32_t x,y,width,height;
    uint32_t red,green,blue,alpha,depth; /* RGBA8 and raw Z24. */
    uint32_t flags; /* Independent color, alpha and depth write masks. */
} GcWiiNativeClear;

/* The supported destination is packed YUY2 at the actual physical/cached
 * guest address, never a synthetic resource address. This version requires
 * tightly packed rows and 32-byte destination/stride alignment. Source is the logical
 * physical EFB (640x528 in the Wii build; 640x480 in the GameCube profile),
 * with even x/width. Scale is the copy engine's Q8.8 factor;
 * invert_y_scale selects its reciprocal. Gamma 0 is identity; other gamma
 * modes currently reject. All seven six-bit filter taps are explicit,
 * including an all-zero (black) filter. Multisample filtering is unsupported.
 * Post-copy clear uses this source rectangle and clear_value's masks/value;
 * its rectangle is ignored. Caller selects write masks for its EFB format. */
typedef struct {
    uint32_t destination,stride;
    uint32_t x,y,width,height;
    uint32_t scale_y,invert_y_scale,gamma,flags;
    uint32_t filter[7];
    GcWiiNativeClear clear_value;
} GcWiiNativeDisplayCopy;

typedef struct {
    uint64_t clears,display_copies,completions,rejected;
    char error[192];
} GcWiiNativeFrameStatus;

typedef struct {
    uint32_t abi;
    int (*clear)(const GcWiiNativeClear*);
    int (*display_copy)(const GcWiiNativeDisplayCopy*);
    /* Publish preceding copy effects and ordered host work. The existing
     * build's physical/owned completion policy is preserved. Optional PE
     * draw-done/token effects use its existing owner callback, after copies;
     * TOKEN and TOKEN_INTERRUPT are mutually exclusive, token is 16-bit.
     * A zero event mask is a barrier without a PE notification. */
    int (*complete)(uint32_t events,uint32_t token);
    void (*status)(GcWiiNativeFrameStatus*);
} GcWiiNativeFrameApi;
typedef const GcWiiNativeFrameApi* (*GcWiiNativeFrameGetApi)(uint32_t);

/* Unscaled color EFB capture into tightly packed tiled texture memory.
 * Formats 0..6 are I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8. No clear, mip chain,
 * filter, depth conversion or command stream. The RGB8 source is opaque.
 * Destination aliases resolve through the consumer's coherent memory map;
 * existing immutable-copy ownership and CPU observation rules apply.
 * Separate extension: NativeFrame1 and its status layout remain unchanged. */
#define GC_WII_NATIVE_CAPTURE_ABI 1u
typedef struct {
    uint32_t destination,x,y,width,height,format;
} GcWiiNativeTextureCopy;
typedef struct {
    uint32_t abi;
    int (*texture_copy)(const GcWiiNativeTextureCopy*);
} GcWiiNativeCaptureApi;
typedef const GcWiiNativeCaptureApi* (*GcWiiNativeCaptureGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
