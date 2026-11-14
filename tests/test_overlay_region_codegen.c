/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/overlay_region.h"
#include "cpu/overlay_binding.h"
#include "cpu/aot_module.h"
#include "cpu/native_candidate.h"
#include "memory/memory.h"
#include "overlay_regions.inc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_REGION_BASE
#define TEST_REGION_BASE 0x80001000u
#endif
#define BASE ((u32)TEST_REGION_BASE)
static unsigned failures, checks, mode, callbacks;
static GcnOverlayModule* observed;
static GcnOverlayRegion* active_region;
static u32 mutation;

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures; }} while(0)

static int candidate(CPUState* cpu,u32 pc) {
    return observed && gcn_overlay_module_call(observed,cpu,pc);
}
static void invalidate(u32 address,u32 size) {
    if (observed) gcn_overlay_module_invalidate(observed,address,size);
    if (active_region) gcn_overlay_region_invalidate(active_region,address,size);
}
static void external(CPUState* cpu,u32 address,u64 value,u8 size) {
    (void)address;(void)value;(void)size;
    ++callbacks;
    /* Architectural state mutations must survive both emitted paths. */
    cpu->gpr[11]^=0x13579bdfu;cpu->cr^=0xa55aa55au;
    cpu->xer^=0x20000000u;cpu->fpr[13]+=0.5;
    if (mode==1 || mode==2) {
        const u32 alias=(mutation&0x1fffffffu) | (BASE&0xe0000000u);
        u8 byte=mem_read8(cpu,alias);
        mem_write8(cpu,alias,mode==2?(u8)(byte^1u):byte,cpu->pc);
    } else if (mode==3) cpu->halted=true;
    else if (mode==4) cpu->exception=0x500u;
}

typedef struct { CPUState cpu; u8* ram; bool missing; unsigned callback_count; u64 declines,crossings; } Result;

static Result execute(const u8* image,u32 entry,u64 deadline,unsigned kind,u32 changed,bool region_path) {
    Result r={0};CPUState live;CHECK(cpu_init(&live));
    if(!live.ram)return r;
    memcpy(live.ram+0x1000u,image,8192u);
    for(unsigned n=0;n<32;++n) {
        live.gpr[n]=n*0x10203u;live.fpr[n]=(double)n+0.25;live.ps1[n]=(double)n-0.5;
    }
    live.pc=BASE+entry;live.lr=BASE+0x2000u;live.gpr[9]=0xcc000000u;
    live.msr=0x2000u;live.cycles=17;live.cycle_deadline=deadline;
    live.external_write=external;
    const GcnOverlayPage pages[]={{BASE,BASE+4096u,0,1},{BASE+4096u,BASE+8192u,1,1}};
    const GcnOverlayVariant variants[]={
        {gcn_aot_fnv1a64(image,4096u),native_region_members_0[0]},
        {gcn_aot_fnv1a64(image+4096u,4096u),native_region_members_0[1]}};
    u32 resident[]={GCN_OVERLAY_UNKNOWN,GCN_OVERLAY_UNKNOWN};
    GcnOverlayModule module={pages,variants,resident,2,2};
    observed=&module;active_region=region_path?&native_regions[0]:NULL;
    callbacks=0;mode=kind;mutation=changed;
    if (active_region) {
        active_region->calls=active_region->crossings=active_region->declined_crossings=0;
        gcn_overlay_region_enable(active_region,true);
    }
    gcn_native_code_set_candidate_scoped(0,GC_MAIN_RAM_SIZE,candidate,invalidate);
    unsigned calls=0;
    while (!live.halted && !live.exception && live.pc>=BASE && live.pc<BASE+8192u && calls++<100u) {
        if (region_path && gcn_overlay_region_call(&module,&live,active_region,live.pc)) continue;
        if (!gcn_overlay_module_call(&module,&live,live.pc)) {r.missing=true;break;}
    }
    CHECK(calls<100u);
    r.callback_count=callbacks;
    if (active_region) {r.declines=active_region->declined_crossings;r.crossings=active_region->crossings;}
    gcn_native_code_set_candidate(0,0,NULL,NULL);observed=NULL;active_region=NULL;
    r.cpu=live;r.ram=(u8*)malloc(live.ram_size);CHECK(r.ram!=NULL);
    if(r.ram)memcpy(r.ram,live.ram,live.ram_size);
    r.cpu.ram=NULL;r.cpu.mem2=NULL;
    cpu_free(&live); /* The runtime's locked-L1 owner must not outlive this run. */
    return r;
}

static void compare(const u8* image,u32 entry,u64 deadline,unsigned kind,u32 changed) {
    Result a=execute(image,entry,deadline,kind,changed,false);
    Result b=execute(image,entry,deadline,kind,changed,true);
    CPUState ca=a.cpu,cb=b.cpu;
    ca.ram=cb.ram=NULL;ca.mem2=cb.mem2=NULL;
    CHECK(a.missing==b.missing);CHECK(a.callback_count==b.callback_count);
    if (memcmp(&ca,&cb,sizeof ca)) {
        fprintf(stderr,"CPU mismatch base=%08x entry=%x deadline=%llu mode=%u PCs=%08x/%08x cycles=%llu/%llu\n",
            BASE,entry,(unsigned long long)deadline,kind,ca.pc,cb.pc,
            (unsigned long long)ca.cycles,(unsigned long long)cb.cycles);
        const u8* x=(const u8*)&ca;const u8* y=(const u8*)&cb;
        for(size_t n=0;n<sizeof ca;++n)if(x[n]!=y[n])fprintf(stderr," byte %zu: %02x/%02x\n",n,x[n],y[n]);
        CHECK(false);
    } else CHECK(true);
    CHECK(a.ram && b.ram && !memcmp(a.ram,b.ram,a.cpu.ram_size));
    if (kind==1 || kind==2) CHECK(b.declines>0);
    free(a.ram);free(b.ram);
}

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    FILE* f=fopen(argv[1],"rb");if(!f)return 2;
    u8 image[8192];const size_t count=fread(image,1,sizeof image,f);fclose(f);if(count!=sizeof image)return 2;
    const u32 entries[]={0,4,0x10,0x14,0x18,0x40,0x44,0x48,0x80,0x84,0x100,0x104,0xff8,0xffc,0x1000,0x1004,0x1020,0x1024,0x1060,0x1064,0x1080,0x1088};
    const u64 deadlines[]={0,17,18,19,20,21,23,100000};
    for(unsigned e=0;e<sizeof entries/sizeof *entries;++e)
        for(unsigned d=0;d<sizeof deadlines/sizeof *deadlines;++d)
            compare(image,entries[e],deadlines[d],0,0);
    /* Active A -> B invalidation, sequential and BL; B -> A shadow return.
     * Identical bytes must retire the current invocation as well. */
    const u32 from[]={0x40,0xff8,0x80};
    const u32 target[]={BASE+0x1060,BASE+0x1000,BASE+0x84};
    for(unsigned n=0;n<3;++n) {
        compare(image,from[n],100000,1,target[n]);
        compare(image,from[n],100000,2,target[n]);
        compare(image,from[n],100000,3,0);
        compare(image,from[n],100000,4,0);
    }
    printf("region emitted differential base=%08x checks=%u failures=%u\n",BASE,checks,failures);
    return failures?1:0;
}
