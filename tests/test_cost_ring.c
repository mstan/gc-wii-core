/* SPDX-License-Identifier: MIT */
#include "debug/cost_ring.h"
#include <stdio.h>
#include <string.h>
#if !GCN_COST_RING_ENABLED && defined(_WIN32)
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

#if !GCN_COST_RING_ENABLED
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
    gcn_cost_owner_boundary(GCN_COST_OWNER_GRAPHICS);
    CHECK(gcn_cost_boundary(GCN_COST_DSP)==GCN_COST_HOST);
    gcn_cost_begin_turn(UINT64_MAX,~0u);
    gcn_cost_thread_boundary(0,0);
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
    return failures?1:0;
}

#endif
