# SPDX-License-Identifier: MIT
# Optional per-item wall times at the existing render-worker timing boundary.
# Generate a copy; never edit the pinned Aurora checkout.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/render_worker.cpp" _worker_source)
set(_worker_trace [=[
#include <cstdio>
#include <cstdlib>
namespace {
struct GcnWorkerTrace {
  using Clock = std::chrono::steady_clock;
  FILE* file{};
  std::vector<char> buffer;
  GcnWorkerTrace() {
    const char* path = std::getenv("GCNLLR_RENDER_WORKER_TIMES");
    if (!path || !*path) return;
    file = std::fopen(path, "wx");
    if (!file) {
      std::fprintf(stderr, "[render-worker-times] could not create trace\n");
      return;
    }
    buffer.resize(4u * 1024u * 1024u);
    std::setvbuf(file, buffer.data(), _IOFBF, buffer.size());
    std::fprintf(file, "phase,item_type,frame_id,pass_index,begin_ns,end_ns\n");
  }
  ~GcnWorkerTrace() { if (file) std::fclose(file); }
  void record(const char* phase, unsigned type, uint64_t frame, uint32_t pass,
              Clock::time_point begin, Clock::time_point end) {
    if (!file) return;
    const auto ns = [](Clock::time_point t) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    };
    std::fprintf(file, "%s,%u,%llu,%u,%lld,%lld\n", phase, type,
        static_cast<unsigned long long>(frame), pass,
        static_cast<long long>(ns(begin)), static_cast<long long>(ns(end)));
  }
};
}
]=])
set(_worker_start "  while (true) {\n    if (g_idleHook) {\n      g_idleHook();\n    }")
set(_worker_end "      item->work();\n      const long ms = static_cast<long>(\n          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());")
foreach(_anchor IN ITEMS "${_worker_start}" "${_worker_end}")
  string(FIND "${_worker_source}" "${_anchor}" _worker_at)
  if(_worker_at EQUAL -1)
    message(FATAL_ERROR "Pinned Aurora render-worker timing boundary differs")
  endif()
endforeach()
string(REPLACE "namespace aurora::gfx::render_worker {"
  "${_worker_trace}\nnamespace aurora::gfx::render_worker {" _worker_source "${_worker_source}")
string(REPLACE "${_worker_start}" [=[  GcnWorkerTrace trace;
  while (true) {
    if (g_idleHook) {
      if (trace.file) {
        const auto begin = GcnWorkerTrace::Clock::now();
        g_idleHook();
        trace.record("idle", 0, 0, 0, begin, GcnWorkerTrace::Clock::now());
      } else g_idleHook();
    }]=] _worker_source "${_worker_source}")
string(REPLACE "${_worker_end}" [=[      item->work();
      const auto end = std::chrono::steady_clock::now();
      trace.record("work", static_cast<unsigned>(item->type), item->frameId, item->passIndex, start, end);
      const long ms = static_cast<long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());]=]
  _worker_source "${_worker_source}")
get_target_property(_worker_sources aurora_gx SOURCES)
list(FIND _worker_sources lib/gfx/render_worker.cpp _worker_index)
if(_worker_index EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora render-worker target source differs")
endif()
set(_worker_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/render_worker.cpp")
file(CONFIGURE OUTPUT "${_worker_path}" CONTENT "${_worker_source}" @ONLY)
list(REMOVE_ITEM _worker_sources lib/gfx/render_worker.cpp)
list(APPEND _worker_sources "${_worker_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_worker_sources}")
target_include_directories(aurora_gx PRIVATE "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")
