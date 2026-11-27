/* SPDX-License-Identifier: MIT */
#ifndef GC_WII_NATIVE_GUI_PROGRAM_H
#define GC_WII_NATIVE_GUI_PROGRAM_H
#include "native_gui_effects.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GC_WII_NATIVE_GUI_PROGRAM_ABI 1u
/* A complete bounded GUI shader description, lowered once through the shared
 * integer shader compiler. There is no FIFO, register-write stream or persistent
 * emulated graphics state. Existing named GUI and Render ABIs remain intact. */
typedef struct {
    uint8_t inputs[4];
    uint8_t operation,bias,scale,clamp,output;
} GcWiiNativeGuiCombine;
typedef struct {
    GcWiiNativeGuiCombine color,alpha;
    uint8_t texture_enabled,texture,coordinate,raster;
    uint8_t color_constant,alpha_constant;
    uint8_t texture_swap[4],raster_swap[4];
} GcWiiNativeGuiStage;
typedef struct {
    GcWiiNativeDraw draw;
    uint32_t texture_count,coordinate_count,stage_count;
    GcWiiNativeTexture textures[8];
    uint8_t coordinate_source[8]; /* retained mesh UV channel, 0..7 */
    float uv[8][2][3];
    int32_t colors[4][4],constants[4][4];
    GcWiiNativeGuiStage stages[16];
} GcWiiNativeGuiProgramDraw;
typedef struct {
    uint32_t abi;
    uint64_t (*create_mesh)(const GcWiiNativeGuiMesh*);
    int (*draw)(const GcWiiNativeGuiProgramDraw*);
} GcWiiNativeGuiProgramApi;
typedef const GcWiiNativeGuiProgramApi* (*GcWiiNativeGuiProgramGetApi)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
