/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Content-keyed dispatch for relocated overlay code. See
 * include/cpu/overlay_module.h for why this exists separately from
 * cpu/aot_module.h.
 */
#include "cpu/overlay_module.h"
#include "cpu/aot_module.h"   /* gcn_aot_fnv1a64 -- one hash function, shared */
#include "debug/cost_ring.h"

/* Same three MEM1 windows the reservation logic and the AOT module fold
 * together: cached, uncached, and the bare physical view used in real mode.
 * Overlay pages are recorded in the cached form, so everything canonicalizes
 * to that before lookup. */
static bool canonical_mem1(u32 address, u32* canonical, u32* offset) {
    if (address < GC_MAIN_RAM_SIZE) {
        *offset = address;
        *canonical = address | GC_RAM_BASE;
        return true;
    }
    if (address >= GC_RAM_BASE && address < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_BASE;
        *canonical = address;
        return true;
    }
    if (address >= GC_RAM_UNCACHED &&
        address < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_UNCACHED;
        *canonical = *offset | GC_RAM_BASE;
        return true;
    }
    return false;
}

static int find_page(const GcnOverlayModule* module, u32 canonical) {
    u32 lo = 0;
    u32 hi = module->page_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        const GcnOverlayPage* page = &module->pages[mid];
        if (canonical < page->start)
            hi = mid;
        else if (canonical >= page->end)
            lo = mid + 1u;
        else
            return (int)mid;
    }
    return -1;
}

static int index_slot(u32 address) {
    u32 canonical,offset;
    if(!canonical_mem1(address,&canonical,&offset))return -1;
    const u32 bank=address<GC_MAIN_RAM_SIZE?0u:address<GC_RAM_UNCACHED?1u:2u;
    return (int)(bank*(GC_MAIN_RAM_SIZE/4096u)+offset/4096u);
}

bool gcn_overlay_module_index(GcnOverlayModule* module,u32* storage,u32 capacity) {
    if(!module)return false;
    module->index=NULL;
    if(!storage || capacity<GCN_OVERLAY_INDEX_SLOTS)return false;
    for(u32 i=0;i<GCN_OVERLAY_INDEX_SLOTS;++i)storage[i]=GCN_OVERLAY_NONE;
    for(u32 i=0;i<module->page_count;++i) {
        const GcnOverlayPage* page=&module->pages[i];
        const int slot=index_slot(page->start);
        if(slot<0 || (page->start&4095u) || page->end-page->start!=4096u ||
           storage[slot]!=GCN_OVERLAY_NONE)return false;
        storage[slot]=i;
    }
    module->index=storage;
    return true;
}

static int lookup_page(const GcnOverlayModule* module,u32 address) {
#if GCN_OVERLAY_INDEXED
    if(module->index) {
        const int slot=index_slot(address);
        if(slot<0 || module->index[slot]==GCN_OVERLAY_NONE)return -1;
        return (int)module->index[slot];
    }
#endif
    return find_page(module,address);
}

/* Hash the page's live bytes once and record which compiled variant (if any)
 * is currently resident. Only reached after an invalidation, never per call. */
static u32 resolve_resident(GcnOverlayModule* module, CPUState* cpu,
                            u32 page_index, u32 offset) {
    const GcnOverlayPage* page = &module->pages[page_index];
    u32 length = page->end - page->start;
    if (offset > cpu->ram_size || length > cpu->ram_size - offset) {
        module->resident[page_index] = GCN_OVERLAY_NONE;
        return GCN_OVERLAY_NONE;
    }

    u64 live = gcn_aot_fnv1a64(cpu->ram + offset, length);
    module->hashes++;

    u32 chosen = GCN_OVERLAY_NONE;
    for (u32 i = 0; i < page->count; i++) {
        if (module->variants[page->first + i].hash == live) {
            chosen = page->first + i;
            break;
        }
    }
    if (chosen != GCN_OVERLAY_NONE)
        module->pages_resident++;
    module->resident[page_index] = chosen;
    return chosen;
}

bool gcn_overlay_module_call(GcnOverlayModule* module, CPUState* cpu,
                             u32 address) {
    u32 canonical;
    u32 offset;
    if (!module || !module->page_count || !cpu || !cpu->ram ||
        !canonical_mem1(address, &canonical, &offset))
        return false;
    /* Lookup preserves the execution namespace. A physical/uncached entry
     * requires its own compiled body; identical RAM does not authorize
     * rewriting observable branch/LR/exception PCs. */
    int found = lookup_page(module, address);
    if (found < 0)
        return false;
    u32 page_index = (u32)found;

    u32 resident = module->resident[page_index];
    if (resident == GCN_OVERLAY_UNKNOWN) {
        /* Hash the whole page, not the entry offset. */
        u32 page_offset, ignored;
        if (!canonical_mem1(module->pages[page_index].start, &ignored, &page_offset))
            return false;
        resident = resolve_resident(module, cpu, page_index, page_offset);
    }
    if (resident == GCN_OVERLAY_NONE)
        return false;

    /* The generated body switches on ctx->pc, so a mid-page entry is fine. */
    const unsigned previous=gcn_cost_enter(GCN_COST_GUEST);
    module->variants[resident].fn(cpu);
    gcn_cost_leave(previous);
    module->dispatches++;
    return true;
}

static void invalidate_page(GcnOverlayModule* module,u32 index) {
    if(module->resident[index]!=GCN_OVERLAY_UNKNOWN && module->resident[index]!=GCN_OVERLAY_NONE)
        --module->pages_resident;
    module->resident[index]=GCN_OVERLAY_UNKNOWN;
    ++module->invalidations;
}

void gcn_overlay_module_invalidate(GcnOverlayModule* module, u32 address,
                                   u32 size) {
    u32 canonical;
    u32 offset;
    if (!module || !module->page_count || !size ||
        !canonical_mem1(address, &canonical, &offset))
        return;

    const u64 end = (u64)offset + size;
    const unsigned previous=gcn_cost_enter(GCN_COST_INVALIDATE);
#if GCN_OVERLAY_INDEXED
    if(module->index) {
        const u32 pages=GC_MAIN_RAM_SIZE/4096u;
        const u32 last=end>=GC_MAIN_RAM_SIZE?pages-1u:(u32)((end-1u)/4096u);
        for(u32 page=offset/4096u;page<=last;++page) {
            for(u32 bank=0;bank<3;++bank) {
                const u32 index=module->index[bank*pages+page];
                if(index!=GCN_OVERLAY_NONE)invalidate_page(module,index);
            }
        }
        gcn_cost_leave(previous);
        return;
    }
#endif
    for (u32 i = 0; i < module->page_count; i++) {
        const GcnOverlayPage* page = &module->pages[i];
        u32 page_offset, ignored;
        if (!canonical_mem1(page->start, &ignored, &page_offset) ||
            (u64)page_offset + (page->end-page->start) <= offset || page_offset >= end)
            continue;
        invalidate_page(module,i);
    }
    gcn_cost_leave(previous);
}
