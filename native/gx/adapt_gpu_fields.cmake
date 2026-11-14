# SPDX-License-Identifier: MIT
# Correlate the existing timestamp slots; do not edit the maintained donor.
macro(_gc_wii_prof_replace old new)
  string(FIND "${_gcnllr_gpu_prof}" "${old}" _field_anchor)
  if(_field_anchor EQUAL -1)
    message(FATAL_ERROR "Pinned Aurora presentation-profiler boundary differs: ${old}")
  endif()
  string(REPLACE "${old}" "${new}" _gcnllr_gpu_prof "${_gcnllr_gpu_prof}")
endmacro()
_gc_wii_prof_replace("#include \"gpu_prof.hpp\"" "#include \"gpu_prof.hpp\"\n#include \"${CMAKE_CURRENT_LIST_DIR}/gpu_field_prof.hpp\"")
_gc_wii_prof_replace("  uint32_t passCount = 0;" "  uint32_t passCount = 0;\n  uint64_t viField = 0;\n  bool viQueryLoss = false;")
_gc_wii_prof_replace("std::string_view log_kind(" "#include \"${CMAKE_CURRENT_LIST_DIR}/gpu_field_prof.inc\"\n\nstd::string_view log_kind(")
_gc_wii_prof_replace("  if (ts == nullptr) {" "  field_sample(slot, ts);\n  if (ts == nullptr) {")
_gc_wii_prof_replace("  if (!g_frameActive || g_zoneCount >= MaxZones) {"
  "  if (!g_frameActive || g_zoneCount >= MaxZones) {\n    if(g_frameActive)record_slot().viQueryLoss=true;")
_gc_wii_prof_replace("void initialize() {" "void initialize() {\n  field_initialize();")
_gc_wii_prof_replace("    Log.info(\"Timestamp queries unsupported; GPU profiling disabled\");"
  "    Log.info(\"Timestamp queries unsupported; GPU profiling disabled\");\n    field_available();")
_gc_wii_prof_replace("  Log.info(\"GPU profiling enabled ({} zones max)\", MaxZones);"
  "  Log.info(\"GPU profiling enabled ({} zones max)\", MaxZones);\n  field_available();")
_gc_wii_prof_replace("void shutdown() {" "void shutdown() {\n  field_shutdown();")
_gc_wii_prof_replace("  if (slot.state != SlotState::Free) {\n    g_frameActive = false;"
  "  if (slot.state != SlotState::Free) {\n    if(g_log)g_viFields.begin(false);\n    g_frameActive = false;")
_gc_wii_prof_replace("  slot.state = SlotState::Recording;"
  "  slot.viField=g_log?g_viFields.begin(true):0;slot.viQueryLoss=false;\n  slot.state = SlotState::Recording;")
_gc_wii_prof_replace("  if (slot.events.empty() && !g_timestampsEnabled) {"
  "  if (slot.events.empty() && !g_timestampsEnabled) {\n    if(slot.viField)g_viFields.failed(slot.viField);")
_gc_wii_prof_replace("    } else if (state != SlotState::Failed) {"
  "    } else if (state == SlotState::Failed) {\n      if(slot.viField)g_viFields.failed(slot.viField);\n    } else {")
string(APPEND _gcnllr_gpu_prof [=[

extern "C" int gc_wii_gpu_field_enabled() {
  return aurora::webgpu::gpu_prof::g_viEnabled.load(std::memory_order_acquire);
}
extern "C" void gc_wii_gpu_field_boundary(uint64_t presentation) {
  aurora::webgpu::gpu_prof::field_boundary(presentation);
}
extern "C" void gc_wii_gpu_field_untracked() {
  if(gc_wii_gpu_field_enabled())aurora::webgpu::gpu_prof::g_viUntracked.fetch_add(1,std::memory_order_relaxed);
}
]=])
