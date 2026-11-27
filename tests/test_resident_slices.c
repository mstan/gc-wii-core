/* SPDX-License-Identifier: MIT
 * Compare observable memory against an independent byte oracle, including
 * GPU generations which are only partly superseded and later CPU writes. */
#include "gcnllr/resident_memory.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum {MEMORY=2048,TOKENS=8192,PAGE=32};
typedef struct {
    GcnResidentMemory* manager;
    unsigned salt[TOKENS],sequence,attempts[TOKENS],releases[TOKENS];
    bool held[TOKENS];
    uint64_t fail;
    uint8_t ram[MEMORY],expected[MEMORY],pages[MEMORY/PAGE];
} Fixture;
static uint8_t byte(const Fixture* f,unsigned token,unsigned row,unsigned x) {
    return (uint8_t)(f->salt[token]*31u+row*19u+x*7u);
}
static bool materialize(void* user,const GcnResidentCopy* c) {
    Fixture* f=user;const GcnResidentRows* r=&c->destination;
    assert(c->token<TOKENS && f->held[c->token]);++f->attempts[c->token];
    bool wrote=false;
    for(unsigned y=0;y<r->rows;y++) {
        uint32_t cursor=r->offset+y*r->stride,last=cursor+r->row_bytes,first,length;
        while(gcn_resident_memory_next_span(f->manager,c->token,cursor,last-cursor,&first,&length)) {
            assert(first>=cursor && length && first+length<=last);
            for(unsigned x=0;x<length;x++)f->ram[first+x]=byte(f,(unsigned)c->token,y,first+x-r->offset-y*r->stride);
            wrote=true;
            if(c->token==f->fail)return false;
            cursor=first+length;
        }
    }
    assert(wrote);return true;
}
static void release(void* user,const GcnResidentCopy* c) {
    Fixture* f=user;assert(c->token<TOKENS && f->held[c->token]);
    f->held[c->token]=false;++f->releases[c->token];
}
static void open_fixture(Fixture* f,unsigned capacity) {
    memset(f,0,sizeof(*f));memset(f->ram,0xA5,MEMORY);memset(f->expected,0xA5,MEMORY);
    const GcnResidentMemoryConfig config={MEMORY,PAGE,capacity,f->pages,MEMORY/PAGE,materialize,release,f};
    f->manager=gcn_resident_memory_create(&config);assert(f->manager);
}
static void check(Fixture* f) {
    const GcnResidentPageBounds* actual=gcn_resident_memory_page_bounds(f->manager);
    for(unsigned page=0;page<MEMORY/PAGE;++page) {
        uint32_t first=0,limit=0;
        for(unsigned i=page*PAGE;i<(page+1)*PAGE;++i) {
            if(gcn_resident_memory_owned_range(f->manager,i,1)) {
                if(!limit)first=i;
                limit=i+1;
            } else assert(f->ram[i]==f->expected[i]);
        }
        assert(gcn_resident_page_first(actual[page])==first &&
               gcn_resident_page_limit(actual[page])==limit);
    }
}
static void publish(Fixture* f,unsigned token,GcnResidentRows rows,bool sliced) {
    assert(token && token<TOKENS && !f->held[token]);
    f->held[token]=true;f->salt[token]=++f->sequence;
    assert((sliced?gcn_resident_memory_publish_slices:gcn_resident_memory_publish)(f->manager,token,&rows,NULL));
    for(unsigned y=0;y<rows.rows;y++)for(unsigned x=0;x<rows.row_bytes;x++)
        f->expected[rows.offset+y*rows.stride+x]=byte(f,token,y,x);
    check(f);
}
static void close_fixture(Fixture* f) {
    assert(gcn_resident_memory_destroy(f->manager));
    assert(!memcmp(f->ram,f->expected,MEMORY));
    for(unsigned i=0;i<TOKENS;i++)assert(!f->held[i]);
    for(unsigned i=0;i<MEMORY/PAGE;i++)assert(!f->pages[i]);
}
static void surviving_regions_and_cpu_bytes(void) {
    Fixture f;open_fixture(&f,8);
    publish(&f,1,(GcnResidentRows){100,8,8,8},true);
    publish(&f,2,(GcnResidentRows){124,8,2,8},true);
    assert(!f.attempts[1] && !f.releases[1]);
    assert(gcn_resident_memory_prepare_read(f.manager,110,0) && !f.attempts[1]);
    uint32_t first,length;
    assert(gcn_resident_memory_next_span(f.manager,1,100,64,&first,&length) && first==100 && length==24);
    assert(gcn_resident_memory_next_span(f.manager,1,124,40,&first,&length) && first==140 && length==24);
    GcnResidentCopy c;assert(gcn_resident_memory_find(f.manager,140,&c));
    assert(c.token==1 && c.destination.offset==100 && c.destination.rows==8);
    /* Newer RAM must not be overwritten when the older image is observed. */
    assert(gcn_resident_memory_prepare_read(f.manager,124,16));
    assert(f.releases[2] && !f.attempts[1]);
    assert(gcn_resident_memory_prepare_write(f.manager,124,16,true));
    memset(f.ram+124,0x37,16);memset(f.expected+124,0x37,16);
    assert(gcn_resident_memory_prepare_read(f.manager,100,64));
    assert(f.attempts[1]==1 && !memcmp(f.ram+100,f.expected+100,64));
    close_fixture(&f);
}
static int materialize_page(void* user,const GcnResidentCopy* copy,uint32_t offset,uint32_t size) {
    Fixture* f=user;
    if(f->fail==copy->token)return -1;
    uint32_t first,length;
    const uint32_t last=offset+size;
    while(gcn_resident_memory_next_span(f->manager,copy->token,offset,last-offset,&first,&length)) {
        const GcnResidentRows* r=&copy->destination;
        for(uint32_t i=first;i<first+length;i++) {
            const uint32_t pixel=i-r->offset;
            f->ram[i]=byte(f,(unsigned)copy->token,pixel/r->stride,pixel%r->stride);
        }
        offset=first+length;
    }
    return 1;
}
static void page_observation(void) {
    Fixture f;open_fixture(&f,8);
    publish(&f,1,(GcnResidentRows){100,16,6,16},true);
    publish(&f,2,(GcnResidentRows){132,8,1,8},true);
    f.fail=2;
    assert(!gcn_resident_memory_prepare_page(f.manager,137,false,materialize_page));
    assert(f.pages[4] && f.held[2]);
    f.fail=0;
    assert(gcn_resident_memory_prepare_page(f.manager,137,true,materialize_page));
    assert(!f.pages[4] && f.pages[3] && f.pages[5]);
    assert(f.held[1] && !f.attempts[1] && !f.held[2]);
    assert(!memcmp(f.ram+128,f.expected+128,PAGE));
    f.ram[137]=f.expected[137]=3;
    close_fixture(&f);
}
static void failure_and_fragment_bound(void) {
    Fixture f;open_fixture(&f,64);
    publish(&f,1,(GcnResidentRows){100,64,1,64},true);
    publish(&f,2,(GcnResidentRows){120,8,1,8},true);
    f.fail=1;assert(!gcn_resident_memory_prepare_read(f.manager,100,1));
    assert(f.held[1] && f.held[2] && gcn_resident_memory_stats(f.manager).callback_failures==1);
    f.fail=0;assert(gcn_resident_memory_prepare_read(f.manager,100,64));check(&f);
    publish(&f,3,(GcnResidentRows){500,256,1,256},true);
    for(unsigned i=0;i<20;i++)publish(&f,4+i,(GcnResidentRows){502+i*8,2,1,2},true);
    assert(f.attempts[3]==1); /* Bounded fallback, not lost fragments. */
    close_fixture(&f);
}
static unsigned random_word(void) {
    static unsigned state=0x173b5a97u;state=state*1664525u+1013904223u;return state;
}
static void randomized_observable_contract(void) {
    Fixture f;open_fixture(&f,5);
    for(unsigned step=0;step<4000;step++) {
        const unsigned operation=random_word()%7;
        if(operation<3) {
            const unsigned token=step+1;assert(token<TOKENS && !f.held[token]);
            const unsigned width=1+random_word()%80,rows=1+random_word()%6;
            const bool sliced=(random_word()&3)!=0;
            const unsigned stride=width+(sliced?0:random_word()%20),extent=(rows-1)*stride+width;
            publish(&f,token,(GcnResidentRows){random_word()%(MEMORY-extent+1),width,rows,stride},sliced);
        } else {
            const unsigned size=random_word()%120,offset=random_word()%(MEMORY-size+1);
            if(operation==3) {
                assert(gcn_resident_memory_prepare_read(f.manager,offset,size));
                assert(!memcmp(f.ram+offset,f.expected+offset,size));
            } else if(operation<6) {
                assert(gcn_resident_memory_prepare_write(f.manager,offset,size,operation==4));
                const int value=random_word()&255;memset(f.ram+offset,value,size);memset(f.expected+offset,value,size);
            } else assert(gcn_resident_memory_flush(f.manager));
            check(&f);
        }
    }
    close_fixture(&f);
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<MEMORY;i++)hash=(hash^f.ram[i])*16777619u;
    printf("observable byte-oracle hash=%08X\n",hash);
}
int main(int argc,char** argv) {
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--oracle-only")));
    if(argc==1) {surviving_regions_and_cpu_bytes();page_observation();failure_and_fragment_bound();}
    randomized_observable_contract();
    puts("GPU ownership slices: exact byte oracle, partial supersession, CPU stores, failure, eviction and bounded fallback passed");
}
