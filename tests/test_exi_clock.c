/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "exi/exi.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"EXI clock failure line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int irq_level;
static unsigned persists;
static void irq(void* user,int level){(void)user;irq_level=level;}
static void persist(void* user){(void)user;++persists;}
static void write_reg(GcnExi* exi,const GcnExiAccess* access,u32 reg,u32 value,bool wii){
    if(wii)gcn_exi_bus_write_clock(exi,access,GCN_EXI_BASE+reg,value,4,GCN_RTC_WII_CORE_CLOCK_HZ);
    else gcn_exi_bus_write(exi,access,GCN_EXI_BASE+reg,value,4);
}
static void command(GcnExi* exi,const GcnExiAccess* a,u32 cmd,bool wii){
    write_reg(exi,a,GCN_EXI_CSR_OFF,0,wii);
    write_reg(exi,a,GCN_EXI_CSR_OFF,0x104,wii); /* IPL device, TCINT mask */
    write_reg(exi,a,GCN_EXI_DATA_OFF,cmd,wii);
    write_reg(exi,a,GCN_EXI_CR_OFF,0x35,wii); /* four-byte immediate write */
}
static u32 read_rtc(GcnExi* exi,GcnExiAccess* a,u64 cycles,bool wii){
    a->core_cycles=cycles;command(exi,a,0x20000000,wii);
    write_reg(exi,a,GCN_EXI_CR_OFF,0x31,wii); /* four-byte immediate read */
    return gcn_exi_bus_read(exi,GCN_EXI_BASE+GCN_EXI_DATA_OFF,4);
}
int main(void){
    for(unsigned p=0;p<2;p++){
        const bool wii=p!=0;const u64 hz=wii?729000000ull:486000000ull;
        GcnExi exi;gcn_exi_init(&exi);gcn_exi_set_irq(&exi,irq,NULL);gcn_exi_set_persist(&exi,persist,NULL);
        GcnExiAccess a={0};gcn_rtc_start(&exi.rtc,1234,9);persists=0;irq_level=0;
        CHECK(read_rtc(&exi,&a,9+hz-1,wii)==1234);
        CHECK(read_rtc(&exi,&a,9+hz,wii)==1235);
        CHECK(irq_level && (exi.channels[0].csr&GCN_EXI_CSR_TCINT));
        write_reg(&exi,&a,GCN_EXI_CSR_OFF,0x10c,wii);
        CHECK(!irq_level && !(exi.channels[0].csr&GCN_EXI_CSR_TCINT));
        CHECK(read_rtc(&exi,&a,9+2*hz+5,wii)==1236);
        GcnExi saved=exi;
        CHECK(read_rtc(&exi,&a,9+3*hz,wii)==1237);
        exi=saved;CHECK(read_rtc(&exi,&a,9+3*hz,wii)==1237);
        a.core_cycles=9+3*hz+17;command(&exi,&a,0xa0000000,wii);
        write_reg(&exi,&a,GCN_EXI_DATA_OFF,0xfffffffeu,wii);
        write_reg(&exi,&a,GCN_EXI_CR_OFF,0x35,wii);
        CHECK(persists==1 && exi.rtc.counter==0xfffffffeu && exi.rtc.anchor_cycles==a.core_cycles);
        CHECK(read_rtc(&exi,&a,9+5*hz+17,wii)==0u);
        CHECK((wii?gcn_exi_rtc_latch_clock(&exi,9+6*hz+17,hz):gcn_exi_rtc_latch(&exi,9+6*hz+17))==1u);
        saved=exi;gcn_exi_bus_write_clock(&exi,&a,GCN_EXI_BASE,0,4,0);
        CHECK(!memcmp(&saved,&exi,sizeof exi));
        gcn_exi_set_rtc(&exi,17u);
        CHECK(read_rtc(&exi,&a,9+100*hz,wii)==17u && !exi.rtc.running);
        gcn_exi_free(&exi);
    }
    puts("EXI: legacy486 and explicit729 RTC reads/writes/wrap/snapshot/IRQ/fixed passed");
    return 0;
}
