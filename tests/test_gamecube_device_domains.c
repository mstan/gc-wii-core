/* SPDX-License-Identifier: MIT */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gcnllr/device_domains.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    GcnDeviceDomains d;
    uint64_t elapsed[4],queries[4],period[4];
    uint32_t order[64],count;
    uint32_t fail_domain,reenter_domain;
    bool video_dependencies;
} Fixture;
static unsigned index_of(uint32_t bit) {
    for(unsigned i=0;i<4;i++)if(bit==(1u<<i))return i;
    assert(false);return 0;
}
static uint64_t deadline(void* user,uint32_t bit,uint64_t cursor) {
    Fixture* f=user;const unsigned i=index_of(bit);++f->queries[i];
    /* Next real event is independent of observation/callback frequency. */
    return f->period[i] ? f->period[i]-cursor%f->period[i]:UINT64_MAX;
}
static bool advance(void* user,uint32_t bit,uint64_t elapsed,uint64_t target) {
    Fixture* f=user;const unsigned i=index_of(bit);
    if(f->reenter_domain==bit)return gcn_domains_observe(&f->d,bit,target);
    if(f->video_dependencies && bit==GCN_DOMAIN_VIDEO)
        if(!gcn_domains_observe(&f->d,GCN_DOMAIN_AUDIO|GCN_DOMAIN_GPU,target))return false;
    f->elapsed[i]+=elapsed;
    assert(f->count<64);f->order[f->count++]=bit;
    return bit!=f->fail_domain;
}
static void init(Fixture* f) {
    memset(f,0,sizeof(*f));
    f->period[0]=10;f->period[1]=12;f->period[2]=100;f->period[3]=30;
    assert(gcn_domains_init(&f->d,0,advance,deadline,f));
}
int main(void) {
    Fixture f;init(&f);
    assert(gcn_domains_deadline(&f.d,0)==10);
    for(unsigned n=1;n<10;n++)assert(gcn_domains_credit(&f.d,n));
    for(unsigned i=0;i<4;i++)assert(f.queries[i]==1 && !f.elapsed[i]);
    assert(gcn_domains_credit(&f.d,10));assert(f.elapsed[0]==10 && !f.elapsed[1]);
    assert(gcn_domains_observe(&f.d,GCN_DOMAIN_GPU,11));
    assert(f.elapsed[1]==11 && !f.elapsed[2] && !f.elapsed[3]);
    assert(gcn_domains_deadline(&f.d,11)==1); /* lag-adjusted GPU event */
    assert(gcn_domains_credit(&f.d,12));assert(f.elapsed[1]==12);
    /* Register mutation revokes the cached deadline, without forcing other
     * domains or querying every device at every generated CPU boundary. */
    f.period[2]=13;gcn_domains_changed(&f.d,GCN_DOMAIN_STORAGE);
    assert(gcn_domains_deadline(&f.d,12)==1);
    assert(gcn_domains_credit(&f.d,13));assert(f.elapsed[2]==13);
    assert(gcn_domains_observe(&f.d,GCN_DOMAIN_ALL,15));
    for(unsigned i=0;i<4;i++)assert(f.elapsed[i]==15);

    init(&f);f.video_dependencies=true;
    assert(gcn_domains_observe(&f.d,GCN_DOMAIN_VIDEO,7));
    assert(f.count==3 && f.order[0]==GCN_DOMAIN_AUDIO &&
           f.order[1]==GCN_DOMAIN_GPU && f.order[2]==GCN_DOMAIN_VIDEO);
    assert(f.elapsed[0]==7 && f.elapsed[1]==7 && !f.elapsed[2] && f.elapsed[3]==7);
    assert(gcn_domains_observe(&f.d,GCN_DOMAIN_VIDEO,7));assert(f.count==3);
    assert(gcn_domains_observe(&f.d,GCN_DOMAIN_ALL,30));
    for(unsigned i=0;i<4;i++)assert(f.elapsed[i]==30);

    init(&f);f.fail_domain=GCN_DOMAIN_GPU;
    assert(!gcn_domains_observe(&f.d,GCN_DOMAIN_ALL,20));
    assert(f.elapsed[0]==20 && f.elapsed[1]==20 && !f.elapsed[2]);
    const uint64_t count=f.d.services[0];
    assert(!gcn_domains_credit(&f.d,21) && f.d.services[0]==count && f.d.failures==1);
    init(&f);f.reenter_domain=GCN_DOMAIN_AUDIO;
    assert(!gcn_domains_observe(&f.d,GCN_DOMAIN_AUDIO,3) && f.d.failed);
    init(&f);assert(gcn_domains_credit(&f.d,3));
    assert(!gcn_domains_credit(&f.d,2) && f.d.failed);
    init(&f);gcn_domains_changed(&f.d,16);assert(f.d.failed);
    puts("independent device domains: earned time, cached deadlines, dependencies and terminal failures pass");
    return 0;
}
