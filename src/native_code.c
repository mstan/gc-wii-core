/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/native_candidate.h"
#include "cpu/rel_module.h"

#include <string.h>

#define GCN_CODE_PAGE_SHIFT 12u
#define GCN_CODE_PAGE_SIZE (1u << GCN_CODE_PAGE_SHIFT)
#define GCN_RAM_PAGE_COUNT (GCN_NATIVE_RAM_LIMIT / GCN_CODE_PAGE_SIZE)

static u8 s_invalid[(GCN_RAM_PAGE_COUNT + 7u) / 8u];
static u32 s_invalid_count;
static u32 s_candidate_start, s_candidate_end;
static GcnNativeCandidateFn s_candidate;
static GcnNativeCandidateInvalidateFn s_candidate_invalidate;
static bool s_candidate_full_watch = true;

/* Content-staleness companion to s_invalid, consumed by interpreter.c's
 * native-miss page-CRC memo. A page's CRC identity can only change through
 * the same funnel that invalidates native code (guest stores via memory.c,
 * device DMA, icbi via cpu_glue.c / the interpreter batch path), so setting
 * a bit here in the same loop lets the miss path reuse its cached CRC until
 * the page could actually have changed -- identical identity semantics to
 * recomputing the CRC on every miss, without hashing 4 KiB per duplicate.
 * Unlike s_invalid (monotonic until reset), these bits are read-and-clear. */
static u8 s_content_stale[(GCN_RAM_PAGE_COUNT + 7u) / 8u];
u8 gcn_native_store_pages[GCN_RAM_PAGE_COUNT];

static bool ram_offset(u32 address, u32* offset) {
    if (address < GC_MAIN_RAM_SIZE) {
        *offset = address;
        return true;
    }
    if (address >= GC_RAM_BASE &&
        address < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_BASE;
        return true;
    }
    if (address >= GC_RAM_UNCACHED &&
        address < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_UNCACHED;
        return true;
    }
    const u32 physical=address&0x1fffffffu;
    if ((address>>29u==0 || address>>29u==4 || address>>29u==6) &&
        physical>=0x10000000u && physical<GCN_NATIVE_RAM_LIMIT) {
        *offset=physical;return true;
    }
    return false;
}

static u32 bank_limit(u32 offset) {return offset>=0x10000000u?GCN_NATIVE_RAM_LIMIT:GC_MAIN_RAM_SIZE;}

void gcn_native_code_reset(void) {
    gcn_rel_reset();
    memset(s_invalid, 0, sizeof s_invalid);
    memset(gcn_native_store_pages,0,sizeof gcn_native_store_pages);
    s_invalid_count = 0;
    /* All-stale, not all-clean: interpreter.c may hold CRCs cached from
     * before the reset, and reset means the backing bytes are about to be
     * repopulated. */
    memset(s_content_stale, 0xFF, sizeof s_content_stale);
    if (s_candidate_invalidate) {
        if (s_candidate_full_watch)
            gcn_native_code_watch(s_candidate_start,s_candidate_end-s_candidate_start);
        s_candidate_invalidate(s_candidate_start, s_candidate_end - s_candidate_start);
    }
}

static void set_candidate(u32 start,u32 end,GcnNativeCandidateFn call,
                          GcnNativeCandidateInvalidateFn invalidate,bool full_watch) {
    s_candidate_start = start;
    s_candidate_end = end;
    s_candidate = start < end && end <= GCN_NATIVE_RAM_LIMIT ? call : NULL;
    s_candidate_invalidate = s_candidate ? invalidate : NULL;
    s_candidate_full_watch = full_watch;
    if (s_candidate_invalidate) {
        if (full_watch) gcn_native_code_watch(start,end-start);
        s_candidate_invalidate(start, end - start);
    }
}

void gcn_native_code_set_candidate(u32 start,u32 end,
    GcnNativeCandidateFn call,GcnNativeCandidateInvalidateFn invalidate) {
    set_candidate(start,end,call,invalidate,true);
}

void gcn_native_code_set_candidate_scoped(u32 start,u32 end,
    GcnNativeCandidateFn call,GcnNativeCandidateInvalidateFn invalidate) {
    set_candidate(start,end,call,invalidate,false);
}

void gcn_native_code_watch(u32 address,u32 size) {
    u32 offset;
    if (!size || !ram_offset(address,&offset)) return;
    u64 end=(u64)offset+size;
    if(offset<GC_MAIN_RAM_SIZE && end>0x10000000u)
        gcn_native_code_watch((address&0xe0000000u)|0x10000000u,
            (u32)((end>GCN_NATIVE_RAM_LIMIT?GCN_NATIVE_RAM_LIMIT:end)-0x10000000u));
    if (end>bank_limit(offset)) end=bank_limit(offset);
    for (u32 page=offset>>GCN_CODE_PAGE_SHIFT; page<=(end-1u)>>GCN_CODE_PAGE_SHIFT; page++)
        gcn_native_store_pages[page] |= GCN_NATIVE_STORE_OBSERVED;
}

int gcn_native_code_try_candidate(CPUState* cpu, u32 pc) {
    return s_candidate && pc >= s_candidate_start && pc < s_candidate_end
        ? s_candidate(cpu, pc) : 0;
}

void gcn_native_code_invalidate(u32 address, u32 size) {
    gcn_rel_invalidate(address, size);
    u32 offset;
    if (!size || !ram_offset(address, &offset))
        return;

    u64 end = (u64)offset + size;
    if(offset<GC_MAIN_RAM_SIZE && end>0x10000000u)
        gcn_native_code_invalidate((address&0xe0000000u)|0x10000000u,
            (u32)((end>GCN_NATIVE_RAM_LIMIT?GCN_NATIVE_RAM_LIMIT:end)-0x10000000u));
    if (end > bank_limit(offset))
        end = bank_limit(offset);
    if (s_candidate_invalidate && offset < s_candidate_end && end > s_candidate_start)
        s_candidate_invalidate(address, (u32)(end-offset));
    u32 first = offset >> GCN_CODE_PAGE_SHIFT;
    u32 last = (u32)(end - 1u) >> GCN_CODE_PAGE_SHIFT;
    for (u32 page = first; page <= last; page++) {
        u8 mask = (u8)(1u << (page & 7u));
        u8* byte = &s_invalid[page >> 3];
        if (!(*byte & mask)) {
            *byte |= mask;
            s_invalid_count++;
        }
        s_content_stale[page >> 3] |= mask;
        gcn_native_store_pages[page] |= GCN_NATIVE_STORE_DIRTY;
    }
}

void gcn_native_code_content_dirty(u32 address, u32 size) {
    gcn_rel_invalidate(address, size);
    u32 offset;
    if (!size || !ram_offset(address, &offset))
        return;
    u64 end = (u64)offset + size;
    if(offset<GC_MAIN_RAM_SIZE && end>0x10000000u)
        gcn_native_code_content_dirty((address&0xe0000000u)|0x10000000u,
            (u32)((end>GCN_NATIVE_RAM_LIMIT?GCN_NATIVE_RAM_LIMIT:end)-0x10000000u));
    if (end > bank_limit(offset))
        end = bank_limit(offset);
    u32 first = offset >> GCN_CODE_PAGE_SHIFT;
    u32 last = (u32)(end - 1u) >> GCN_CODE_PAGE_SHIFT;
    for (u32 page = first; page <= last; page++) {
        const u8 mask=(u8)(1u << (page & 7u));
        s_content_stale[page >> 3] |= mask;
        if (s_invalid[page>>3]&mask) gcn_native_store_pages[page] |= GCN_NATIVE_STORE_DIRTY;
    }
}

bool gcn_native_code_page_content_stale_take(u32 pc) {
    u32 offset;
    if (!ram_offset(pc, &offset))
        return true;   /* outside RAM: no staleness tracking, always stale */
    u32 page = offset >> GCN_CODE_PAGE_SHIFT;
    u8 mask = (u8)(1u << (page & 7u));
    u8* byte = &s_content_stale[page >> 3];
    if (*byte & mask) {
        *byte &= (u8)~mask;
        gcn_native_store_pages[page] &= (u8)~GCN_NATIVE_STORE_DIRTY;
        return true;
    }
    return false;
}

bool gcn_native_code_is_invalid(u32 pc) {
    u32 offset;
    if (!ram_offset(pc, &offset))
        return false;
    u32 page = offset >> GCN_CODE_PAGE_SHIFT;
    return (s_invalid[page >> 3] & (u8)(1u << (page & 7u))) != 0;
}

u32 gcn_native_code_invalid_page_count(void) {
    return s_invalid_count;
}
