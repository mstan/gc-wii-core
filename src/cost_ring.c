/* SPDX-License-Identifier: MIT */
#include "debug/cost_ring.h"
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

bool gcn_cost_sampling;
static GcnCostField ring[GCN_COST_RING_CAPACITY],current;
static u64 written,field_cycles,field_start,turn_cycles,sample_start,total_turns;
static u64 field_ticks,clock_overhead,turn_end_ticks;
static bool use_tsc;
static u32 turn_thread,last_thread_index;
static unsigned bucket;
static unsigned owner_bucket;
static u64 owner_start;
#ifdef _WIN32
static u64 frequency;
#endif

static u64 now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const u64 value=(u64)counter.QuadPart;
    return value/frequency*1000000000ull+value%frequency*1000000000ull/frequency;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC,&value);
    return (u64)value.tv_sec*1000000000ull+(u64)value.tv_nsec;
#endif
}

static bool invariant_tsc(void) {
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
    int registers[4];
    __cpuid(registers,0x80000000);
    if((u32)registers[0]<0x80000007u)return false;
    __cpuid(registers,0x80000007);
    return (registers[3]&(1u<<8))!=0;
#else
    unsigned a,b,c,d;
    return __get_cpuid(0x80000007u,&a,&b,&c,&d) && (d&(1u<<8));
#endif
#else
    return false;
#endif
}

/* TSC reads are restricted to x86-64 hosts advertising invariant TSC. They
 * measure sampled host regions, with minimum clock-pair cost removed and a
 * fresh tick-to-wall conversion per field. No guest clock uses these values.
 * Every other host retains the monotonic-clock implementation. */
static u64 ticks_now(void) {
#if defined(__x86_64__) || defined(_M_X64)
    if(use_tsc)return __rdtsc();
#endif
    return now_ns();
}

void gcn_cost_init(u64 cycles) {
#ifdef _WIN32
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    frequency=(u64)value.QuadPart;
#endif
    memset(ring,0,sizeof ring);
    memset(&current,0,sizeof current);
    written=0;
    total_turns=0;
    field_cycles=cycles;
    use_tsc=invariant_tsc();
    clock_overhead=UINT64_MAX;
    for(unsigned i=0;i<512;++i) {
        const u64 before=ticks_now();
        const u64 elapsed=ticks_now()-before;
        if(elapsed<clock_overhead)clock_overhead=elapsed;
    }
    field_start=now_ns();
    field_ticks=ticks_now();
    turn_end_ticks=field_ticks;
    owner_start=field_ticks;owner_bucket=GCN_COST_OWNER_HOST;
    gcn_cost_sampling=false;
    last_thread_index=0;
}

static unsigned boundary_at(unsigned next,u64 now) {
    const unsigned previous=bucket;
    const u64 elapsed=now-sample_start;
    if(elapsed>clock_overhead)current.sampled_ns[bucket]+=elapsed-clock_overhead;
    sample_start=now;
    bucket=next;
    return previous;
}

unsigned gcn_cost_boundary(unsigned next) {
    return boundary_at(next,ticks_now());
}

static void owner_at(unsigned next,u64 now) {
    current.measured_ns[owner_bucket]+=now-owner_start;
    owner_start=now;owner_bucket=next;
}

void gcn_cost_owner_boundary(GcnCostOwner next) {
    owner_at((unsigned)next,ticks_now());
}

void gcn_cost_begin_turn(u64 cycles,u32 thread) {
    turn_cycles=cycles;
    turn_thread=thread;
    ++current.turns;
    gcn_cost_sampling=++total_turns%257u==1u;
    if(gcn_cost_sampling) {
        ++current.sampled_turns;
        bucket=GCN_COST_HOST;
        /* Include control polling, input publication and field bookkeeping
         * between turns. Starting the clock here omitted that host work. */
        sample_start=turn_end_ticks;
    }
}

static void charge_thread(u64 cycles) {
    const u64 elapsed=cycles-turn_cycles;
    if(!elapsed)return;
    if(!turn_thread) {
        current.no_thread_cycles+=elapsed;
        return;
    }
    if(last_thread_index>=current.thread_count || current.threads[last_thread_index].thread!=turn_thread) {
        u32 index=0;
        while(index<current.thread_count && current.threads[index].thread!=turn_thread)++index;
        if(index==GCN_COST_THREAD_CAPACITY) {
            current.other_thread_cycles+=elapsed;
            return;
        }
        if(index==current.thread_count) {
            current.threads[index].thread=turn_thread;
            ++current.thread_count;
        }
        last_thread_index=index;
    }
    current.threads[last_thread_index].cycles+=elapsed;
}

void gcn_cost_thread_boundary(u64 cycles,u32 thread) {
    charge_thread(cycles);
    turn_cycles=cycles;turn_thread=thread;
}

void gcn_cost_end_turn(u64 cycles) {
    /* One clock per owner turn retains the preceding turn's end even when
     * that turn was not sampled. Nested device scopes remain sampled. */
    const u64 now=ticks_now();
    if(gcn_cost_sampling)boundary_at(GCN_COST_HOST,now);
    owner_at(GCN_COST_OWNER_HOST,now);
    turn_end_ticks=now;
    gcn_cost_sampling=false;
    charge_thread(cycles);
}

void gcn_cost_finish_field(u64 field,u64 cycles) {
    const u64 now=now_ns();
    const u64 ticks=ticks_now();
    owner_at(GCN_COST_OWNER_HOST,ticks);
    current.field=field;
    current.guest_cycles=cycles-field_cycles;
    current.wall_ns=now-field_start;
    const u64 elapsed_ticks=ticks-field_ticks;
    if(elapsed_ticks) {
        /* Fixed-point conversion avoids changing host floating-point flags
         * while the guest's FP implementation is active. Reduce precision
         * defensively when a field spans a very long host pause. */
        unsigned shift=16;
        while(shift && current.wall_ns>(UINT64_MAX>>shift))--shift;
        const u64 scale=(current.wall_ns<<shift)/elapsed_ticks;
        const u64 mask=(1ull<<shift)-1u;
        for(unsigned i=0;i<GCN_COST_BUCKET_COUNT;++i) {
            const u64 value=current.sampled_ns[i];
            current.sampled_ns[i]=(value>>shift)*scale+((value&mask)*scale>>shift);
        }
        for(unsigned i=0;i<GCN_COST_OWNER_COUNT;++i) {
            const u64 value=current.measured_ns[i];
            current.measured_ns[i]=(value>>shift)*scale+((value&mask)*scale>>shift);
        }
    }
    ring[written++&(GCN_COST_RING_CAPACITY-1u)]=current;
    memset(&current,0,sizeof current);
    field_cycles=cycles;
    field_start=now;
    field_ticks=ticks;
    turn_end_ticks=ticks;
}

void gcn_cost_write_json(FILE* output,u32 limit) {
    static const char* names[GCN_COST_BUCKET_COUNT]={
        "host","dispatch","guest","invalidation","fifo","time","video",
        "dsp","ai","di","ios","ipc","graphics"
    };
    u64 count=written<GCN_COST_RING_CAPACITY?written:GCN_COST_RING_CAPACITY;
    if(count>limit)count=limit;
    fprintf(output,"{\"clock\":\"%s\",\"clock_pair_overhead_ticks\":%llu,\"sampling_period\":257,\"written\":%llu,\"fields\":[",
            use_tsc?"invariant-tsc":"monotonic",(unsigned long long)clock_overhead,(unsigned long long)written);
    for(u64 sequence=written-count;sequence<written;++sequence) {
        const GcnCostField* row=&ring[sequence&(GCN_COST_RING_CAPACITY-1u)];
        fprintf(output,"%s{\"field\":%llu,\"guest_cycles\":%llu,\"wall_ns\":%llu,\"turns\":%llu,\"sampled_turns\":%llu,\"sampled_exclusive_ns\":{",
                sequence==written-count?"":",",(unsigned long long)row->field,
                (unsigned long long)row->guest_cycles,(unsigned long long)row->wall_ns,
                (unsigned long long)row->turns,(unsigned long long)row->sampled_turns);
        for(unsigned i=0;i<GCN_COST_BUCKET_COUNT;++i)
            fprintf(output,"%s\"%s\":%llu",i?",":"",names[i],(unsigned long long)row->sampled_ns[i]);
        fprintf(output,"},\"measured_exclusive_ns\":{\"host\":%llu,\"execution\":%llu,\"devices\":%llu,\"graphics\":%llu},\"no_thread_cycles\":%llu,\"other_thread_cycles\":%llu,\"threads\":[",
                (unsigned long long)row->measured_ns[GCN_COST_OWNER_HOST],
                (unsigned long long)row->measured_ns[GCN_COST_OWNER_EXECUTION],
                (unsigned long long)row->measured_ns[GCN_COST_OWNER_DEVICES],
                (unsigned long long)row->measured_ns[GCN_COST_OWNER_GRAPHICS],
                (unsigned long long)row->no_thread_cycles,(unsigned long long)row->other_thread_cycles);
        for(u32 i=0;i<row->thread_count;++i)
            fprintf(output,"%s{\"thread\":%u,\"cycles\":%llu}",i?",":"",row->threads[i].thread,(unsigned long long)row->threads[i].cycles);
        fputs("]}",output);
    }
    fputs("]}",output);
}
