/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_GX_MEMORY_H
#define GCN_GX_MEMORY_H
#include "cpu/cpu.h"
#include "cpu/native_code.h"
#include "memory/memory.h"

/* GX DMA addresses identify physical main RAM. Keep MEM2 distinct from MEM1
 * and stop every view at its bank boundary; MMIO and locked L1 are not RAM. */
static inline u8* gcn_gx_memory(CPUState* cpu, u64 address, u64 size,
                               u32* available) {
    if (available) *available = 0;
    if (!cpu || address > 0xffffffffu) return NULL;
    u32 alias = (u32)address & 0xe0000000u;
    if (alias != 0 && alias != 0x80000000u && alias != 0xc0000000u) return NULL;
    u32 physical = (u32)address & 0x1fffffffu;
    u8* bytes = NULL;
    u32 length = 0;
    if (cpu->ram && physical < cpu->ram_size) {
        bytes = cpu->ram + physical;
        length = cpu->ram_size - physical;
    } else if (cpu->mem2 && physical >= 0x10000000u &&
               physical - 0x10000000u < cpu->mem2_size) {
        u32 offset = physical - 0x10000000u;
        bytes = cpu->mem2 + offset;
        length = cpu->mem2_size - offset;
    }
    if (size > length || !bytes) return NULL;
    if (available) *available = length;
    return bytes;
}

/* Device stores invalidate compiled contents and overlapping reservations at
 * the physical identity, including MEM2. Call after the bytes are visible. */
static inline void gcn_gx_memory_written(CPUState* cpu, u32 physical, u32 size) {
    gcn_native_code_content_dirty(physical, size);
    if (cpu && cpu->reserve_valid && size) {
        u32 reservation = gcn_reservation_key(cpu->reserve_addr) & ~31u;
        if ((u64)reservation + 32u > physical &&
            reservation < (u64)physical + size) cpu->reserve_valid = false;
    }
}
#endif
