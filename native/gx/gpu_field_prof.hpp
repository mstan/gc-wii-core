// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Private profiler bridge, not a native graphics/CPU ABI change.
extern "C" int gc_wii_gpu_field_enabled();
extern "C" void gc_wii_gpu_field_boundary(uint64_t presentation);
extern "C" void gc_wii_gpu_field_untracked();

namespace gcnllr::gpu_field {
enum Reason : uint32_t {
  Initial=1, Tail=2, Dropped=4, Invalid=8, Overflow=16,
  Untracked=32, Pending=64, NoPackets=128, QueryLoss=256, Sequence=512,
};
struct BusySum {
  uint64_t ns=0, last=0;
  uint32_t depth=0;
  bool valid=true;
  void begin(uint64_t first,uint64_t end) {
    if(!first || end<=first)valid=false;
    if(depth++==0 && first && end>first) {
      if((last && first<last) || end-first>std::numeric_limits<uint64_t>::max()-ns)valid=false;
      else ns+=end-first;
      last=end;
    }
  }
  void finish() {if(!depth)valid=false;else --depth;}
  bool complete() const {return valid && !depth;}
};
struct Interval {
  uint64_t ticket=0, presentation=0, expected=0, sampled=0, pending=0, dropped=0, failed=0;
  uint64_t first=0, last=0, packet_ns=0, pass_ns=0, untracked=0;
  uint32_t reasons=0;
  bool closed=false;
  bool complete() const {
    return closed && !reasons && !pending && expected && sampled==expected;
  }
};
class Intervals {
public:
  // Four existing timestamp readback slots plus the open presentation interval.
  static constexpr std::size_t Capacity=5;
  using Emit=void(*)(void*,const Interval&);
  void reset(Emit emit,void* user=nullptr) {
    rows={};next=1;last_presentation=0;callback=emit;context=user;
  }
  uint64_t begin(bool recorded) {
    auto& r=current();++r.expected;
    if(recorded)++r.pending;else{++r.dropped;r.reasons|=Dropped;}
    return r.ticket;
  }
  void sample(uint64_t ticket,uint64_t first,uint64_t last,uint64_t pass_ns,uint32_t reasons=0) {
    auto* r=find(ticket);if(!r)return;
    if(!r->pending){r->reasons|=Invalid;return;}--r->pending;
    r->reasons|=reasons;
    if(!first || last<=first || (r->last && first<r->last) ||
       last-first>std::numeric_limits<uint64_t>::max()-r->packet_ns ||
       pass_ns>std::numeric_limits<uint64_t>::max()-r->pass_ns) {
      ++r->failed;r->reasons|=Invalid;
    } else {
      if(!r->first)r->first=first;
      r->last=last;
      r->packet_ns+=last-first;r->pass_ns+=pass_ns;++r->sampled;
    }
    if(r->closed && !r->pending)emit(*r);
  }
  void failed(uint64_t ticket) {sample(ticket,0,0,0,Invalid);}
  void unknown(uint64_t count) {if(count){auto& r=current();r.untracked+=count;r.reasons|=Untracked;}}
  void boundary(uint64_t presentation) {
    auto& r=current();r.presentation=presentation;r.closed=true;
    if(!presentation || (last_presentation && presentation!=last_presentation+1))r.reasons|=Sequence;
    last_presentation=presentation;if(!r.pending)emit(r);++next;
  }
  void flush() {
    for(auto& r:rows)if(r.ticket){if(!r.closed)r.reasons|=Tail;if(r.pending)r.reasons|=Pending;emit(r);}
  }
private:
  std::array<Interval,Capacity> rows{};
  uint64_t next=1,last_presentation=0;
  Emit callback=nullptr;void* context=nullptr;
  Interval* find(uint64_t ticket) {for(auto& r:rows)if(r.ticket==ticket)return &r;return nullptr;}
  Interval& current() {
    if(auto* r=find(next))return *r;
    Interval* available=nullptr;
    for(auto& r:rows)if(!r.ticket){available=&r;break;}
    if(!available){
      available=&rows[0];for(auto& r:rows)if(r.ticket<available->ticket)available=&r;
      available->reasons|=Overflow;if(available->pending)available->reasons|=Pending;emit(*available);
    }
    *available={};available->ticket=next;if(next==1)available->reasons|=Initial;return *available;
  }
  void emit(Interval& row) {
    if(!row.expected)row.reasons|=NoPackets;
    if(callback)callback(context,row);
    row={};
  }
};
} // namespace gcnllr::gpu_field
