/* SPDX-License-Identifier: MIT */
#include "gcnllr/resident_memory.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {MEMORY=4096,TOKENS=256,PAGES=128};
typedef struct {
    GcnResidentMemory* manager;
    uint8_t ram[MEMORY],pages[PAGES+1];
    unsigned attempts[TOKENS],released[TOKENS],release_order[TOKENS],release_count;
    uint64_t fail_token;
    bool probe_reentrancy,check_replacement;
    uint32_t replacement_offset;
} Fixture;
static uint8_t value(uint64_t token,uint32_t row,uint32_t column) {
    return (uint8_t)(token*13+row*17+column);
}
static bool materialize(void* user,const GcnResidentCopy* copy) {
    Fixture* f=user;const GcnResidentRows* r=&copy->destination;
    assert(copy->token<TOKENS && copy->generation);
    ++f->attempts[copy->token];
    assert(gcn_resident_memory_pending(f->manager,r->offset,1));
    GcnResidentCopy found;
    assert(gcn_resident_memory_find(f->manager,r->offset,&found));
    assert(found.token==copy->token && found.generation==copy->generation);
    if(f->probe_reentrancy) {
        const GcnResidentRows incoming={3000,4,1,4};uint64_t generation=999;
        assert(!gcn_resident_memory_publish(f->manager,255,&incoming,&generation) && generation==999);
        assert(gcn_resident_memory_error(f->manager)==GCN_RESIDENT_BUSY);
        assert(!gcn_resident_memory_prepare_read(f->manager,0,1));
        assert(!gcn_resident_memory_prepare_write(f->manager,0,1,false));
        assert(!gcn_resident_memory_flush(f->manager));
        assert(!gcn_resident_memory_destroy(f->manager));
    }
    if(copy->token==f->fail_token) {
        f->ram[r->offset]=value(copy->token,0,0); /* An honest partial failure. */
        return false;
    }
    for(uint32_t row=0;row<r->rows;row++)for(uint32_t x=0;x<r->row_bytes;x++)
        f->ram[r->offset+row*r->stride+x]=value(copy->token,row,x);
    return true;
}
static void release(void* user,const GcnResidentCopy* copy) {
    Fixture* f=user;assert(copy->token<TOKENS && f->release_count<TOKENS);
    ++f->released[copy->token];f->release_order[f->release_count++]=(unsigned)copy->token;
    if(f->check_replacement)
        assert(gcn_resident_memory_pending(f->manager,f->replacement_offset,1));
    if(f->probe_reentrancy)assert(!gcn_resident_memory_destroy(f->manager));
}
static GcnResidentMemoryConfig configuration(Fixture* f,uint32_t capacity) {
    const GcnResidentMemoryConfig c={MEMORY,32,capacity,f->pages,PAGES,materialize,release,f};return c;
}
static void open_fixture(Fixture* f,uint32_t capacity) {
    memset(f,0,sizeof(*f));memset(f->ram,0xCC,sizeof(f->ram));memset(f->pages,0xA5,sizeof(f->pages));
    const GcnResidentMemoryConfig c=configuration(f,capacity);
    f->manager=gcn_resident_memory_create(&c);assert(f->manager);
    for(unsigned i=0;i<PAGES;i++)assert(!f->pages[i]);
    assert(f->pages[PAGES]==0xA5);
}
static uint64_t publish(Fixture* f,uint64_t token,GcnResidentRows rows) {
    uint64_t generation=0;
    assert(gcn_resident_memory_publish(f->manager,token,&rows,&generation) && generation);return generation;
}
static void close_fixture(Fixture* f) {
    assert(gcn_resident_memory_destroy(f->manager));f->manager=NULL;
    for(unsigned i=0;i<PAGES;i++)assert(!f->pages[i]);
    assert(f->pages[PAGES]==0xA5);
}
static void configuration_and_floor(void) {
    Fixture f;open_fixture(&f,2);
    assert(gcn_resident_memory_prepare_read(f.manager,0,MEMORY));
    assert(gcn_resident_memory_prepare_write(f.manager,0,MEMORY,false));
    assert(gcn_resident_memory_prepare_read(f.manager,MEMORY,0));
    assert(!gcn_resident_memory_pending(f.manager,0,MEMORY));
    assert(!gcn_resident_memory_pending(f.manager,MEMORY,0));
    assert(gcn_resident_memory_pending(f.manager,MEMORY,1));
    assert(!gcn_resident_memory_prepare_read(f.manager,MEMORY,1));
    assert(!gcn_resident_memory_prepare_write(f.manager,1,UINT32_MAX,true));
    assert(!gcn_resident_memory_prepare_read(f.manager,UINT32_MAX,0));
    assert(gcn_resident_memory_error(f.manager)==GCN_RESIDENT_INVALID);
    const GcnResidentRows invalid[]={
        {0,0,1,4},{0,4,0,4},{0,8,1,4},{MEMORY,1,1,1},
        {UINT32_MAX,4,1,4},{0,4,UINT32_MAX,UINT32_MAX},
        {0,4,GCN_RESIDENT_MAX_ROWS+1,4},{MEMORY-3,4,1,4},
        {MEMORY-8,4,2,UINT32_MAX}
    };
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        uint64_t generation=99;
        assert(!gcn_resident_memory_publish(f.manager,1,invalid+i,&generation) && generation==99);
    }
    const GcnResidentRows valid={0,4,1,4};
    assert(!gcn_resident_memory_publish(f.manager,0,&valid,NULL));
    assert(!gcn_resident_memory_publish(f.manager,1,NULL,NULL));
    assert(!gcn_resident_memory_stats(f.manager).owned && !f.release_count);
    close_fixture(&f);
    GcnResidentMemoryConfig c=configuration(&f,2);
    assert(!gcn_resident_memory_create(NULL));
    c.page_size=3;assert(!gcn_resident_memory_create(&c));c=configuration(&f,2);
    c.page_count=1;assert(!gcn_resident_memory_create(&c));c=configuration(&f,2);
    c.capacity=GCN_RESIDENT_MAX_RECORDS+1;assert(!gcn_resident_memory_create(&c));c=configuration(&f,2);
    c.materialize=NULL;assert(!gcn_resident_memory_create(&c));c=configuration(&f,2);
    c.release=NULL;assert(!gcn_resident_memory_create(&c));c=configuration(&f,2);
    c.page_flags=NULL;assert(!gcn_resident_memory_create(&c));c=configuration(&f,2);
    c.memory_size=UINT32_MAX;c.page_size=1;c.page_count=UINT32_MAX;
    assert(!gcn_resident_memory_create(&c));
    assert(!gcn_resident_memory_prepare_read(NULL,0,0));
    assert(gcn_resident_memory_pending(NULL,0,0));
    assert(gcn_resident_memory_flush(NULL) && gcn_resident_memory_destroy(NULL));
}
static void padding_and_cross_page(void) {
    Fixture f;open_fixture(&f,3);
    const GcnResidentRows rows={28,8,3,96};publish(&f,1,rows);
    for(unsigned p=0;p<PAGES;p++)assert(f.pages[p]==(p==0 || p==1 || p==3 || p==4 || p==6 || p==7));
    assert(gcn_resident_memory_pending(f.manager,40,1)); /* Conservative shared page. */
    assert(!gcn_resident_memory_pending(f.manager,80,8));
    assert(gcn_resident_memory_prepare_read(f.manager,40,4));
    assert(gcn_resident_memory_prepare_write(f.manager,80,8,true));memset(f.ram+80,0x77,8);
    assert(!f.attempts[1] && !f.released[1]);
    GcnResidentCopy copy;
    assert(gcn_resident_memory_find(f.manager,28,&copy) && copy.token==1);
    assert(!gcn_resident_memory_find(f.manager,36,&copy));
    assert(gcn_resident_memory_copy_at(f.manager,0,&copy));
    assert(!gcn_resident_memory_copy_at(f.manager,1,&copy));
    assert(gcn_resident_memory_prepare_read(f.manager,31,2));
    assert(f.attempts[1]==1 && f.released[1]==1 && !gcn_resident_memory_stats(f.manager).owned);
    for(unsigned row=0;row<3;row++)for(unsigned x=0;x<8;x++)
        assert(f.ram[28+row*96+x]==value(1,row,x));
    assert(f.ram[36]==0xCC && f.ram[123]==0xCC && f.ram[132]==0xCC && f.ram[80]==0x77);
    assert(!gcn_resident_memory_pending(f.manager,0,MEMORY));close_fixture(&f);
}
static void stores_and_generations(void) {
    Fixture f;open_fixture(&f,3);const GcnResidentRows rows={100,8,2,16};
    const uint64_t a=publish(&f,1,rows);
    assert(gcn_resident_memory_prepare_write(f.manager,104,2,true));memset(f.ram+104,0xEE,2);
    assert(f.attempts[1]==1 && f.released[1]==1);
    assert(f.ram[103]==value(1,0,3) && f.ram[104]==0xEE && f.ram[106]==value(1,0,6));
    assert(f.ram[116]==value(1,1,0) && f.ram[108]==0xCC);
    const uint64_t b=publish(&f,2,rows);assert(b>a);
    assert(gcn_resident_memory_prepare_write(f.manager,100,0,true));assert(!f.released[2]);
    assert(gcn_resident_memory_prepare_write(f.manager,100,24,true));memset(f.ram+100,0x99,24);
    assert(!f.attempts[2] && f.released[2]==1);
    assert(gcn_resident_memory_stats(f.manager).discards==1);
    const uint64_t c=publish(&f,1,rows);assert(c>b); /* Token reused after release, new generation. */
    assert(gcn_resident_memory_prepare_write(f.manager,100,24,false));
    assert(f.attempts[1]==2 && f.released[1]==2 && f.ram[108]==0x99);
    close_fixture(&f);
}
static void overlap_and_replacement(void) {
    Fixture f;open_fixture(&f,3);
    const GcnResidentRows old={0,4,3,16},middle={16,4,1,4};
    const uint64_t a=publish(&f,1,old),b=publish(&f,2,middle);assert(b>a);
    assert(f.attempts[1]==1 && f.released[1]==1 && f.ram[32]==value(1,2,0));
    GcnResidentCopy copy;assert(!gcn_resident_memory_find(f.manager,0,&copy));
    assert(gcn_resident_memory_find(f.manager,16,&copy) && copy.token==2);
    f.check_replacement=true;f.replacement_offset=16;
    const uint64_t c=publish(&f,3,middle);f.check_replacement=false;assert(c>b);
    assert(!f.attempts[2] && f.released[2]==1);
    uint64_t unchanged=123;assert(!gcn_resident_memory_publish(f.manager,3,&middle,&unchanged));assert(unchanged==123);
    const GcnResidentRows padding={8,4,1,4};publish(&f,4,padding);
    assert(!f.attempts[3] && gcn_resident_memory_stats(f.manager).owned==2);
    close_fixture(&f);
    assert(f.release_order[2]==3 && f.release_order[3]==4);
    assert(f.ram[8]==value(4,0,0) && f.ram[16]==value(3,0,0) && f.ram[4]==0xCC);
}
static void eviction(void) {
    Fixture f;open_fixture(&f,2);
    publish(&f,1,(GcnResidentRows){0,4,1,4});publish(&f,2,(GcnResidentRows){32,4,1,4});
    f.fail_token=1;uint64_t generation=77;
    const GcnResidentRows third={64,4,1,4};
    assert(!gcn_resident_memory_publish(f.manager,3,&third,&generation) && generation==77);
    assert(!f.release_count && gcn_resident_memory_stats(f.manager).owned==2);
    assert(gcn_resident_memory_pending(f.manager,0,1));
    f.fail_token=0;assert(publish(&f,3,third)==3);
    assert(f.attempts[1]==2 && f.released[1]==1 && f.release_order[0]==1);
    assert(gcn_resident_memory_stats(f.manager).evictions==1 && f.ram[0]==value(1,0,0));
    close_fixture(&f);assert(f.release_order[1]==2 && f.release_order[2]==3);
}
static void failures_preserve_ownership(void) {
    Fixture f;open_fixture(&f,2);
    const GcnResidentRows a={0,8,1,8},b={16,8,1,8},both={0,20,1,20};
    publish(&f,1,a);publish(&f,2,b);f.fail_token=2;uint64_t generation=66;
    /* Full cover of A must not discard A if partial preservation of B fails. */
    assert(!gcn_resident_memory_publish(f.manager,3,&both,&generation) && generation==66);
    assert(!f.released[1] && !f.released[2] && !f.released[3]);
    assert(gcn_resident_memory_stats(f.manager).owned==2 && gcn_resident_memory_pending(f.manager,0,24));
    assert(gcn_resident_memory_error(f.manager)==GCN_RESIDENT_MATERIALIZE_FAILED);
    f.fail_token=0;assert(publish(&f,3,both)==3);
    assert(!f.attempts[1] && f.released[1]==1 && f.attempts[2]==2 && f.released[2]==1);
    assert(gcn_resident_memory_prepare_read(f.manager,0,20));
    assert(f.ram[19]==value(3,0,19) && f.ram[20]==value(2,0,4));close_fixture(&f);

    open_fixture(&f,2);publish(&f,1,a);publish(&f,2,b);f.fail_token=2;
    assert(!gcn_resident_memory_prepare_write(f.manager,0,20,true));
    assert(!f.released[1] && !f.released[2]); /* Caller must not perform the store. */
    f.fail_token=0;assert(gcn_resident_memory_prepare_write(f.manager,0,20,true));memset(f.ram,0xAA,20);
    assert(!f.attempts[1] && f.released[1]==1 && f.ram[20]==value(2,0,4));close_fixture(&f);

    open_fixture(&f,2);publish(&f,1,a);f.fail_token=1;
    assert(!gcn_resident_memory_flush(f.manager));assert(!gcn_resident_memory_destroy(f.manager));
    GcnResidentCopy copy;assert(gcn_resident_memory_find(f.manager,0,&copy) && copy.token==1);
    assert(f.pages[0] && !f.released[1]);
    assert(gcn_resident_memory_stats(f.manager).callback_failures==2);
    f.fail_token=0;close_fixture(&f);assert(f.attempts[1]==3 && f.released[1]==1);

    /* A failed flush may have committed a successful prefix, but must retain
     * every unresolved token and never release the successful prefix twice. */
    open_fixture(&f,2);publish(&f,1,a);publish(&f,2,(GcnResidentRows){64,8,1,8});f.fail_token=2;
    assert(!gcn_resident_memory_flush(f.manager));
    assert(f.released[1]==1 && !f.released[2] && !f.pages[0] && f.pages[2]);
    assert(!gcn_resident_memory_pending(f.manager,0,8));
    assert(gcn_resident_memory_pending(f.manager,64,8));
    assert(!gcn_resident_memory_destroy(f.manager));assert(f.released[1]==1 && !f.released[2]);
    f.fail_token=0;close_fixture(&f);assert(f.released[1]==1 && f.released[2]==1);
}
static void reentrancy_and_atomic_page_words(void) {
    Fixture f;open_fixture(&f,3);publish(&f,1,(GcnResidentRows){1022,8,1,8});
    publish(&f,2,(GcnResidentRows){1040,4,1,4});
    assert(f.pages[31] && f.pages[32]);
    assert(gcn_resident_memory_pending(f.manager,31*32,64));
    assert(!gcn_resident_memory_pending(f.manager,0,31*32));
    f.probe_reentrancy=true;assert(gcn_resident_memory_prepare_read(f.manager,1023,2));
    f.probe_reentrancy=false;
    assert(!f.pages[31] && f.pages[32]);
    assert(!gcn_resident_memory_pending(f.manager,31*32,32));
    assert(gcn_resident_memory_pending(f.manager,32*32,32));
    assert(gcn_resident_memory_prepare_read(f.manager,1023,2));assert(!f.attempts[2]);
    assert(gcn_resident_memory_stats(f.manager).reentrant_rejections==6 && !f.released[255]);
    close_fixture(&f);
}
static uint64_t active_bits(GcnResidentRows rows) {
    uint64_t bits=0;
    for(unsigned row=0;row<rows.rows;row++)for(unsigned x=0;x<rows.row_bytes;x++)
        bits|=UINT64_C(1)<<(rows.offset+row*rows.stride+x);
    return bits;
}
static void layout_oracle(void) {
    /* Compare coverage decisions and final bytes against independent per-byte
     * ownership, including equal envelopes with different stride holes. */
    static const GcnResidentRows layouts[]={
        {0,4,1,4},{0,4,3,4},{0,4,3,8},{0,4,3,16},{4,4,2,8},
        {8,4,1,4},{0,20,1,20},{1,1,3,7},{2,2,4,5},
        {0,2,3,8},{0,8,3,8},{4,2,4,7},{12,1,5,3},{31,2,2,16}
    };
    for(unsigned a=0;a<sizeof(layouts)/sizeof(layouts[0]);a++)
    for(unsigned b=0;b<sizeof(layouts)/sizeof(layouts[0]);b++) {
        Fixture f;open_fixture(&f,2);
        uint8_t expected[64];memset(expected,0xCC,sizeof(expected));
        const uint64_t prior=active_bits(layouts[a]),next=active_bits(layouts[b]);
        for(unsigned row=0;row<layouts[a].rows;row++)for(unsigned x=0;x<layouts[a].row_bytes;x++)
            expected[layouts[a].offset+row*layouts[a].stride+x]=value(1,row,x);
        for(unsigned row=0;row<layouts[b].rows;row++)for(unsigned x=0;x<layouts[b].row_bytes;x++)
            expected[layouts[b].offset+row*layouts[b].stride+x]=value(2,row,x);
        publish(&f,1,layouts[a]);publish(&f,2,layouts[b]);
        assert(f.attempts[1]==((prior&next)!=0 && (prior&~next)!=0));
        assert(f.released[1]==((prior&next)!=0));
        assert(gcn_resident_memory_flush(f.manager));assert(!memcmp(f.ram,expected,sizeof(expected)));
        assert(f.released[1]==1 && f.released[2]==1);close_fixture(&f);
    }
}
static void exact_completion_does_not_observe_replacements(void) {
    Fixture f;open_fixture(&f,3);
    const GcnResidentRows a={32,8,1,8};
    publish(&f,1,a);publish(&f,2,a); /* 1 was wholly replaced, not materialized */
    assert(gcn_resident_memory_complete(f.manager,1));
    assert(!f.attempts[2] && f.pages[1]);
    publish(&f,3,(GcnResidentRows){128,8,1,8});
    assert(gcn_resident_memory_complete(f.manager,2));
    assert(f.attempts[2]==1 && f.released[2]==1 && !f.attempts[3]);
    assert(gcn_resident_memory_complete(f.manager,2));assert(f.attempts[2]==1);
    f.fail_token=3;assert(!gcn_resident_memory_complete(f.manager,3));
    assert(!f.released[3] && gcn_resident_memory_pending(f.manager,128,8));
    f.fail_token=0;assert(gcn_resident_memory_complete(f.manager,3));
    assert(f.released[3]==1);close_fixture(&f);
}
static void retained_checkpoint_branches(void) {
    Fixture f,other;open_fixture(&f,4);open_fixture(&other,4);
    const GcnResidentRows rows={32,8,3,16};
    const uint64_t first=publish(&f,1,rows);
    GcnResidentCheckpoint* earlier=gcn_resident_memory_capture(f.manager);assert(earlier);
    const uint64_t second=publish(&f,2,rows);assert(second>first);
    GcnResidentCheckpoint* later=gcn_resident_memory_capture(f.manager);assert(later);
    assert(!gcn_resident_memory_restore(other.manager,earlier));
    assert(gcn_resident_memory_restore(f.manager,earlier));
    assert(!f.attempts[1] && !f.attempts[2]);
    GcnResidentCopy live;
    assert(gcn_resident_memory_find(f.manager,32,&live) && live.token==1);
    assert(gcn_resident_memory_pending(f.manager,32,8));
    assert(!gcn_resident_memory_owned_range(f.manager,40,8)); /* padding */
    assert(gcn_resident_memory_prepare_read(f.manager,32,8));
    assert(f.ram[32]==value(1,0,0) && f.ram[40]==0xcc);
    assert(publish(&f,3,(GcnResidentRows){512,8,1,8})>second);
    assert(gcn_resident_memory_restore(f.manager,later));
    assert(gcn_resident_memory_find(f.manager,32,&live) && live.token==2);
    assert(!gcn_resident_memory_pending(f.manager,512,8));
    assert(gcn_resident_memory_prepare_read(f.manager,32,8));
    assert(f.ram[32]==value(2,0,0));
    /* A third branch can still return to the older independently retained map. */
    assert(gcn_resident_memory_restore(f.manager,earlier));
    assert(gcn_resident_memory_find(f.manager,32,&live) && live.token==1);
    gcn_resident_memory_checkpoint_close(later);
    gcn_resident_memory_checkpoint_close(earlier);
    close_fixture(&f);close_fixture(&other);
}
int main(void) {
    retained_checkpoint_branches();
    configuration_and_floor();padding_and_cross_page();stores_and_generations();
    overlap_and_replacement();eviction();failures_preserve_ownership();
    reentrancy_and_atomic_page_words();layout_oracle();exact_completion_does_not_observe_replacements();
    puts("resident memory: rows/padding, access preparation, replacement, eviction, failure/reentrancy and token lifetime passed");
    return 0;
}
