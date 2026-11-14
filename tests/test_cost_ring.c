/* SPDX-License-Identifier: MIT */
#include "debug/cost_ring.h"
#include <stdio.h>
#include <string.h>
#ifndef GCN_COST_TEST_DISABLED
#define GCN_COST_TEST_DISABLED (!GCN_COST_RING_ENABLED)
#endif
#if GCN_COST_TEST_DISABLED && defined(_WIN32)
#include <windows.h>
#endif

static int failures;
#define CHECK(condition) do { if(!(condition)) { \
    fprintf(stderr,"cost ring check failed at line %d\n",__LINE__);++failures; \
} } while(0)

static void read_json(char* output,size_t capacity,u32 limit) {
    FILE* file=tmpfile();
    if(!file) { ++failures;output[0]=0;return; }
    gcn_cost_write_json(file,limit);
    rewind(file);
    const size_t size=fread(output,1,capacity-1,file);
    output[size]=0;
    CHECK(!ferror(file));
    fclose(file);
}

#if GCN_COST_TEST_DISABLED
static void check_disabled(u32 limit) {
    char json[512];
    read_json(json,sizeof json,limit);
    CHECK(strstr(json,"\"enabled\":false"));
    CHECK(strstr(json,"\"clock\":\"disabled\""));
    CHECK(strstr(json,"\"clock_pair_overhead_ticks\":0"));
    CHECK(strstr(json,"\"written\":0"));
    CHECK(strstr(json,"\"fields\":[]"));
    CHECK(!gcn_cost_sampling);
}
static void invoke_disabled(void) {
    /* The first direct call exposed an uninitialized Windows clock when OFF.
     * Direct operations and unusual values must stay inert before init too. */
#if GCN_COST_THREAD_FAST
    CHECK(gcn_cost_thread_state==NULL);
#endif
    gcn_cost_owner_boundary(GCN_COST_OWNER_GRAPHICS);
    CHECK(gcn_cost_boundary(GCN_COST_DSP)==GCN_COST_HOST);
    gcn_cost_begin_turn(UINT64_MAX,~0u);
    gcn_cost_thread_boundary(0,0);
    gcn_cost_thread_boundary_fast(UINT64_MAX,~0u);
    gcn_cost_note_idle(UINT64_MAX);
    const unsigned previous=gcn_cost_enter(GCN_COST_DSP);
    CHECK(previous==GCN_COST_HOST);
    gcn_cost_leave(previous);
    gcn_cost_end_turn(0);
    gcn_cost_finish_field(UINT64_MAX,0);
}
int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX|SEM_FAILCRITICALERRORS);
#endif
    check_disabled(0);
    invoke_disabled();
    check_disabled(UINT32_MAX);
    gcn_cost_init(UINT64_MAX);
    for(unsigned i=0;i<4;++i)invoke_disabled();
    check_disabled(1);
    gcn_cost_init(0);
    check_disabled(UINT32_MAX);
    return failures?1:0;
}
#else
static void note_thread(bool fast,u64 cycles,u32 thread) {
    if(fast)gcn_cost_thread_boundary_fast(cycles,thread);
    else gcn_cost_thread_boundary(cycles,thread);
}
static void ledger_part(const char* json,unsigned field,char* output,size_t size) {
    char key[64];snprintf(key,sizeof key,"\"field\":%u,",field);
    const char* row=strstr(json,key);
    const char* first=row?strstr(row,"\"no_thread_cycles\":"):NULL;
    const char* last=first?strstr(first,"]}"):NULL;
    CHECK(first && last);
    if(!first || !last) { output[0]=0;return; }
    const size_t length=(size_t)(last-first)+2;
    CHECK(length<size);
    if(length>=size) { output[0]=0;return; }
    memcpy(output,first,length);output[length]=0;
}
static void thread_ledger(bool fast,char ledger[3][2048]) {
    const u32 a=0x80001000u,b=0x80002000u;
    gcn_cost_init(0);gcn_cost_begin_turn(0,0);
    for(u64 i=1;i<=10000;++i)note_thread(fast,i*3,0);
    note_thread(fast,30007,a);
    for(u64 i=30008;i<=50000;++i)note_thread(fast,i,a);
    note_thread(fast,50011,b);note_thread(fast,50016,a);
    gcn_cost_end_turn(50019);gcn_cost_finish_field(1,50019);
    gcn_cost_begin_turn(50019,a);
    for(u64 i=50020;i<=60019;++i)note_thread(fast,i,a);
    note_thread(fast,60023,0);
    for(u64 i=60024;i<=61023;++i)note_thread(fast,i,0);
    gcn_cost_end_turn(61029);gcn_cost_begin_turn(61029,b);
    /* Real zero-length transitions do not create ledger entries. */
    note_thread(fast,61029,a);note_thread(fast,61029,b);
    for(u64 i=61030;i<=62029;++i)note_thread(fast,i,b);
    gcn_cost_end_turn(62031);gcn_cost_finish_field(2,62031);
    u64 cycles=62031;gcn_cost_begin_turn(cycles,a);
    for(unsigned i=0;i<18;++i) { cycles+=i+1;note_thread(fast,cycles,a+(i+1)*32); }
    cycles+=19;gcn_cost_end_turn(cycles);gcn_cost_begin_turn(cycles,a);
    for(unsigned i=0;i<10000;++i)note_thread(fast,++cycles,a);
    cycles+=3;gcn_cost_note_idle(123);gcn_cost_end_turn(cycles);gcn_cost_finish_field(3,cycles);
    char json[32768];read_json(json,sizeof json,3);
    CHECK(strstr(json,"\"field\":1,\"guest_cycles\":50019"));
    CHECK(strstr(json,"\"field\":2,\"guest_cycles\":12012"));
    CHECK(strstr(json,"\"field\":3,\"guest_cycles\":10193"));
    for(unsigned field=1;field<=3;++field)ledger_part(json,field,ledger[field-1],2048);
    CHECK(strstr(ledger[0],"\"no_thread_cycles\":30007,\"other_thread_cycles\":0,\"idle_skipped_cycles\":0"));
    CHECK(strstr(ledger[0],"\"thread\":2147487744,\"cycles\":20007"));
    CHECK(strstr(ledger[0],"\"thread\":2147491840,\"cycles\":5"));
    CHECK(strstr(ledger[1],"\"no_thread_cycles\":1006,\"other_thread_cycles\":0,\"idle_skipped_cycles\":0"));
    CHECK(strstr(ledger[1],"\"thread\":2147487744,\"cycles\":10004"));
    CHECK(strstr(ledger[1],"\"thread\":2147491840,\"cycles\":1002"));
    CHECK(strstr(ledger[2],"\"no_thread_cycles\":0,\"other_thread_cycles\":54,\"idle_skipped_cycles\":123"));
    for(unsigned i=0;i<16;++i) {
        char expected[96];snprintf(expected,sizeof expected,"\"thread\":%u,\"cycles\":%u",a+i*32,i?i+1:10004);
        CHECK(strstr(ledger[2],expected));
    }
}
static void abandoned_turn_ledger(bool fast,char ledger[2048]) {
    const u32 a=0x80001000u,b=0x80002000u;
    gcn_cost_init(0);gcn_cost_begin_turn(0,a);
    note_thread(fast,10,a);note_thread(fast,20,a);
    /* discover.c bootstrap_launch -> continue skips end_turn. Devices and
     * launch reach cycle32, then the next begin_turn replaces the origin.
     * Raw boundaries retain A20; charging through32 would also be wrong. */
    gcn_cost_begin_turn(32,b);note_thread(fast,40,b);
    gcn_cost_end_turn(45);gcn_cost_finish_field(1,45);
    char json[8192];read_json(json,sizeof json,1);
    CHECK(strstr(json,"\"field\":1,\"guest_cycles\":45"));
    ledger_part(json,1,ledger,2048);
    CHECK(strstr(ledger,"\"thread\":2147491840,\"cycles\":13"));
    fprintf(stderr,"abandoned-turn %s: %s\n",fast?"fast":"raw",ledger);
}
static void capture_ledger(unsigned field,char ledger[2048]) {
    char json[16384];read_json(json,sizeof json,3);
    ledger_part(json,field,ledger,2048);
}
static void protocol_ledger(bool fast,char ledger[5][2048]) {
    const u32 a=0x80001000u,b=0x80002000u;
    gcn_cost_init(0);gcn_cost_begin_turn(0,a);
    note_thread(fast,10,a);note_thread(fast,20,a);
    gcn_cost_thread_boundary(25,a);note_thread(fast,30,a);
    gcn_cost_thread_boundary(35,b);note_thread(fast,38,b);note_thread(fast,40,b);
    gcn_cost_end_turn(45);gcn_cost_finish_field(1,45);
    gcn_cost_begin_turn(45,a);note_thread(fast,50,a);note_thread(fast,55,a);
    /* finish_field need not follow end_turn: only observed boundaries count. */
    gcn_cost_finish_field(2,60);
    note_thread(fast,62,a);gcn_cost_thread_boundary(65,b);note_thread(fast,70,b);
    gcn_cost_begin_turn(75,a);note_thread(fast,80,a);
    gcn_cost_end_turn(85);gcn_cost_finish_field(3,85);
    for(unsigned field=1;field<=3;++field)capture_ledger(field,ledger[field-1]);
    CHECK(strstr(ledger[0],"\"thread\":2147487744,\"cycles\":35"));
    CHECK(strstr(ledger[0],"\"thread\":2147491840,\"cycles\":10"));
    CHECK(strstr(ledger[1],"\"thread\":2147487744,\"cycles\":10"));
    CHECK(strstr(ledger[2],"\"thread\":2147487744,\"cycles\":20"));
    CHECK(strstr(ledger[2],"\"thread\":2147491840,\"cycles\":5"));
    gcn_cost_init(100);gcn_cost_begin_turn(100,a);
    note_thread(fast,110,a);note_thread(fast,120,a);
    /* init clears totals, but the existing API preserves the last origin. */
    gcn_cost_init(500);gcn_cost_end_turn(125);gcn_cost_finish_field(1,505);
    capture_ledger(1,ledger[3]);
    CHECK(strstr(ledger[3],"\"thread\":2147487744,\"cycles\":5"));
    gcn_cost_init(0);gcn_cost_begin_turn(0,a);
    note_thread(fast,UINT64_MAX,a);note_thread(fast,0,a);
    gcn_cost_thread_boundary(0,b);note_thread(fast,4,b);note_thread(fast,2,b);
    gcn_cost_end_turn(7);gcn_cost_finish_field(1,7);capture_ledger(1,ledger[4]);
    /* A wrapped total still occupies its original slot; it is not omitted. */
    CHECK(strstr(ledger[4],"\"thread\":2147487744,\"cycles\":0"));
    CHECK(strstr(ledger[4],"\"thread\":2147491840,\"cycles\":7"));
}
static void check_pending_path(void) {
#if GCN_COST_THREAD_FAST
    /* A mixed FAST header/raw provider legitimately falls back to raw. */
    if(!gcn_cost_thread_state)return;
    GcnCostThreadState* s=gcn_cost_thread_state;
    gcn_cost_init(0);gcn_cost_begin_turn(0,1);
    gcn_cost_thread_boundary_fast(10,1);
    CHECK(s->pending && s->cycles==0 && s->pending_cycles==10);
    gcn_cost_thread_boundary(15,2);
    CHECK(!s->pending && s->cycles==15 && s->thread==2);
    gcn_cost_thread_boundary_fast(20,2);gcn_cost_end_turn(25);
    CHECK(!s->pending && s->cycles==20 && s->thread==2);
#endif
}
int main(void) {
    char json[8192];
    gcn_cost_init(0);
    gcn_cost_begin_turn(0,0);
    gcn_cost_note_idle(3);
    const unsigned previous=gcn_cost_enter(GCN_COST_GUEST);
    gcn_cost_leave(previous);
    gcn_cost_end_turn(8);
    gcn_cost_begin_turn(8,0x80001000);
    gcn_cost_end_turn(16);
    gcn_cost_finish_field(1,16);
    read_json(json,sizeof json,1);
    CHECK(strstr(json,"\"guest_cycles\":16"));
    CHECK(strstr(json,"\"turns\":2,\"sampled_turns\":1"));
    CHECK(strstr(json,"\"no_thread_cycles\":8"));
    CHECK(strstr(json,"\"idle_skipped_cycles\":3"));
    CHECK(strstr(json,"\"thread\":2147487744,\"cycles\":8"));
    gcn_cost_begin_turn(16,0);
    gcn_cost_thread_boundary(20,0x80001000);
    gcn_cost_thread_boundary(22,0x80002000);
    gcn_cost_end_turn(24);
    gcn_cost_finish_field(2,24);
    read_json(json,sizeof json,1);
    CHECK(strstr(json,"\"no_thread_cycles\":4"));
    CHECK(strstr(json,"\"idle_skipped_cycles\":0"));
    CHECK(strstr(json,"\"thread\":2147487744,\"cycles\":2"));
    CHECK(strstr(json,"\"thread\":2147491840,\"cycles\":2"));
    for(u64 field=3;field<=2052;++field) {
        gcn_cost_begin_turn(field*16-16,0);
        gcn_cost_end_turn(field*16);
        gcn_cost_finish_field(field,field*16);
    }
    read_json(json,sizeof json,3);
    CHECK(strstr(json,"\"written\":2052"));
    CHECK(!strstr(json,"\"field\":2049,"));
    CHECK(strstr(json,"\"field\":2050,"));
    CHECK(strstr(json,"\"field\":2052,"));
    char raw[3][2048],fast[3][2048];
    thread_ledger(false,raw);thread_ledger(true,fast);
    for(unsigned i=0;i<3;++i)CHECK(!strcmp(raw[i],fast[i]));
    char raw_abandoned[2048],fast_abandoned[2048];
    abandoned_turn_ledger(false,raw_abandoned);
    abandoned_turn_ledger(true,fast_abandoned);
    CHECK(strstr(raw_abandoned,"\"thread\":2147487744,\"cycles\":20"));
    CHECK(!strcmp(raw_abandoned,fast_abandoned));
    char raw_protocol[5][2048],fast_protocol[5][2048];
    protocol_ledger(false,raw_protocol);protocol_ledger(true,fast_protocol);
    for(unsigned i=0;i<5;++i)CHECK(!strcmp(raw_protocol[i],fast_protocol[i]));
    check_pending_path();
    return failures?1:0;
}

#endif
