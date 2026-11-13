/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/overlay_module.h"
#include "cpu/aot_module.h"
#include "cpu/native_candidate.h"
#include <stdio.h>
static int failures;
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);failures++;}}while(0)
static void body(CPUState* cpu){cpu->lr=cpu->pc+4;cpu->pc+=4;}
static GcnOverlayModule* observed_module;
static unsigned observer_calls;
static void invalidate(u32 address,u32 size) {
    ++observer_calls;
    if(observed_module)gcn_overlay_module_invalidate(observed_module,address,size);
}
static int candidate(CPUState* cpu,u32 pc) {
    return observed_module && gcn_overlay_module_call(observed_module,cpu,pc);
}
static void scoped_watches(CPUState* cpu) {
    const GcnOverlayPage page={0x80001000,0x80002000,0,1};
    cpu->ram[0x1000]=0;
    GcnOverlayVariant variant={gcn_aot_fnv1a64(cpu->ram+0x1000,4096),body};
    u32 resident=GCN_OVERLAY_UNKNOWN;
    GcnOverlayModule module={&page,&variant,&resident,1,1};
    observed_module=&module;observer_calls=0;
    gcn_native_code_reset();
    gcn_native_code_set_candidate_scoped(0,GC_MAIN_RAM_SIZE,candidate,invalidate);
    CHECK(observer_calls==1 && !(gcn_native_store_pages[1]&GCN_NATIVE_STORE_OBSERVED));
    cpu->ram[0x1000]=1;cpu->pc=0x80001008;
    CHECK(!gcn_overlay_module_call(&module,cpu,cpu->pc));
    CHECK(gcn_native_store_pages[1]&GCN_NATIVE_STORE_OBSERVED);
    cpu->ram[0x1000]=0;gcn_native_code_note_store(0x80001000,1);
    cpu->pc=0x80001008;CHECK(gcn_overlay_module_call(&module,cpu,cpu->pc));
    CHECK(gcn_native_store_pages[1]&GCN_NATIVE_STORE_OBSERVED);

    /* Ordinary data records its first write, then skips redundant work only
     * while CRC dirtiness remains recorded. Consumption rearms the fence. */
    unsigned before=observer_calls;
    cpu->ram[0x3000]=1;gcn_native_code_note_store(0xc0003000,1);
    CHECK(observer_calls==before+1 && gcn_native_store_pages[3]==GCN_NATIVE_STORE_DIRTY);
    cpu->ram[0x3001]=2;gcn_native_code_note_store(0x3001,1);
    CHECK(observer_calls==before+1);
    CHECK(gcn_native_code_page_content_stale_take(0x80003000));
    cpu->ram[0x3002]=3;gcn_native_code_note_store(0x80003002,1);
    CHECK(observer_calls==before+2);

    /* Bound code stores invalidate in every alias. A negative cache also
     * remains watched and must recover when original bytes are restored. */
    cpu->ram[0x1000]=1;gcn_native_code_note_store(0xc0001000,1);
    cpu->pc=0x80001008;CHECK(!gcn_overlay_module_call(&module,cpu,cpu->pc));
    CHECK(gcn_native_store_pages[1]&GCN_NATIVE_STORE_OBSERVED);
    cpu->ram[0x1000]=0;gcn_native_code_note_store(0x1000,1);
    cpu->pc=0x80001008;CHECK(gcn_overlay_module_call(&module,cpu,cpu->pc));

    /* dcbz/GPU content dirtiness does not replace the icache authorization.
     * An explicit DMA/ICBI invalidation still retires that native decision. */
    before=observer_calls;cpu->ram[0x1000]=1;
    gcn_native_code_content_dirty(0x80001000,1);
    CHECK(observer_calls==before);
    cpu->pc=0x80001008;CHECK(gcn_overlay_module_call(&module,cpu,cpu->pc));
    gcn_native_code_invalidate(0x1000,32);
    cpu->pc=0x80001008;CHECK(!gcn_overlay_module_call(&module,cpu,cpu->pc));
    cpu->ram[0x1000]=0;gcn_native_code_invalidate(0xc0001000,32);

    gcn_native_code_reset();
    CHECK(!(gcn_native_store_pages[1]&GCN_NATIVE_STORE_OBSERVED));
    CHECK(!(gcn_native_store_pages[3]&GCN_NATIVE_STORE_OBSERVED));
    cpu->pc=0x80001008;CHECK(gcn_overlay_module_call(&module,cpu,cpu->pc));
    gcn_native_code_watch(0x80005000,32); /* An independent/REL watch survives. */
    gcn_native_code_set_candidate_scoped(0,GC_MAIN_RAM_SIZE,candidate,invalidate);
    CHECK(gcn_native_store_pages[5]&GCN_NATIVE_STORE_OBSERVED);

    gcn_native_code_set_candidate(0,GC_MAIN_RAM_SIZE,candidate,invalidate);
    CHECK(gcn_native_store_pages[3]&GCN_NATIVE_STORE_OBSERVED);
    gcn_native_code_reset();
    CHECK(gcn_native_store_pages[3]&GCN_NATIVE_STORE_OBSERVED);
    gcn_native_code_set_candidate(0,0,NULL,NULL);gcn_native_code_reset();observed_module=NULL;
}
int main(void){
    CPUState cpu; if(!cpu_init(&cpu))return 1;
    const GcnOverlayPage pages[]={{0x1000,0x2000,0,1},{0x80001000,0x80002000,0,1},{0xc0001000,0xc0002000,0,1}};
    GcnOverlayVariant variant={gcn_aot_fnv1a64(cpu.ram+0x1000,4096),body};
    u32 resident[]={GCN_OVERLAY_UNKNOWN,GCN_OVERLAY_UNKNOWN,GCN_OVERLAY_UNKNOWN};
    GcnOverlayModule module={pages,&variant,resident,3,1};
    static u32 index[GCN_OVERLAY_INDEX_SLOTS];
    CHECK(!gcn_overlay_module_index(&module,index,GCN_OVERLAY_INDEX_SLOTS-1));
    CHECK(gcn_overlay_module_index(&module,index,GCN_OVERLAY_INDEX_SLOTS));
    for(unsigned i=0;i<3;i++){
        cpu.pc=pages[i].start+8;
        CHECK(gcn_overlay_module_call(&module,&cpu,cpu.pc));
        CHECK(cpu.pc==pages[i].start+12 && cpu.lr==pages[i].start+12);
    }
    CHECK(module.pages_resident==3 && module.hashes==3);
    cpu.ram[0x1000]=1;
    gcn_overlay_module_invalidate(&module,0xc0001000,32);
    CHECK(module.pages_resident==0);
    CHECK(module.invalidations==3);
    for(unsigned i=0;i<3;i++){
        cpu.pc=pages[i].start+8;
        CHECK(!gcn_overlay_module_call(&module,&cpu,cpu.pc));
        CHECK(cpu.pc==pages[i].start+8);
    }
    /* A cached-only table cannot substitute its body for a physical PC. */
    GcnOverlayModule cached={pages+1,&variant,resident+1,1,1};
    CHECK(gcn_overlay_module_index(&cached,index,GCN_OVERLAY_INDEX_SLOTS));
    resident[1]=GCN_OVERLAY_UNKNOWN;cpu.ram[0x1000]=0;cpu.pc=0x1008;
    CHECK(!gcn_overlay_module_call(&cached,&cpu,cpu.pc));
    CHECK(cpu.pc==0x1008);
    scoped_watches(&cpu);
    cpu_free(&cpu);return failures?1:0;
}
