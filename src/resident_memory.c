/* SPDX-License-Identifier: MIT */
#include "gcnllr/resident_memory.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {MAX_SLICES=16};
typedef struct {uint32_t first,last;} Slice;
typedef struct {
    GcnResidentCopy copy;
    bool owned,sliced;
    unsigned slice_count;
    Slice slices[MAX_SLICES];
} Record;
enum {M_PARTIAL,M_EVICT,M_READ,M_WRITE,M_OVERWRITE,M_COMPLETE,M_FLUSH,M_REASONS};
static const char* const materialize_reasons[]={"partial-publish","eviction","read","write","overwrite","completion","flush"};
struct GcnResidentMemory {
    GcnResidentMemoryConfig config;
    Record* records;
    uint32_t* page_refs;
    GcnResidentPageBounds* page_bounds;
    uint8_t* stale_bounds;
    _Atomic(uint32_t)* pending;
    uint32_t pages,first_stale,last_stale;
    uint64_t generation;
    bool busy;
    GcnResidentError error;
    GcnResidentMemoryStats stats;
    bool profile;
    unsigned profile_examples;
    uint64_t materialize_attempts[M_REASONS];
    uint64_t range_preparations,range_misses;
};
static bool begin(GcnResidentMemory* m) {
    if(!m)return false;
    if(m->busy) {m->error=GCN_RESIDENT_BUSY;++m->stats.reentrant_rejections;return false;}
    m->busy=true;m->error=GCN_RESIDENT_OK;return true;
}
static void rebuild_bounds(GcnResidentMemory* m);
static bool finish(GcnResidentMemory* m,bool result) {
    if(m->first_stale<m->pages)rebuild_bounds(m);
    m->busy=false;return result;
}
static bool valid_range(const GcnResidentMemory* m,uint32_t offset,uint32_t size) {
    return offset<=m->config.memory_size && size<=m->config.memory_size-offset;
}
static uint64_t end(const GcnResidentRows* r) {
    return (uint64_t)r->offset+(uint64_t)(r->rows-1)*r->stride+r->row_bytes;
}
static bool valid_rows(const GcnResidentMemory* m,const GcnResidentRows* r) {
    return r && r->row_bytes && r->rows && r->rows<=GCN_RESIDENT_MAX_ROWS &&
        r->stride>=r->row_bytes && r->offset<m->config.memory_size && end(r)<=m->config.memory_size;
}
/* Collapse adjacent rows for interval arithmetic without changing the
 * descriptor passed to the owner, which retains the original GPU layout. */
static GcnResidentRows compact(GcnResidentRows r) {
    if(r.stride==r.row_bytes) {r.row_bytes*=r.rows;r.stride=r.row_bytes;r.rows=1;}
    return r;
}
static bool intersects(const GcnResidentRows* r,uint64_t first,uint64_t last) {
    if(first>=last || last<=r->offset || first>=end(r))return false;
    uint64_t row=first>r->offset ? (first-r->offset)/r->stride:0;
    if(row>=r->rows)return false;
    uint64_t start=(uint64_t)r->offset+row*r->stride;
    if(start+r->row_bytes<=first) {++row;start+=r->stride;}
    return row<r->rows && start<last;
}
static bool overlap(GcnResidentRows a,GcnResidentRows b) {
    a=compact(a);b=compact(b);
    if(a.rows>b.rows) {const GcnResidentRows swap=a;a=b;b=swap;}
    for(uint32_t row=0;row<a.rows;row++) {
        const uint64_t start=(uint64_t)a.offset+(uint64_t)row*a.stride;
        if(intersects(&b,start,start+a.row_bytes))return true;
    }
    return false;
}
static bool covers(GcnResidentRows outer,GcnResidentRows inner) {
    outer=compact(outer);inner=compact(inner);
    if(outer.offset>inner.offset || end(&outer)<end(&inner))return false;
    for(uint32_t row=0;row<inner.rows;row++) {
        const uint64_t start=(uint64_t)inner.offset+(uint64_t)row*inner.stride;
        const uint64_t index=(start-outer.offset)/outer.stride;
        if(index>=outer.rows || start+inner.row_bytes>
            (uint64_t)outer.offset+index*outer.stride+outer.row_bytes)return false;
    }
    return true;
}
static bool record_intersects(const Record* r,uint64_t first,uint64_t last) {
    if(first>=last)return false;
    if(!r->sliced)return intersects(&r->copy.destination,first,last);
    for(unsigned i=0;i<r->slice_count;i++)if(first<r->slices[i].last && r->slices[i].first<last)return true;
    return false;
}
static bool record_overlaps(const Record* r,GcnResidentRows rows) {
    if(!r->sliced)return overlap(rows,r->copy.destination);
    for(unsigned i=0;i<r->slice_count;i++)
        if(intersects(&rows,r->slices[i].first,r->slices[i].last))return true;
    return false;
}
/* Bounded subtraction. Exceeding the fragment budget uses whole-copy
 * materialization; it never drops surviving bytes or grows an unbounded map. */
static bool subtract(const Record* r,GcnResidentRows incoming,Slice* out,unsigned* count) {
    incoming=compact(incoming);*count=0;
    for(unsigned i=0;i<r->slice_count;i++) {
        uint64_t cursor=r->slices[i].first,last=r->slices[i].last;
        uint64_t row=cursor>incoming.offset ? (cursor-incoming.offset)/incoming.stride:0;
        while(row<incoming.rows && cursor<last) {
            const uint64_t a=(uint64_t)incoming.offset+row*incoming.stride,b=a+incoming.row_bytes;
            if(a>=last)break;
            if(b>cursor) {
                if(a>cursor) {
                    if(*count==MAX_SLICES)return false;
                    out[(*count)++]=(Slice){(uint32_t)cursor,(uint32_t)a};
                }
                if(b>cursor)cursor=b;
            }
            ++row;
        }
        if(cursor<last) {
            if(*count==MAX_SLICES)return false;
            out[(*count)++]=(Slice){(uint32_t)cursor,(uint32_t)last};
        }
    }
    return true;
}
static bool record_covered(const Record* r,GcnResidentRows incoming) {
    if(!r->sliced)return covers(incoming,r->copy.destination);
    Slice remaining[MAX_SLICES];unsigned count;
    return subtract(r,incoming,remaining,&count) && !count;
}
static void expand_bounds(GcnResidentPageBounds* bounds,uint64_t start,uint64_t end) {
    const uint32_t first=gcn_resident_page_first(*bounds);
    const uint32_t limit=gcn_resident_page_limit(*bounds);
    if(*bounds && first<start)start=first;
    if(limit>end)end=limit;
    *bounds=(end<<32)|start;
}
static void rebuild_bounds(GcnResidentMemory* m) {
    for(uint32_t page=m->first_stale;page<=m->last_stale;++page) {
        if(!m->stale_bounds[page])continue;
        m->stale_bounds[page]=0;
        GcnResidentPageBounds* bounds=m->page_bounds+page;
        *bounds=0;
        if(!m->page_refs[page])continue;
        const uint64_t low=(uint64_t)page*m->config.page_size;
        const uint64_t high=low+m->config.page_size;
        const uint32_t limit=(uint32_t)(high<m->config.memory_size?high:m->config.memory_size);
        for(uint32_t i=0;i<m->config.capacity;++i) {
            const Record* record=m->records+i;
            if(!record->owned || !record_intersects(record,low,limit))continue;
            uint32_t at=(uint32_t)low,first,size;
            while(gcn_resident_memory_next_span(m,record->copy.token,at,limit-at,&first,&size)) {
                at=first+size;expand_bounds(bounds,first,at);
            }
        }
    }
    m->first_stale=m->pages;m->last_stale=0;
}
static void flags(GcnResidentMemory* m,const GcnResidentRows* r,bool add) {
    const GcnResidentRows active=compact(*r);
    for(uint32_t row=0;row<active.rows;row++) {
        const uint64_t start=(uint64_t)active.offset+(uint64_t)row*active.stride;
        const uint32_t first=(uint32_t)(start/m->config.page_size);
        const uint32_t last=(uint32_t)((start+active.row_bytes-1)/m->config.page_size);
        for(uint32_t page=first;page<=last;page++) {
            const uint32_t before=m->page_refs[page];
            GcnResidentPageBounds* bounds=m->page_bounds+page;
            const uint64_t page_start=(uint64_t)page*m->config.page_size;
            const uint64_t page_end=page_start+m->config.page_size;
            const uint64_t first_byte=start>page_start?start:page_start;
            const uint64_t last_byte=start+active.row_bytes<page_end?start+active.row_bytes:page_end;
            if(add) {
                ++m->page_refs[page];
                expand_bounds(bounds,first_byte,last_byte);
            } else if(!--m->page_refs[page]) {
                *bounds=0;
                m->stale_bounds[page]=0;
            } else if(first_byte==gcn_resident_page_first(*bounds) ||
                      last_byte==gcn_resident_page_limit(*bounds)) {
                m->stale_bounds[page]=1;
                if(page<m->first_stale)m->first_stale=page;
                if(page>m->last_stale)m->last_stale=page;
            }
            m->config.page_flags[page]=(uint8_t)((m->config.page_flags[page]&~GCN_RESIDENT_PAGE_FLAG) |
                (m->page_refs[page]?GCN_RESIDENT_PAGE_FLAG:0u));
            const uint32_t mask=1u<<(page%32);
            if(!before)atomic_fetch_or_explicit(m->pending+page/32,mask,memory_order_release);
            else if(!m->page_refs[page])atomic_fetch_and_explicit(m->pending+page/32,~mask,memory_order_release);
        }
    }
}
static void slice_flags(GcnResidentMemory* m,const Slice* slices,unsigned count,bool add) {
    for(unsigned i=0;i<count;i++) {
        const uint32_t size=slices[i].last-slices[i].first;
        const GcnResidentRows rows={slices[i].first,size,1,size};flags(m,&rows,add);
    }
}
static void record_flags(GcnResidentMemory* m,const Record* r,bool add) {
    if(r->sliced)slice_flags(m,r->slices,r->slice_count,add);
    else flags(m,&r->copy.destination,add);
}
static void retire(GcnResidentMemory* m,uint32_t index,bool discarded) {
    Record* r=m->records+index;const GcnResidentCopy copy=r->copy;
    record_flags(m,r,false);r->owned=false;--m->stats.owned;
    if(discarded)++m->stats.discards;
    m->config.release(m->config.user,&copy);
}

struct GcnResidentCheckpoint {
    const GcnResidentMemory* owner;
    uint32_t capacity,owned;
    Record records[];
};
GcnResidentCheckpoint* gcn_resident_memory_capture(GcnResidentMemory* m) {
    if(!m || m->busy)return NULL;
    GcnResidentCheckpoint* saved=malloc(sizeof(*saved)+(size_t)m->config.capacity*sizeof(Record));
    if(!saved)return NULL;
    saved->owner=m;saved->capacity=m->config.capacity;saved->owned=m->stats.owned;
    memcpy(saved->records,m->records,(size_t)saved->capacity*sizeof(Record));
    return saved;
}
bool gcn_resident_memory_restore(GcnResidentMemory* m,const GcnResidentCheckpoint* saved) {
    if(!m || !saved || saved->owner!=m || saved->capacity!=m->config.capacity || !begin(m))return false;
    // The opaque checkpoint was constructed by this owner. Retiring the old
    // branch only releases handles; the new provider is restored afterwards.
    for(uint32_t i=0;i<m->config.capacity;++i)if(m->records[i].owned)retire(m,i,true);
    memcpy(m->records,saved->records,(size_t)saved->capacity*sizeof(Record));
    m->stats.owned=saved->owned;
    for(uint32_t i=0;i<m->config.capacity;++i)if(m->records[i].owned)record_flags(m,m->records+i,true);
    // Keep publication generations monotonic across abandoned branches.
    return finish(m,true);
}
void gcn_resident_memory_checkpoint_close(GcnResidentCheckpoint* saved){free(saved);}
static bool materialize(GcnResidentMemory* m,uint32_t index,unsigned reason,uint32_t offset,uint32_t size) {
    if(m->profile) {
        ++m->materialize_attempts[reason];
        if(m->profile_examples++<16) {
            const GcnResidentCopy* c=&m->records[index].copy;
            fprintf(stderr,"[resident-demand] reason=%s token=%llu copy=%08X/%u/%u/%u access=%08X/%u\n",
                materialize_reasons[reason],(unsigned long long)c->token,c->destination.offset,
                c->destination.row_bytes,c->destination.rows,c->destination.stride,offset,size);
        }
    }
    if(!m->config.materialize(m->config.user,&m->records[index].copy)) {
        m->error=GCN_RESIDENT_MATERIALIZE_FAILED;++m->stats.callback_failures;return false;
    }
    ++m->stats.materializations;retire(m,index,false);return true;
}
static uint32_t oldest(const GcnResidentMemory* m) {
    uint32_t selected=m->config.capacity;uint64_t generation=UINT64_MAX;
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned &&
        (selected==m->config.capacity || m->records[i].copy.generation<generation)) {
        selected=i;generation=m->records[i].copy.generation;
    }
    return selected;
}
GcnResidentMemory* gcn_resident_memory_create(const GcnResidentMemoryConfig* config) {
    if(!config || !config->memory_size || !config->page_size ||
       (config->page_size&(config->page_size-1)) || !config->capacity ||
       config->capacity>GCN_RESIDENT_MAX_RECORDS || !config->page_flags ||
       !config->materialize || !config->release)return NULL;
    const uint64_t pages=((uint64_t)config->memory_size+config->page_size-1)/config->page_size;
    if(pages>GCN_RESIDENT_MAX_PAGES || pages>config->page_count)return NULL;
    GcnResidentMemory* m=calloc(1,sizeof(*m));if(!m)return NULL;
    m->records=calloc(config->capacity,sizeof(*m->records));
    m->page_refs=calloc((size_t)pages,sizeof(*m->page_refs));
    m->page_bounds=calloc((size_t)pages,sizeof(*m->page_bounds));
    m->stale_bounds=calloc((size_t)pages,1);
    const size_t words=((size_t)pages+31)/32;
    m->pending=malloc(words*sizeof(*m->pending));
    if(!m->records || !m->page_refs || !m->page_bounds || !m->stale_bounds || !m->pending) {
        free(m->records);free(m->page_refs);free(m->page_bounds);free(m->stale_bounds);
        free(m->pending);free(m);return NULL;
    }
    for(size_t i=0;i<words;i++)atomic_init(m->pending+i,0);
    m->config=*config;m->pages=(uint32_t)pages;m->profile=getenv("GCNLLR_RESIDENT_PROFILE")!=NULL;
    m->first_stale=m->pages;
    memset(config->page_flags,0,m->pages);return m;
}
static bool publish(GcnResidentMemory* m,uint64_t token,const GcnResidentRows* destination,
                    uint64_t* generation,bool sliced) {
    if(!begin(m))return false;
    if(!token || !valid_rows(m,destination)) {m->error=GCN_RESIDENT_INVALID;return finish(m,false);}
    /* Snapshot caller metadata before any callback can run. */
    const GcnResidentRows next=*destination;
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned && m->records[i].copy.token==token) {
        m->error=GCN_RESIDENT_INVALID;return finish(m,false);
    }
    if(m->generation==UINT64_MAX) {m->error=GCN_RESIDENT_GENERATION_EXHAUSTED;return finish(m,false);}
    uint32_t slot=m->config.capacity;
    for(uint32_t i=0;i<m->config.capacity;i++) {
        const Record* r=m->records+i;
        if(r->owned && record_overlaps(r,next) && !record_covered(r,next)) {
            Slice remaining[MAX_SLICES];unsigned count;
            if(r->sliced && subtract(r,next,remaining,&count))continue;
            if(!materialize(m,i,M_PARTIAL,next.offset,(uint32_t)(end(&next)-next.offset)))return finish(m,false);
        }
    }
    /* Covered predecessors stay owned until all fallible work succeeds. If
     * publication fails, their stale RAM must not become accessible. */
    for(uint32_t i=0;i<m->config.capacity;i++)if(!m->records[i].owned ||
        record_covered(m->records+i,next)) {slot=i;break;}
    if(slot==m->config.capacity) {
        slot=oldest(m);
        if(!materialize(m,slot,M_EVICT,next.offset,(uint32_t)(end(&next)-next.offset)))return finish(m,false);
        ++m->stats.evictions;
    }
    /* Publish conservative bits before retiring fully covered predecessors:
     * replacement must not transiently clear a continuously owned page. */
    flags(m,&next,true);
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned) {
        Record* old=m->records+i;
        if(record_covered(old,next))retire(m,i,true);
        else if(old->sliced && record_overlaps(old,next)) {
            Slice remaining[MAX_SLICES];unsigned count;
            /* Preflight above materialized every over-budget predecessor.
             * No callbacks can mutate surviving records while busy. */
            if(!subtract(old,next,remaining,&count))abort();
            slice_flags(m,remaining,count,true);record_flags(m,old,false);
            memcpy(old->slices,remaining,count*sizeof(*remaining));old->slice_count=count;
        }
    }
    Record* r=m->records+slot;
    memset(r,0,sizeof(*r));
    r->copy=(GcnResidentCopy){token,++m->generation,next};r->owned=true;
    r->sliced=sliced && next.stride==next.row_bytes;
    if(r->sliced) {r->slice_count=1;r->slices[0]=(Slice){next.offset,(uint32_t)end(&next)};}
    ++m->stats.owned;++m->stats.publications;
    if(generation)*generation=r->copy.generation;
    m->error=GCN_RESIDENT_OK;return finish(m,true);
}
bool gcn_resident_memory_publish(GcnResidentMemory* m,uint64_t token,
                                 const GcnResidentRows* destination,uint64_t* generation) {
    return publish(m,token,destination,generation,false);
}
bool gcn_resident_memory_publish_slices(GcnResidentMemory* m,uint64_t token,
                                       const GcnResidentRows* destination,uint64_t* generation) {
#if GCNLLR_RESIDENT_SLICES
    return publish(m,token,destination,generation,true);
#else
    return publish(m,token,destination,generation,false);
#endif
}
static bool prepare(GcnResidentMemory* m,uint32_t offset,uint32_t size,bool overwrite,unsigned reason) {
    if(!begin(m))return false;
    if(!valid_range(m,offset,size)) {m->error=GCN_RESIDENT_INVALID;return finish(m,false);}
    const uint64_t last=(uint64_t)offset+size;
    /* Materialize partial records before committing any promised full-cover
     * discard. On failure the caller must not perform its intended store. */
    ++m->range_preparations;
    bool intersects_owner=false;
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned) {
        const GcnResidentRows range={offset,size,1,size};
        if(!record_intersects(m->records+i,offset,last))continue;
        intersects_owner=true;
        if(!(overwrite && size && record_covered(m->records+i,range)) &&
           !materialize(m,i,reason,offset,size))return finish(m,false);
    }
    m->range_misses+=!intersects_owner;
    if(overwrite && size)for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned) {
        const GcnResidentRows range={offset,size,1,size};
        if(record_covered(m->records+i,range))retire(m,i,true);
    }
    m->error=GCN_RESIDENT_OK;return finish(m,true);
}
bool gcn_resident_memory_prepare_read(GcnResidentMemory* m,uint32_t offset,uint32_t size) {
    return prepare(m,offset,size,false,M_READ);
}
bool gcn_resident_memory_prepare_write(GcnResidentMemory* m,uint32_t offset,uint32_t size,bool complete) {
    return prepare(m,offset,size,complete,complete?M_OVERWRITE:M_WRITE);
}
bool gcn_resident_memory_prepare_page(GcnResidentMemory* m,uint32_t offset,bool writing,
                                    GcnResidentMaterializeRange materialize_range) {
    if(!begin(m))return false;
    if(offset>=m->config.memory_size) {m->error=GCN_RESIDENT_INVALID;return finish(m,false);}
    const uint32_t first=offset&~(m->config.page_size-1u);
    uint32_t size=m->config.memory_size-first;
    if(size>m->config.page_size)size=m->config.page_size;
    const GcnResidentRows range={first,size,1,size};
    for(uint32_t i=0;i<m->config.capacity;i++) {
        Record* r=m->records+i;
        if(!r->owned || !record_intersects(r,first,(uint64_t)first+size))continue;
        Slice remaining[MAX_SLICES];unsigned count;
        int result=0;
        if(r->sliced && materialize_range && subtract(r,range,remaining,&count))
            result=materialize_range(m->config.user,&r->copy,first,size);
        if(result<0) {
            m->error=GCN_RESIDENT_MATERIALIZE_FAILED;++m->stats.callback_failures;return finish(m,false);
        }
        if(!result) {
            if(!materialize(m,i,writing?M_WRITE:M_READ,first,size))return finish(m,false);
        } else if(!count) {
            ++m->stats.materializations;retire(m,i,false);
        } else {
            record_flags(m,r,false);
            r->slice_count=count;memcpy(r->slices,remaining,count*sizeof(*remaining));
            record_flags(m,r,true);
        }
    }
    return finish(m,true);
}
bool gcn_resident_memory_complete(GcnResidentMemory* m,uint64_t token) {
    if(!begin(m))return false;
    if(!token){m->error=GCN_RESIDENT_INVALID;return finish(m,false);}
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned && m->records[i].copy.token==token)
        return finish(m,materialize(m,i,M_COMPLETE,0,0));
    return finish(m,true);
}
bool gcn_resident_memory_flush(GcnResidentMemory* m) {
    if(!m)return true;
    if(!begin(m))return false;
    while(m->stats.owned)if(!materialize(m,oldest(m),M_FLUSH,0,0))return finish(m,false);
    m->error=GCN_RESIDENT_OK;return finish(m,true);
}
bool gcn_resident_memory_destroy(GcnResidentMemory* m) {
    if(!m)return true;
    if(!gcn_resident_memory_flush(m))return false;
    fprintf(stderr,"[resident-range-observation] attempts=%llu no-owner-overlap=%llu\n",
        (unsigned long long)m->range_preparations,(unsigned long long)m->range_misses);
    if(m->profile) {
        fprintf(stderr,"[resident-demand-summary]");
        for(unsigned i=0;i<M_REASONS;i++)fprintf(stderr," %s=%llu",materialize_reasons[i],
            (unsigned long long)m->materialize_attempts[i]);
        fprintf(stderr,"\n");
    }
    free(m->records);free(m->page_refs);free(m->page_bounds);free(m->stale_bounds);
    free(m->pending);free(m);return true;
}
const GcnResidentPageBounds* gcn_resident_memory_page_bounds(const GcnResidentMemory* m) {
    return m?m->page_bounds:NULL;
}
bool gcn_resident_memory_pending(const GcnResidentMemory* m,uint32_t offset,uint32_t size) {
    if(!m || !valid_range(m,offset,size))return true;
    if(!size)return false;
    const uint32_t first=offset/m->config.page_size;
    const uint32_t last=(uint32_t)(((uint64_t)offset+size-1)/m->config.page_size);
    for(uint32_t word=first/32;word<=last/32;word++) {
        uint32_t mask=UINT32_MAX;
        if(word==first/32)mask&=UINT32_MAX<<(first%32);
        if(word==last/32)mask&=UINT32_MAX>>(31-last%32);
        if(atomic_load_explicit(m->pending+word,memory_order_acquire)&mask)return true;
    }
    return false;
}
bool gcn_resident_memory_copy_at(const GcnResidentMemory* m,uint32_t index,GcnResidentCopy* copy) {
    if(!m || !copy)return false;
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned) {
        if(!index) {*copy=m->records[i].copy;return true;}--index;
    }
    return false;
}
bool gcn_resident_memory_find(const GcnResidentMemory* m,uint32_t offset,GcnResidentCopy* copy) {
    if(!m || !copy || offset>=m->config.memory_size)return false;
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned &&
        record_intersects(m->records+i,offset,(uint64_t)offset+1)) {
        *copy=m->records[i].copy;return true;
    }
    return false;
}
bool gcn_resident_memory_owned_range(const GcnResidentMemory* m,uint32_t offset,uint32_t size) {
    if(!m || offset>m->config.memory_size || size>m->config.memory_size-offset)return true;
    if(!size)return false;
    for(uint32_t i=0;i<m->config.capacity;i++)if(m->records[i].owned &&
        record_intersects(m->records+i,offset,(uint64_t)offset+size))return true;
    return false;
}
bool gcn_resident_memory_next_span(const GcnResidentMemory* m,uint64_t token,uint32_t offset,
                                  uint32_t size,uint32_t* first,uint32_t* length) {
    if(!m || !token || !first || !length || !size || !valid_range(m,offset,size))return false;
    const uint64_t last=(uint64_t)offset+size;
    for(uint32_t i=0;i<m->config.capacity;i++) {
        const Record* r=m->records+i;
        if(!r->owned || r->copy.token!=token)continue;
        if(r->sliced) {
            for(unsigned j=0;j<r->slice_count;j++) {
                const uint64_t a=r->slices[j].first>offset?r->slices[j].first:offset;
                const uint64_t b=r->slices[j].last<last?r->slices[j].last:last;
                if(a<b) {*first=(uint32_t)a;*length=(uint32_t)(b-a);return true;}
            }
        } else {
            const GcnResidentRows rows=compact(r->copy.destination);
            uint64_t row=offset>rows.offset?(offset-rows.offset)/rows.stride:0;
            for(;row<rows.rows;row++) {
                const uint64_t begin=(uint64_t)rows.offset+row*rows.stride;
                if(begin>=last)break;
                const uint64_t a=begin>offset?begin:offset,b=begin+rows.row_bytes<last?begin+rows.row_bytes:last;
                if(a<b) {*first=(uint32_t)a;*length=(uint32_t)(b-a);return true;}
            }
        }
        break;
    }
    return false;
}
GcnResidentMemoryStats gcn_resident_memory_stats(const GcnResidentMemory* m) {
    const GcnResidentMemoryStats empty={0};return m ? m->stats:empty;
}
GcnResidentError gcn_resident_memory_error(const GcnResidentMemory* m) {
    return m ? m->error:GCN_RESIDENT_INVALID;
}
