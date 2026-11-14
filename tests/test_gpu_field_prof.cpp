// SPDX-License-Identifier: MIT
#include "gpu_field_prof.hpp"
#include <cstdio>
#include <vector>
using namespace gcnllr::gpu_field;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void collect(void* user,const Interval& row){static_cast<std::vector<Interval>*>(user)->push_back(row);}
int main() {
  BusySum busy;busy.begin(100,200);busy.begin(120,150);busy.finish();busy.finish();
  busy.begin(250,300);busy.finish();CHECK(busy.complete() && busy.ns==150);
  std::vector<Interval> rows;Intervals fields;fields.reset(collect,&rows);
  auto first=fields.begin(true);fields.sample(first,10,20,5);fields.boundary(1);
  CHECK(rows.size()==1 && !rows.back().complete() && (rows.back().reasons&Initial));
  auto a=fields.begin(true),b=fields.begin(true);CHECK(a==b);
  fields.boundary(2);CHECK(rows.size()==1); // asynchronous results remain pending
  fields.sample(a,100,150,20);CHECK(rows.size()==1);
  fields.sample(b,180,220,10);CHECK(rows.size()==2);
  const auto full=rows.back();CHECK(full.complete() && full.packet_ns==90 && full.last-full.first==120 && full.pass_ns==30);
  fields.boundary(3);CHECK(!rows.back().complete() && (rows.back().reasons&NoPackets) && !rows.back().sampled);
  auto lost=fields.begin(true);fields.begin(false);fields.boundary(4);fields.failed(lost);
  CHECK((rows.back().reasons&(Dropped|Invalid))==(Dropped|Invalid) && !rows.back().complete());
  auto unknown=fields.begin(true);fields.unknown(2);fields.sample(unknown,230,250,10);fields.boundary(5);
  CHECK(rows.back().untracked==2 && (rows.back().reasons&Untracked));
  auto tail=fields.begin(true);fields.flush();CHECK((rows.back().reasons&(Tail|Pending))==(Tail|Pending));
  const auto n=rows.size();fields.sample(tail,300,350,10);CHECK(rows.size()==n); // discarded tail cannot become a full interval
  fields.reset(collect,&rows);
  for(unsigned i=0;i<Intervals::Capacity+1;i++){fields.begin(true);fields.boundary(i+1);}
  CHECK(rows.back().reasons&Overflow);fields.flush();
  fields.reset(collect,&rows);fields.boundary(9);fields.begin(true);fields.boundary(11);fields.flush();
  CHECK(rows.back().reasons&Sequence);
  fields.reset(collect,&rows);fields.boundary(1);fields.boundary(1);
  CHECK(rows.back().reasons&Sequence); // repeated presentation, never a zero-cost guest field
  fields.reset(collect,&rows);auto reset=fields.begin(true);fields.sample(reset,1000,1100,50);fields.boundary(1);
  CHECK(rows.back().reasons&Initial); // new title/generation requires another observed boundary
  std::puts("presentation interval aggregation PASS (no GPU)");return 0;
}
