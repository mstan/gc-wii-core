/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_INDEXED_H
#define GC_WII_NATIVE_INDEXED_H
#include <gcnllr/native_render.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit native asset family, outside the guest GX TLUT namespace. Source
 * colors are big-endian RGB5551; RGB is converted through the actual RGB565
 * copy representation. Index zero is transparent regardless of its RGB/LSB.
 * The original native material and guest TLUT families remain unchanged. */
#define GC_WII_NATIVE_PALETTE_RGB5551_ZERO 0x4d390001u
#define GC_WII_NATIVE_INDEXED_ABI 1u
typedef struct {
    uint32_t abi;
    /* Reuses Render1 mesh handles. Accepts nearest CI4 with exactly 16 colors
     * and the named palette family; source bytes follow the existing resource
     * epoch/snapshot ownership rules. Unsupported descriptors reject. */
    int (*draw)(const GcWiiNativeDraw*);
} GcWiiNativeIndexedApi;
typedef const GcWiiNativeIndexedApi* (*GcWiiNativeIndexedGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
