/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/overlay_region.h"
#include "cpu/overlay_binding.h"

#if defined(_MSC_VER)
static __declspec(thread) GcnOverlayRegionInvocation current;
#else
static _Thread_local GcnOverlayRegionInvocation current;
#endif

static bool shape(const GcnOverlayRegion* region) {
    if (!region || !region->members || !region->body || region->page_count<2u ||
        region->page_count>GC_MAIN_RAM_SIZE/4096u || (region->start&4095u)) return false;
    const u32 physical=region->start&0x1fffffffu;
    const u32 bank=region->start>>29u;
    return (bank==0u || bank==4u || bank==6u) &&
        (u64)physical+(u64)region->page_count*4096u<=GC_MAIN_RAM_SIZE;
}

void gcn_overlay_region_enable(GcnOverlayRegion* region,bool enabled) {
    if (!region) return;
    ++region->generation;
    region->known=false; region->valid=false; region->cpu=NULL; region->module=NULL;
    region->enabled=enabled && shape(region);
}

void gcn_overlay_region_invalidate(GcnOverlayRegion* region,u32 address,u32 size) {
    if (!region || !size || !shape(region)) return;
    const u32 bank=address>>29u;
    if (bank!=0u && bank!=4u && bank!=6u) return;
    const u32 physical=address&0x1fffffffu;
    const u32 start=region->start&0x1fffffffu;
    const u64 end=(u64)start+(u64)region->page_count*4096u;
    if (physical>=end || (u64)physical+size<=start) return;
    ++region->generation;
    region->known=false; region->valid=false; region->cpu=NULL; region->module=NULL;
}

GcnOverlayRegionInvocation gcn_overlay_region_invocation(void) { return current; }

bool gcn_overlay_region_call(GcnOverlayModule* module,CPUState* cpu,
                             GcnOverlayRegion* region,u32 address) {
    if (!module || !shape(region) || !region->enabled || !cpu || !cpu->ram ||
        cpu->pc!=address ||
        (address&3u) || address<region->start ||
        (u64)address>=(u64)region->start+(u64)region->page_count*4096u) return false;
    if (region->known && (region->cpu!=cpu || region->module!=module)) return false;
    if (!region->known) {
        region->cpu=cpu;
        region->module=module;
        region->valid=true;
        /* Resolve all members even after a mismatch: negative identities must
         * also be watched so restoring original content can admit this tuple. */
        for (u32 i=0;i<region->page_count;++i) {
            GcnOverlayFn body=gcn_overlay_module_bind(module,cpu,region->start+i*4096u);
            if (!region->members[i] || body!=region->members[i]) region->valid=false;
        }
        region->known=true;
    }
    if (!region->valid) return false;
    const GcnOverlayRegionInvocation previous=current;
    current=(GcnOverlayRegionInvocation){region,cpu,region->generation};
    ++region->calls;
    const bool called=gcn_overlay_binding_call(module,cpu,region->body);
    current=previous;
    return called;
}
