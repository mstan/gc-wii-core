/* SPDX-License-Identifier: MIT */
#include "gcnllr/device_domains.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t queried,advanced;
    uint64_t elapsed[GCN_DOMAIN_COUNT];
} State;

static uint64_t deadline(void* user,uint32_t domain,uint64_t cursor) {
    (void)cursor;
    ((State*)user)->queried|=domain;
    return domain==GCN_DOMAIN_TIME?10:100;
}

static bool advance(void* user,uint32_t domain,uint64_t elapsed,uint64_t target) {
    (void)target;
    State* state=user;
    state->advanced|=domain;
    for(unsigned i=0;i<GCN_DOMAIN_COUNT;++i)
        if(domain==(1u<<i))state->elapsed[i]+=elapsed;
    return true;
}

int main(void) {
    State state={0};
    GcnDeviceDomains domains;
    if(!gcn_domains_init(&domains,0,advance,deadline,&state) ||
       state.queried!=GCN_DOMAIN_ALL || gcn_domains_deadline(&domains,0)!=100)
        return 1;
    if(gcn_domains_observe(&domains,GCN_DOMAIN_TIME,0) || !domains.failed)
        return 1;
    memset(&state,0,sizeof state);
    if(!gcn_domains_init_mask(&domains,0,GCN_DOMAIN_WII_ALL,advance,deadline,&state) ||
       state.queried!=GCN_DOMAIN_WII_ALL || gcn_domains_deadline(&domains,0)!=10)
        return 1;
    if(!gcn_domains_credit(&domains,10) || state.advanced!=GCN_DOMAIN_TIME ||
       state.elapsed[4]!=10 || domains.services[4]!=1)
        return 1;
    if(!gcn_domains_observe(&domains,GCN_DOMAIN_IO,25) || state.elapsed[2]!=25)
        return 1;
    if(!gcn_domains_credit(&domains,25) || state.elapsed[4]!=25 ||
       gcn_domains_deadline(&domains,25)!=10)
        return 1;
    puts("Shared domains preserve the GameCube mask and service Wii TIME explicitly");
    return 0;
}
