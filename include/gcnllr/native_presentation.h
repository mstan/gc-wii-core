/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_PRESENTATION_H
#define GC_WII_NATIVE_PRESENTATION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_PRESENTATION_ABI 2u
/* A title-owned view for the NEXT display copy. The service copies the pixels
 * immediately and retains them with that immutable XFB generation. Presentation
 * adds only the margins: the center remains the actual VI image, with its HUD,
 * effects and color conversion. Physical EFB/XFB storage is never widened.
 * A null frame selects unmodified presentation for the next copy. The optional
 * NativeResolution extension independently rasterizes the same native jobs;
 * when enabled, its immutable high-resolution display replaces the VI center
 * without changing the guest EFB/XFB or this ABI's margin layout. */
typedef struct {
    const uint8_t* rgba;
    uint32_t width,height,stock_width,bytes;
} GcWiiNativePresentationFrame;
typedef struct {
    uint32_t abi;
    int (*set_frame)(uint32_t xfb_address,const GcWiiNativePresentationFrame*);
    /* Select the real VI address before RAM-backed presentation. Immutable
     * GPU generations already carry that identity. Never infer it from colors. */
    int (*select_source)(uint32_t xfb_address);
    int (*window_size)(uint32_t* width,uint32_t* height);
    int (*resize_window)(uint32_t logical_width,uint32_t logical_height);
    /* Read the actual enhanced GPU output, not guest RAM. Null output queries
     * its dimensions. Zero means the last presentation had no custom view. */
    int (*read_rgba)(uint8_t* output,uint32_t capacity,uint32_t* width,uint32_t* height);
    /* ABI 2: nearest-sampled, untinted logical copy image. The next display
     * copy supplies its real vertical filter; margins use the same integer
     * copy/VI color conversion as the center. tint packs R,G,B in the low three
     * bytes; output is opaque. A zero-alpha input pixel excludes that part of
     * the canvas after filtering (opaque black), preserving finite pillarboxes.
     * Requires both vertical copy clamps. ABI 1 remains available with
     * its original direct-RGB margin contract and function-table prefix. */
    int (*set_filtered_frame)(uint32_t xfb_address,const GcWiiNativePresentationFrame*,uint32_t tint);
} GcWiiNativePresentationApi;
typedef const GcWiiNativePresentationApi* (*GcWiiNativePresentationGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
