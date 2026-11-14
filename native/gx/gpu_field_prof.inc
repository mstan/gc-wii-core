// SPDX-License-Identifier: MIT
// Included in the existing Aurora timestamp recorder's private namespace.
static_assert(gcnllr::gpu_field::Intervals::Capacity==RingDepth+1);
gcnllr::gpu_field::Intervals g_viFields;
std::atomic<bool> g_viEnabled{false};
std::atomic<uint64_t> g_viUntracked{0};
uint64_t g_viGeneration=0;

void field_output(void*,const gcnllr::gpu_field::Interval& row) {
  std::fprintf(stderr,"[gpu-presentation] {\"generation\":%llu,\"interval\":%llu,\"presentation\":%llu,"
      "\"packet_set_complete\":%s,\"guest_vi_correlated\":false,\"reasons\":%u,\"expected_packets\":%llu,"
      "\"sampled_packets\":%llu,\"pending_packets\":%llu,\"dropped_packets\":%llu,"
      "\"failed_packets\":%llu,\"untracked_submissions\":%llu,",
      (unsigned long long)g_viGeneration,(unsigned long long)row.ticket,(unsigned long long)row.presentation,
      row.complete()?"true":"false",row.reasons,(unsigned long long)row.expected,
      (unsigned long long)row.sampled,(unsigned long long)row.pending,(unsigned long long)row.dropped,
      (unsigned long long)row.failed,(unsigned long long)row.untracked);
  if(row.sampled)std::fprintf(stderr,"\"packet_busy_ns\":%llu,\"top_level_pass_ns\":%llu,\"queue_span_ns\":%llu}\n",
      (unsigned long long)row.packet_ns,(unsigned long long)row.pass_ns,(unsigned long long)(row.last-row.first));
  else std::fputs("\"packet_busy_ns\":null,\"top_level_pass_ns\":null,\"queue_span_ns\":null}\n",stderr);
}
void field_initialize() {
  g_viEnabled.store(false,std::memory_order_release);g_viUntracked=0;
  g_viFields.reset(field_output);++g_viGeneration;
}
void field_available() {
  const char* value=std::getenv("DOL_AURORA_GPU_PROF");
  const bool requested=value && value[0]=='1';
  if(requested)std::fprintf(stderr,"[gpu-presentation-session] generation=%llu available=%u scope=timestamped-packet-work "
      "unit=VI-presentation-interval guest-VI-correlated=0 reason-mask=initial:1,tail:2,dropped:4,invalid:8,overflow:16,"
      "untracked:32,pending:64,no-packets:128,query-loss:256,sequence:512\n",
      (unsigned long long)g_viGeneration,unsigned(g_enabled));
  g_viEnabled.store(requested && g_enabled,std::memory_order_release);
}
void field_sample(const Slot& slot,const uint64_t* timestamps) {
  if(!slot.viField)return;
  gcnllr::gpu_field::BusySum busy;
  if(timestamps)for(const auto& event:slot.events) {
    if(event.kind==EventKind::End)busy.finish();
    else busy.begin(timestamps[event.query],timestamps[event.query+1]);
  }
  uint32_t reasons=slot.viQueryLoss?gcnllr::gpu_field::QueryLoss:0;
  if(!busy.complete())reasons|=gcnllr::gpu_field::QueryLoss;
  const uint64_t begin=timestamps?timestamps[FrameBeginQuery]:0,end=timestamps?timestamps[FrameEndQuery]:0;
  if(end>begin && busy.ns>end-begin)reasons|=gcnllr::gpu_field::Invalid;
  g_viFields.sample(slot.viField,begin,end,busy.ns,reasons);
}
void field_boundary(uint64_t presentation) {
  if(!g_viEnabled.load(std::memory_order_acquire))return;
  g_viFields.unknown(g_viUntracked.exchange(0,std::memory_order_acq_rel));g_viFields.boundary(presentation);
}
void field_shutdown() {
  g_viEnabled.store(false,std::memory_order_release);
  if(g_log){g_viFields.unknown(g_viUntracked.exchange(0));g_viFields.flush();}
}
