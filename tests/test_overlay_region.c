/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/overlay_region.h"
#include "cpu/overlay_binding.h"
#include "cpu/aot_module.h"
#include "cpu/native_candidate.h"
#include "memory/memory.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static GcnOverlayRegion region;
static GcnOverlayModule* observed;
static unsigned mutate;
static bool owner_allows=true;
static bool boundary(const CPUState* cpu,u32 target,u64 cycles,void* user) {
    CHECK(cpu && target==0x80002000u && cycles==1 && user==&owner_allows);
    return owner_allows;
}
static int candidate(CPUState* cpu,u32 address) {
    return observed && gcn_overlay_module_call(observed,cpu,address);
}
static void page_a(CPUState* cpu) {++cpu->gpr[3];++cpu->cycles;cpu->pc=0x80002000u;}
static void page_b(CPUState* cpu) {++cpu->gpr[4];++cpu->cycles;cpu->pc=0x80003000u;}
static void invalidate(u32 address,u32 size) {
    if (observed) gcn_overlay_module_invalidate(observed,address,size);
    gcn_overlay_region_invalidate(&region,address,size);
}
static void combined(CPUState* cpu) {
    const GcnOverlayRegionInvocation invocation=gcn_overlay_region_invocation();
    if (cpu->pc==0x80001000u) {
        ++cpu->gpr[3]; ++cpu->cycles;
        /* Same bytes still invalidate the old execution authorization. This
         * models a store/callback while A runs, before its local BL into B. */
        if (mutate) mem_write8(cpu,mutate,0,cpu->pc);
        if (!gcn_overlay_region_cross(&invocation,cpu,0x80002000u,
                                      cpu->cycles,cpu->cycle_deadline)) {
            cpu->pc=0x80002000u; return;
        }
    }
    page_b(cpu);
}
static void reset_cpu(CPUState* cpu,u64 deadline) {
    memset(cpu->gpr,0,sizeof cpu->gpr);
    cpu->pc=0x80001000u;cpu->cycles=0;cpu->cycle_deadline=deadline;
    cpu->exception=0;cpu->halted=false;mutate=0;
}

int main(void) {
    CPUState cpu;if (!cpu_init(&cpu)) return 1;
    const GcnOverlayPage pages[]={{0x80001000u,0x80002000u,0,1},
                                  {0x80002000u,0x80003000u,1,1}};
    const u64 hash=gcn_aot_fnv1a64(cpu.ram+0x1000u,4096u);
    const GcnOverlayVariant variants[]={{hash,page_a},{hash,page_b}};
    u32 resident[]={GCN_OVERLAY_UNKNOWN,GCN_OVERLAY_UNKNOWN};
    GcnOverlayModule module={pages,variants,resident,2,2};
    const GcnOverlayFn members[]={page_a,page_b};
    region=(GcnOverlayRegion){.start=0x80001000u,.page_count=2,
                              .members=members,.body=combined};
    observed=&module;
    gcn_native_code_set_candidate_scoped(0,GC_MAIN_RAM_SIZE,candidate,invalidate);
    gcn_overlay_region_enable(&region,true);
    reset_cpu(&cpu,100u);
    CHECK(gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
    CHECK(cpu.pc==0x80003000u && cpu.gpr[3]==1 && cpu.gpr[4]==1 && cpu.cycles==2);
    CHECK(region.calls==1 && region.crossings==1 && module.hashes==2);
    CHECK(!gcn_overlay_region_invocation().region);
    reset_cpu(&cpu,100u);
    CHECK(gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
    CHECK(module.hashes==2); /* Cached tuple, no recurring content hashes. */
    reset_cpu(&cpu,100u);
    CPUState other_cpu=cpu;GcnOverlayModule other_module=module;
    CHECK(!gcn_overlay_region_call(NULL,&cpu,&region,cpu.pc));
    CHECK(!gcn_overlay_region_call(&module,&other_cpu,&region,other_cpu.pc));
    CHECK(!gcn_overlay_region_call(&other_module,&cpu,&region,cpu.pc));

    /* No cached namespace substitution or mismatched entry-PC contract. */
    for (unsigned bank=0;bank<2;++bank) {
        const u32 address=bank?0xc0001000u:0x1000u;cpu.pc=address;
        CHECK(!gcn_overlay_region_call(&module,&cpu,&region,address));
        CHECK(cpu.pc==address);
    }
    cpu.pc=0x80001004u;
    CHECK(!gcn_overlay_region_call(&module,&cpu,&region,0x80001000u));

    /* Every original deadline boundary is retained when crossing pages. */
    for (unsigned deadline=0;deadline<2;++deadline) {
        reset_cpu(&cpu,deadline);
        CHECK(gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
        CHECK(cpu.pc==0x80002000u && cpu.cycles==1 && !cpu.gpr[4]);
        CHECK(gcn_overlay_module_call(&module,&cpu,cpu.pc));
        CHECK(cpu.pc==0x80003000u && cpu.cycles==2 && cpu.gpr[4]==1);
    }

    /* A store to B via ANY execution alias must retire A's active tuple,
     * including an identical-byte write: old-token == new-bytes is unsafe. */
    const u32 aliases[]={0x2000u,0x80002000u,0xc0002000u};
    for (unsigned i=0;i<3;++i) {
        reset_cpu(&cpu,100u);mutate=aliases[i];const u64 before=region.generation;
        CHECK(gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
        CHECK(region.generation>before && !region.known && !region.valid);
        CHECK(cpu.pc==0x80002000u && cpu.cycles==1 && !cpu.gpr[4]);
        reset_cpu(&cpu,100u);
        CHECK(gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
        CHECK(cpu.pc==0x80003000u && cpu.gpr[4]==1);
    }
    /* Mismatch declines without executing; restoring bytes can re-admit. */
    mem_write8(&cpu,0x80002000u,1,cpu.pc);reset_cpu(&cpu,100u);
    CHECK(!gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
    CHECK(cpu.pc==0x80001000u && !cpu.cycles && !cpu.gpr[3]);
    mem_write8(&cpu,0x80002000u,0,cpu.pc);
    CHECK(gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
    CHECK(cpu.gpr[3]==1 && cpu.gpr[4]==1);

    GcnOverlayRegionInvocation invocation={&region,&cpu,region.generation};
    CHECK(!gcn_overlay_region_cross(&invocation,&cpu,0xc0002000u,1,100));
    CHECK(!gcn_overlay_region_cross(&invocation,&cpu,0x80002001u,1,100));
    cpu.exception=1;CHECK(!gcn_overlay_region_cross(&invocation,&cpu,0x80002000u,1,100));
    cpu.exception=0;cpu.halted=true;
    CHECK(!gcn_overlay_region_cross(&invocation,&cpu,0x80002000u,1,100));cpu.halted=false;
    CPUState other=cpu;CHECK(!gcn_overlay_region_cross(&invocation,&other,0x80002000u,1,100));
    region.boundary=boundary;region.boundary_user=&owner_allows;owner_allows=false;
    CHECK(!gcn_overlay_region_cross(&invocation,&cpu,0x80002000u,1,100));
    owner_allows=true;CHECK(gcn_overlay_region_cross(&invocation,&cpu,0x80002000u,1,100));
    gcn_overlay_region_enable(&region,false);
    CHECK(!gcn_overlay_region_cross(&invocation,&cpu,0x80002000u,1,100));
    CHECK(!gcn_overlay_region_call(&module,&cpu,&region,cpu.pc));
    /* Invalid shape cannot be opted in, including a range crossing MEM1. */
    region.start=0x817ff000u;gcn_overlay_region_enable(&region,true);CHECK(!region.enabled);
    region.start=0x40001000u;gcn_overlay_region_enable(&region,true);CHECK(!region.enabled);
    gcn_native_code_set_candidate(0,0,NULL,NULL);observed=NULL;
    cpu_free(&cpu);
    return failures?1:0;
}
