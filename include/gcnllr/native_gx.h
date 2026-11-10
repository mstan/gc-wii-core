/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_NATIVE_GX_H
#define GCNLLR_NATIVE_GX_H
#include "display.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GCNLLR_NATIVE_GX_ABI 11u
#define GCNLLR_NATIVE_GX_COMPLETED_SCANOUT 1u
#define GCNLLR_NATIVE_GX_RESIDENT_COPY 2u
#define GCNLLR_NATIVE_GX_STREAMED_COPY 4u
#define GCNLLR_NATIVE_GX_COMPOSED_SCANOUT 8u
#define GCNLLR_NATIVE_GX_DEFERRED_RETIREMENT 16u
#define GCNLLR_NATIVE_GX_COPY_DISPLAY 1u
#define GCNLLR_NATIVE_GX_COPY_MATERIALIZING 2u

typedef struct {
    uint64_t token;
    uint32_t address, row_bytes, rows, stride;
    /* DISPLAY selects packed YUY2 scanout; zero selects tiled texture rows.
     * The opaque token retains the provider's immutable format/geometry. */
    uint32_t flags;
} GcnllrNativeGxCopy;

/* Packed YUY2 spans from immutable display-copy generations. Source offsets
 * exclude guest stride padding. Destination spans cover the packed scanout
 * once, in order. Token zero selects CPU-owned bytes at normalized MEM1 source
 * offsets. They retain no lease after the synchronous call returns. */
typedef struct {
    uint64_t token;
    uint32_t source_offset,destination_offset,size;
} GcnllrNativeGxScanoutSpan;

/* Consumer-owned coherent memory. Physical addresses identify the same bytes
 * in resource/epoch/copy callbacks. Available bytes stop at the bank boundary;
 * neither the renderer nor its donor may fold MEM2 onto MEM1. */
typedef struct {
    uint8_t* bytes;
    uint32_t physical_address,available;
} GcnllrNativeGxMemory;
typedef struct {
    uint32_t abi;
    const char* title;
    const char* data_directory;
    const char* cache_directory;
    uint32_t hidden;
    /* Legacy single-bank GameCube convenience. Multi-bank consumers supply
     * both memory callbacks below instead of exporting their console map. */
    uint8_t* mem1;
    uint32_t mem1_size;
    void* user;
    /* Ordered PE completion/token registers (45/47/48), invoked on the
     * calling machine thread after earlier GPU memory effects. Draw-only BP
     * state belongs to the renderer. No foreign CPU state crosses the ABI. */
    void (*bp_write)(void* user, uint32_t reg, uint32_t value);
    int (*write_copy)(void* user, uint32_t address, const uint8_t* bytes, uint32_t size,
                      uint32_t display, uint32_t width, uint32_t height, uint32_t stride);
    /* Synchronous translation only: no CPU/DMA writes run during a query.
     * Reports all writes, including raw compiled stores. Zero means hash
     * actual bytes. Threaded translation rejects this stopped-writer API. */
    int (*memory_epoch)(void* user,uint32_t address,uint32_t size,uint64_t* epoch);
    /* Optional GPU-visible cache publication (CPU cache flushes and completed
     * device writes). Concurrent-safe, distinct from raw CPU-store epochs.
     * Never use this to retain an EFB shadow over changed actual MEM1 bytes. */
    int (*texture_epoch)(void* user,uint32_t address,uint32_t size,uint64_t* epoch);
    /* Optional build-selected memory ownership. Publication occurs on the
     * machine thread before PE completion. A successful publication transfers
     * this immutable GPU generation to the caller until release_copy(). */
    int (*own_copy)(void* user,const GcnllrNativeGxCopy* copy);
    /* pending_read is a concurrent-safe conservative page query. Only an
     * actual pending span needs prepare_read on the machine owner. */
    int (*pending_read)(void* user,uint32_t address,uint32_t size);
    int (*prepare_read)(void* user,uint32_t address,uint32_t size);
    /* Owner-thread completion of this exact published generation. Already
     * observed/overwritten/replaced tokens are successful no-ops. A pending
     * token must publish its real RAM bytes before this callback succeeds.
     * Range-based retirement could accidentally force a newer overlapping
     * generation, hence the explicit token contract. */
    int (*retire_copy)(void* user,uint64_t token);
    int (*resolve_memory)(void* user,uint32_t address,uint32_t size,GcnllrNativeGxMemory* range);
    int (*locate_memory)(void* user,const void* bytes,uint32_t size,uint32_t* physical_address);
} GcnllrNativeGxConfig;
typedef struct {
    uint64_t fifo_bytes, draws, display_copies, texture_copies, readback_ns;
    uint32_t failed, quit;
    GcnllrPad pad;
    uint64_t presents;
    uint64_t texture_generation_hits,texture_hashes,epoch_queries,epoch_cached;
    uint64_t verified_copy_hits,verified_copy_misses;
    uint64_t resident_copies,resident_materializations,resident_scanouts;
    uint64_t resident_textures,resident_texture_hits,resident_texture_materializations;
    uint64_t resident_texture_incompatible;
} GcnllrNativeGxStatus;
typedef struct {
    uint32_t abi;
    int (*start)(const GcnllrNativeGxConfig*);
    int (*fifo)(const uint8_t*, uint32_t);
    int (*drain)(void);
    void (*status)(GcnllrNativeGxStatus*);
    int (*peek)(uint32_t depth, uint16_t x, uint16_t y, uint32_t* value);
    void (*stop)(void);
    int (*present)(const uint8_t* yuy2, uint32_t width, uint32_t height, uint32_t stride);
    /* Publish effects on the machine owner. The ordinary cooperative provider
     * polls without waiting. STREAMED_COPY joins its submitted batch at an
     * earned device boundary, so host GPU latency does not consume extra guest
     * time. It need not flush unrelated pending draw assembly. drain/peek
     * remain full ordered barriers. */
    int (*service)(void);
    /* COMPLETED_SCANOUT: present reads the supplied completed MEM1 XFB while
     * coordinating recording ownership; it need not drain unrelated queued
     * work. The machine still owns VI selection and ordered copy publication. */
    uint32_t capabilities;
    /* Materialize one immutable copy without waiting for the FIFO worker:
     * this can be called while that worker awaits an owner publication.
     * write_copy receives MATERIALIZING and the retained copy's DISPLAY flag
     * for these already-published effects. Texture rows are tiled block rows.
     * release_copy is required once the caller retires ownership. */
    int (*materialize_copy)(uint64_t token);
    void (*release_copy)(uint64_t token);
    /* 1 = presented this exact generation/layout, 0 = use materialized MEM1,
     * -1 = terminal renderer failure. VI selection remains machine-owned. */
    int (*present_copy)(uint64_t token,uint32_t width,uint32_t height,uint32_t stride);
    /* Same return convention as present_copy. No CPU memory observation;
     * declined layouts use the ordinary materialized-MEM1 scanout path. */
    int (*present_spans)(const GcnllrNativeGxScanoutSpan*,uint32_t count,
                         uint32_t width,uint32_t height);
} GcnllrNativeGxApi;
typedef const GcnllrNativeGxApi* (*GcnllrNativeGxGetApi)(uint32_t abi);
#ifdef __cplusplus
}
#endif
#endif
