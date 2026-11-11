/* SPDX-License-Identifier: MIT */
#include "debug/cost_ring.h"
#include <stdio.h>
#include <string.h>

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

int main(void) {
    char json[8192];
    gcn_cost_init(0);
    gcn_cost_begin_turn(0,0);
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
    CHECK(strstr(json,"\"thread\":2147487744,\"cycles\":8"));
    for(u64 field=2;field<=2052;++field) {
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
