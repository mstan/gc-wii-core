# SPDX-License-Identifier: MIT
# Adapt a generated copy; preserve the pinned dependency checkout verbatim.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/window.cpp" _gcnllr_window)
set(_gcnllr_hidden "if ((flags & SDL_WINDOW_HIDDEN) != 0u) {")
string(FIND "${_gcnllr_window}" "${_gcnllr_hidden}" _gcnllr_offset)
if(_gcnllr_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native Aurora hidden-window adaptation differs")
endif()
string(REPLACE "${_gcnllr_hidden}"
  "if ((flags & SDL_WINDOW_HIDDEN) != 0u && !SDL_GetHintBoolean(\"GCNLLR_RENDER_HIDDEN\", false)) {"
  _gcnllr_window "${_gcnllr_window}")
string(REPLACE "void show_window() {"
  "void show_window() {\n  if (SDL_GetHintBoolean(\"GCNLLR_RENDER_HIDDEN\", false)) return;"
  _gcnllr_window "${_gcnllr_window}")
string(REPLACE "SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, \"1\")"
  "(SDL_GetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN) != nullptr || SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, \"1\"))"
  _gcnllr_window "${_gcnllr_window}")
string(REPLACE "if (SDL_WaitEvent(&event)) {" "if (SDL_WaitEventTimeout(&event, 16)) {"
  _gcnllr_window "${_gcnllr_window}")
set(_gcnllr_window_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/window.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_window_path}" CONTENT "${_gcnllr_window}" @ONLY)
get_target_property(_gcnllr_core_sources aurora_core SOURCES)
list(REMOVE_ITEM _gcnllr_core_sources lib/window.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_window_path}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_gcnllr_core_sources}")
target_include_directories(aurora_core PRIVATE "${GXRUNTIME_DIR}/graphics/aurora/lib")

# Dawn's Windows loader requires absolute runtime search directories. A DLL
# service cannot assume that the caller's executable sits beside Dawn/DXC.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu/gpu.cpp" _gcnllr_gpu)
set(_gcnllr_display_viewport "  uint32_t viewport_width = surface_width;")
string(FIND "${_gcnllr_gpu}" "${_gcnllr_display_viewport}" _gcnllr_display_offset)
if(_gcnllr_display_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native presentation viewport differs")
endif()
string(REPLACE "${_gcnllr_display_viewport}"
  "  gcnllr::display_aspect::apply_dimensions(content_width, content_height);\n${_gcnllr_display_viewport}"
  _gcnllr_gpu "${_gcnllr_gpu}")
string(PREPEND _gcnllr_gpu "#include \"${GC_WII_GX_ROOT}/native/gx/display_aspect.hpp\"\n")
string(PREPEND _gcnllr_gpu "#include <SDL3/SDL_hints.h>\n")
set(_gcnllr_power ".powerPreference = wgpu::PowerPreference::HighPerformance,")
string(FIND "${_gcnllr_gpu}" "${_gcnllr_power}" _gcnllr_power_offset)
if(_gcnllr_power_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GPU adapter preference differs")
endif()
# Host adapter selection is independent of HLE/LLE selection. Keep the normal
# preference unless an explicit per-process diagnostic asks for a low-power
# adapter; this never changes another application's GPU configuration.
string(REPLACE "${_gcnllr_power}"
  ".powerPreference = SDL_GetHintBoolean(\"GCNLLR_GPU_LOW_POWER\", false) ? wgpu::PowerPreference::LowPower : wgpu::PowerPreference::HighPerformance,"
  _gcnllr_gpu "${_gcnllr_gpu}")
set(_gcnllr_instance "dawn::native::DawnInstanceDescriptor dawnInstanceDescriptor;")
string(REPLACE "${_gcnllr_instance}"
  "${_gcnllr_instance}\n    const char* nativeRuntimePaths[] = {SDL_GetHint(\"GCNLLR_NATIVE_RUNTIME_DIR\"), SDL_GetHint(\"GCNLLR_SYSTEM_RUNTIME_DIR\")};\n    Log.info(\"Native GPU runtime paths: {} | {}\", nativeRuntimePaths[0] ? nativeRuntimePaths[0] : \"unset\", nativeRuntimePaths[1] ? nativeRuntimePaths[1] : \"unset\");\n    if (nativeRuntimePaths[0] && nativeRuntimePaths[1]) {\n      dawnInstanceDescriptor.additionalRuntimeSearchPathsCount = 2;\n      dawnInstanceDescriptor.additionalRuntimeSearchPaths = nativeRuntimePaths;\n    }"
  _gcnllr_gpu "${_gcnllr_gpu}")
set(_gcnllr_gpu_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gpu.cpp")
string(PREPEND _gcnllr_gpu "#include <algorithm>\n#include <cstring>\n")
string(REPLACE "constexpr std::array enableToggles{" "std::vector<const char*> enableToggles{"
  _gcnllr_gpu "${_gcnllr_gpu}")
string(REPLACE "wgpu::DawnTogglesDescriptor togglesDescriptor(" [=[if (SDL_GetHintBoolean("GCNLLR_VALIDATE_GPU", false)) {
      enableToggles.erase(std::remove_if(enableToggles.begin(), enableToggles.end(), [](const char* toggle) {
        return std::strcmp(toggle, "skip_validation") == 0 || std::strcmp(toggle, "disable_robustness") == 0;
      }), enableToggles.end());
    }
    wgpu::DawnTogglesDescriptor togglesDescriptor(]=]
  _gcnllr_gpu "${_gcnllr_gpu}")
file(CONFIGURE OUTPUT "${_gcnllr_gpu_path}" CONTENT "${_gcnllr_gpu}" @ONLY)
get_target_property(_gcnllr_core_sources aurora_core SOURCES)
list(FIND _gcnllr_core_sources lib/webgpu/gpu.cpp _gcnllr_gpu_source_index)
if(_gcnllr_gpu_source_index EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora GPU source target differs")
endif()
list(REMOVE_ITEM _gcnllr_core_sources lib/webgpu/gpu.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_gpu_path}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_gcnllr_core_sources}")
target_include_directories(aurora_core PRIVATE "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu")

# Pass timestamp descriptors own QuerySet handles. Release the descriptor ring
# during normal shutdown, rather than from a function-static destructor under
# the Windows loader lock after the shader compiler has torn down its TLS.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu/gpu_prof.cpp" _gcnllr_gpu_prof)
set(_profiler_ring "std::array<Slot, RingDepth> g_slots;")
set(_profiler_shutdown "void shutdown() {\n  g_querySet = {};")
set(_profiler_local [=[  static std::array<wgpu::PassTimestampWrites, 4> writes;
  static size_t writesIndex = 0;
  auto& out = writes[writesIndex++ % writes.size()];]=])
foreach(_profiler_anchor IN ITEMS "${_profiler_ring}" "${_profiler_shutdown}" "${_profiler_local}")
  string(FIND "${_gcnllr_gpu_prof}" "${_profiler_anchor}" _profiler_offset)
  if(_profiler_offset EQUAL -1)
    message(FATAL_ERROR "Pinned Aurora GPU profiler lifetime boundary differs")
  endif()
endforeach()
string(REPLACE "${_profiler_ring}"
  "${_profiler_ring}\nstd::array<wgpu::PassTimestampWrites, 4> g_passWrites;\nsize_t g_passWritesIndex = 0;"
  _gcnllr_gpu_prof "${_gcnllr_gpu_prof}")
string(REPLACE "${_profiler_shutdown}"
  "void shutdown() {\n  g_passWrites = {};\n  g_passWritesIndex = 0;\n  g_querySet = {};"
  _gcnllr_gpu_prof "${_gcnllr_gpu_prof}")
string(REPLACE "${_profiler_local}"
  "  auto& out = g_passWrites[g_passWritesIndex++ % g_passWrites.size()];"
  _gcnllr_gpu_prof "${_gcnllr_gpu_prof}")
string(REPLACE "#include \"../internal.hpp\"" "#include \"internal.hpp\""
  _gcnllr_gpu_prof "${_gcnllr_gpu_prof}")
set(_profiler_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gpu_prof.cpp")
file(CONFIGURE OUTPUT "${_profiler_path}" CONTENT "${_gcnllr_gpu_prof}" @ONLY)
get_target_property(_profiler_sources aurora_core SOURCES)
list(FIND _profiler_sources lib/webgpu/gpu_prof.cpp _profiler_source_index)
if(_profiler_source_index EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora GPU profiler target source differs")
endif()
list(REMOVE_ITEM _profiler_sources lib/webgpu/gpu_prof.cpp)
list(APPEND _profiler_sources "${_profiler_path}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_profiler_sources}")

# Release live-device owners during shutdown, before Windows' loader lock is
# held by FreeLibrary. The unmodified donor executable never tested DLL unload.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gx/gx.cpp" _gcnllr_gx)
set(_gcnllr_shutdown "void shutdown() noexcept {\n  // TODO we should probably store this all in g_state.gx instead")
string(FIND "${_gcnllr_gx}" "${_gcnllr_shutdown}" _gcnllr_shutdown_offset)
if(_gcnllr_shutdown_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GX shutdown adaptation differs")
endif()
string(REPLACE "${_gcnllr_shutdown}"
  "void shutdown() noexcept {\n  sPipelineLayout = {};\n  g_emptyTextureBindGroup = {};\n  sEmptyTextureView = {};\n  sEmptyTexture = {};\n  sEmptySampler = {};"
  _gcnllr_gx "${_gcnllr_gx}")
set(_gcnllr_gx_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gx.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_gx_path}" CONTENT "${_gcnllr_gx}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gx/gx.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_gx_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_gx_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gx")

# Specialized shaders may compile asynchronously only when a ready fallback
# can draw the frame. The donor also queued that fallback asynchronously,
# losing first-use draws. Wait for the fallback (or the specialized shader on
# a backend without fallback support) instead of omitting guest geometry.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/pipeline_cache.cpp" _gcnllr_pipeline)
set(_gcnllr_hash "const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));")
string(FIND "${_gcnllr_pipeline}" "${_gcnllr_hash}" _gcnllr_pipeline_offset)
if(_gcnllr_pipeline_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GX pipeline adaptation differs")
endif()
string(REPLACE "${_gcnllr_hash}"
  "if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {\n    if (priority == PipelinePriority::Normal) priority = PipelinePriority::Blocking;\n  }\n#ifdef AURORA_ENABLE_GXCORE\n  if constexpr (std::is_same_v<PipelineConfig, gxcore::PipelineConfig>) {\n    if (priority == PipelinePriority::Normal && (config.depthOnly == 2u || ubershader_mode() == 0))\n      priority = PipelinePriority::Blocking;\n  }\n#endif\n  ${_gcnllr_hash}"
  _gcnllr_pipeline "${_gcnllr_pipeline}")
string(PREPEND _gcnllr_pipeline "#include <type_traits>\n")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_resident_pipeline.cmake")
set(_gcnllr_pipeline_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/pipeline_cache.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_pipeline_path}" CONTENT "${_gcnllr_pipeline}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/pipeline_cache.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_pipeline_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_pipeline_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# Dropping an unparsed display list loses required geometry. Preserve the
# diagnostic and reject the command instead of clearing its failure state.
file(READ "${GXRUNTIME_DIR}/graphics/frontend/src/retail_gx_frontend.cpp" _gcnllr_frontend)
set(_gcnllr_drop "last_error_ = nullptr;\n        ++display_lists_skipped_;")
string(FIND "${_gcnllr_frontend}" "${_gcnllr_drop}" _gcnllr_frontend_offset)
if(_gcnllr_frontend_offset EQUAL -1)
  message(FATAL_ERROR "Pinned display-list failure adaptation differs")
endif()
string(REPLACE "${_gcnllr_drop}"
  "return fail_parse(\"Display list did not parse completely\", cmd, pos, address, size, static_cast<uint32_t>(dl_consumed));"
  _gcnllr_frontend "${_gcnllr_frontend}")
# Retail GXSetTexCopyDst encodes Z16 as real copy format 0xB. Treating it
# as the default Z24X8 doubles the MEM1 write footprint and corrupts callers.
set(_gcnllr_z16 "case 0x3:\n      return 0x13; // GX_TF_Z16")
string(FIND "${_gcnllr_frontend}" "${_gcnllr_z16}" _gcnllr_z16_offset)
if(_gcnllr_z16_offset EQUAL -1)
  message(FATAL_ERROR "Pinned GX Z16 copy decoder adaptation differs")
endif()
string(REPLACE "${_gcnllr_z16}"
  "case 0x3:\n    case 0xB: // GXSetTexCopyDst(GX_TF_Z16)\n      return 0x13; // GX_TF_Z16"
  _gcnllr_frontend "${_gcnllr_frontend}")
# Color conversion is requested only when both trigger bits are set. Without
# auto-conversion the same tiled destination carries raw selected channels.
set(_gcnllr_intensity_decode "const bool intensity = bp_get(value, 1u, 15u) != 0u;")
string(FIND "${_gcnllr_frontend}" "${_gcnllr_intensity_decode}" _gcnllr_intensity_offset)
if(_gcnllr_intensity_offset EQUAL -1)
  message(FATAL_ERROR "Pinned copy intensity decoder differs")
endif()
string(REPLACE "${_gcnllr_intensity_decode}"
  "const bool intensity = bp_get(value, 1u, 15u) != 0u && bp_get(value, 1u, 16u) != 0u;"
  _gcnllr_frontend "${_gcnllr_frontend}")
set(_gcnllr_frontend_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/retail_gx_frontend.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_frontend_path}" CONTENT "${_gcnllr_frontend}" @ONLY)
get_target_property(_gcnllr_frontend_sources gxruntime_retail_gx_frontend SOURCES)
list(REMOVE_ITEM _gcnllr_frontend_sources src/retail_gx_frontend.cpp)
list(APPEND _gcnllr_frontend_sources "${_gcnllr_frontend_path}")
set_property(TARGET gxruntime_retail_gx_frontend PROPERTY SOURCES "${_gcnllr_frontend_sources}")

# Materialize supported native EFB copies into the machine's tiled MEM1 ABI.
# The donor only encoded I8/RGB565 because its own CPU path bypassed other
# consumers. Our actual retail CPU can observe every copied buffer.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/gxcore_draw.cpp" _gcnllr_draw)
# Dawn platform packages can declare these fields in different orders.
# Named assignments retain the descriptor values without an order-sensitive initializer.
set(_gcnllr_vertex_layout [=[  const wgpu::VertexBufferLayout vertexLayout{
      .arrayStride = gxc::kVertexStrideBytes,
      .stepMode = wgpu::VertexStepMode::Vertex,
      .attributeCount = attributes.size(),
      .attributes = attributes.data(),
  };]=])
string(FIND "${_gcnllr_draw}" "${_gcnllr_vertex_layout}" _gcnllr_vertex_layout_offset)
if(_gcnllr_vertex_layout_offset EQUAL -1)
  message(FATAL_ERROR "Pinned Dawn vertex-buffer layout differs")
endif()
string(REPLACE "${_gcnllr_vertex_layout}" [=[  wgpu::VertexBufferLayout vertexLayout{};
  vertexLayout.arrayStride = gxc::kVertexStrideBytes;
  vertexLayout.stepMode = wgpu::VertexStepMode::Vertex;
  vertexLayout.attributeCount = attributes.size();
  vertexLayout.attributes = attributes.data();]=] _gcnllr_draw "${_gcnllr_draw}")
set(_gcnllr_copy_guard "it == g_efbCopyTextures.end() || !it->second.handle ||\n      (cmd.format != 1u && cmd.format != 4u)")
string(FIND "${_gcnllr_draw}" "${_gcnllr_copy_guard}" _gcnllr_copy_offset)
if(_gcnllr_copy_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native texture-copy readback adaptation differs")
endif()
string(REPLACE "${_gcnllr_copy_guard}" "it == g_efbCopyTextures.end() || !it->second.handle"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "gxc::encode_efb_copy(cmd.format" "gcnllr::encode_native_copy(cmd.format"
  _gcnllr_draw "${_gcnllr_draw}")
# An unavailable dirty epoch cannot qualify an unchanged EFB shadow. Our
# machine materializes the copy in MEM1; decode/hash those actual bytes.
set(_gcnllr_shadow_guard "!efbIt->second.memory_epoch_valid ||")
string(FIND "${_gcnllr_draw}" "${_gcnllr_shadow_guard}" _gcnllr_shadow_offset)
if(_gcnllr_shadow_offset EQUAL -1)
  message(FATAL_ERROR "Pinned EFB shadow coherence guard differs")
endif()
string(REPLACE "${_gcnllr_shadow_guard}" "efbIt->second.memory_epoch_valid &&"
  _gcnllr_draw "${_gcnllr_draw}")
# Retain the donor's GPU copy after actual CPU materialization. Hash the bytes
# produced by the copy, not a later RAM snapshot: native CPU stores may resume
# as soon as the machine acknowledges publication. Verify actual RAM and the
# sampled layout before reuse; unknown/strided views still decode real MEM1.
set(_gcnllr_binding "bool memory_epoch_valid = false;\n};\nabsl::flat_hash_map<uint32_t, EfbCopyBinding> g_efbCopyTextures;")
string(FIND "${_gcnllr_draw}" "${_gcnllr_binding}" _gcnllr_binding_offset)
if(_gcnllr_binding_offset EQUAL -1)
  message(FATAL_ERROR "Pinned EFB copy binding differs")
endif()
string(REPLACE "${_gcnllr_binding}" [=[bool memory_epoch_valid = false;
  uint64_t materialized_hash = 0;
  uint32_t materialized_size = 0;
  uint32_t width = 0, height = 0, format = 0;
};
absl::flat_hash_map<uint32_t, EfbCopyBinding> g_efbCopyTextures;
uint64_t g_verifiedCopyHits = 0, g_verifiedCopyMisses = 0;]=]
  _gcnllr_draw "${_gcnllr_draw}")
set(_gcnllr_epoch_test [=[const bool memoryUnchanged =
          efbIt->second.memory_epoch_valid &&
          (efb_copy_epoch(address, efbIt->second.byte_size, &memoryEpoch) &&
           memoryEpoch == efbIt->second.memory_epoch);]=])
string(FIND "${_gcnllr_draw}" "${_gcnllr_epoch_test}" _gcnllr_epoch_test_offset)
if(_gcnllr_epoch_test_offset EQUAL -1)
  message(FATAL_ERROR "Pinned EFB copy sampling guard differs")
endif()
string(REPLACE "${_gcnllr_epoch_test}" [=[const auto& copy = efbIt->second;
      bool memoryUnchanged;
      if (copy.materialized_size != 0u) {
        // A bound pointer can outlive another GPU producer at this address.
        // Observe ownership before reading the expected copy's actual bytes.
        if (g_textureDirtyEpochObserver != nullptr && data != nullptr) {
          uint64_t observedEpoch;
          g_textureDirtyEpochObserver(address, std::min(available,copy.materialized_size), &observedEpoch);
        }
        memoryUnchanged = data != nullptr && available >= copy.materialized_size &&
            width == copy.width && height == copy.height && format == copy.format &&
            XXH3_64bits(data, copy.materialized_size) == copy.materialized_hash;
        if (memoryUnchanged) ++g_verifiedCopyHits;
        else ++g_verifiedCopyMisses;
      } else {
        memoryUnchanged = copy.memory_epoch_valid &&
            (efb_copy_epoch(address, copy.byte_size, &memoryEpoch) && memoryEpoch == copy.memory_epoch);
      }]=] _gcnllr_draw "${_gcnllr_draw}")
set(_gcnllr_copy_publish [=[
void publish_efb_copy_memory(uint32_t address, const void* bytes, uint32_t size,
                             uint32_t width, uint32_t height, uint32_t format) {
  auto it = g_efbCopyTextures.find(address);
  if (it == g_efbCopyTextures.end() || bytes == nullptr || size == 0u) return;
  auto& copy = it->second;
  copy.materialized_hash = XXH3_64bits(bytes, size);
  copy.materialized_size = size;
  copy.width = width; copy.height = height; copy.format = format;
}
void efb_copy_memory_stats(uint64_t* hits, uint64_t* misses) {
  *hits = g_verifiedCopyHits; *misses = g_verifiedCopyMisses;
}
]=])
string(REPLACE "void copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  "${_gcnllr_copy_publish}\nvoid copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "void reset_texture_cache() {"
  "void reset_texture_cache() {\n  g_verifiedCopyHits = g_verifiedCopyMisses = 0;"
  _gcnllr_draw "${_gcnllr_draw}")
# A deferred producer owns a fresh render target. The donor's address/shape
# cache normally reuses mutable targets, which cannot represent older tokens.
set(_gcnllr_owned_allocation "auto it = g_efbCopyCache.find(key);\n  if (it == g_efbCopyCache.end() || !it->second) {")
set(_gcnllr_owned_epoch "cmd.byte_size != 0u && efb_copy_epoch(cmd.dest_address, cmd.byte_size, &memoryEpoch);")
set(_gcnllr_owned_lookup "    auto efbIt = g_efbCopyTextures.find(address);")
set(_gcnllr_owned_tail "// Uniform de-duplication across consecutive draws.")
foreach(_gcnllr_owned_anchor IN ITEMS _gcnllr_owned_allocation _gcnllr_owned_epoch _gcnllr_owned_lookup _gcnllr_owned_tail)
  string(FIND "${_gcnllr_draw}" "${${_gcnllr_owned_anchor}}" _gcnllr_owned_offset)
  if(_gcnllr_owned_offset EQUAL -1)
    message(FATAL_ERROR "Pinned owned texture adaptation differs: ${_gcnllr_owned_anchor}")
  endif()
endforeach()
string(REPLACE "${_gcnllr_owned_allocation}"
  "auto it = g_efbCopyCache.find(key);\n  if (g_capturingOwnedTexture || it == g_efbCopyCache.end() || !it->second) {"
  _gcnllr_draw "${_gcnllr_draw}")
# Complete replacement need not download the previous destination. The
# machine's row ownership manager handles partial overlap/eviction at publish.
string(REPLACE "${_gcnllr_owned_epoch}"
  "!g_capturingOwnedTexture && cmd.byte_size != 0u && efb_copy_epoch(cmd.dest_address, cmd.byte_size, &memoryEpoch);"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "void copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  "bool g_capturingOwnedTexture = false;\nOwnedTextureLookup g_ownedTextureLookup = nullptr;\nvoid copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "${_gcnllr_owned_lookup}" [=[    // A live exact producer is the proof for a GPU view. On a miss every
    // existing observer/hash/decode below still sees actual current MEM1.
    if (g_ownedTextureLookup != nullptr) {
      if (auto owned = g_ownedTextureLookup(address, tsize, format, width, height))
        return owned;
    }
    auto efbIt = g_efbCopyTextures.find(address);]=]
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "${_gcnllr_owned_tail}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/owned_texture.inc\"\n\n${_gcnllr_owned_tail}"
  _gcnllr_draw "${_gcnllr_draw}")
string(PREPEND _gcnllr_draw "#include \"${CMAKE_CURRENT_LIST_DIR}/owned_texture.hpp\"\n#include <stdexcept>\n")
string(PREPEND _gcnllr_draw "#include \"${CMAKE_CURRENT_LIST_DIR}/texture_encode.hpp\"\n")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_resident_draw.cmake")
set(_gcnllr_draw_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gxcore_draw.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_draw_path}" CONTENT "${_gcnllr_draw}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/gxcore_draw.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_draw_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_draw_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# Replace the diagnostic allocation/poll loop with the ordered native-copy
# provider. It preserves actual RGBA bytes and uses worker-owned pooled staging
# and Dawn completion events; the donor checkout stays immutable.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/texture.cpp" _gcnllr_texture)
string(FIND "${_gcnllr_texture}" "std::vector<uint8_t> read_texture_rgba8(const TextureHandle& texture) {" _gcnllr_readback_first)
string(FIND "${_gcnllr_texture}" "void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {" _gcnllr_readback_last)
if(_gcnllr_readback_first LESS 0 OR _gcnllr_readback_last LESS_EQUAL _gcnllr_readback_first)
  message(FATAL_ERROR "Pinned texture readback provider boundary differs")
endif()
string(SUBSTRING "${_gcnllr_texture}" 0 ${_gcnllr_readback_first} _gcnllr_texture_before)
string(SUBSTRING "${_gcnllr_texture}" ${_gcnllr_readback_last} -1 _gcnllr_texture_after)
set(_gcnllr_texture "${_gcnllr_texture_before}${_gcnllr_texture_after}")
set(_gcnllr_texture_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/texture.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_texture_path}" CONTENT "${_gcnllr_texture}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/texture.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_texture_path}")
list(APPEND _gcnllr_gx_sources "${CMAKE_CURRENT_LIST_DIR}/texture_readback.cpp")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_texture_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/texture_readback.cpp" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib")

# A GPU-resident RGB565 copy must sample the same quantized channels as its
# tiled MEM1 representation. An unquantized RGBA blit only agrees on primary
# colors and changes after a CPU observation forces tile decode.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/tex_copy_conv.cpp" _gcnllr_conversion)
set(_gcnllr_rgb565 [=[static constexpr std::string_view FragRGB565 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    return vec4f(c.rgb, 1.0);
}
)"sv;]=])
string(FIND "${_gcnllr_conversion}" "${_gcnllr_rgb565}" _gcnllr_rgb565_offset)
if(_gcnllr_rgb565_offset EQUAL -1)
  message(FATAL_ERROR "Pinned RGB565 copy conversion differs")
endif()
string(REPLACE "${_gcnllr_rgb565}" [=[static constexpr std::string_view FragRGB565 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    let raw = vec3u(clamp(floor(c.rgb * 255.0 + vec3f(0.5)), vec3f(0.0), vec3f(255.0)));
    let packed = raw >> vec3u(3u, 2u, 3u);
    let expanded = (packed << vec3u(3u, 2u, 3u)) | (packed >> vec3u(2u, 4u, 2u));
    return vec4f(vec3f(expanded) / 255.0, 1.0);
}
)"sv;]=] _gcnllr_conversion "${_gcnllr_conversion}")
# Use the hardware-tested integer luma matrix before format quantization.
# Approximate floating coefficients produce 81 instead of 82 for full red.
# Dolphin TextureConverterShaderGen.cpp supplies the same conversion in the
# maintained scalar floor (GPL-2.0-or-later; surrounding Aurora is GPL).
set(_gcnllr_luma [=[fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}]=])
string(FIND "${_gcnllr_conversion}" "${_gcnllr_luma}" _gcnllr_luma_offset)
if(_gcnllr_luma_offset EQUAL -1)
  message(FATAL_ERROR "Pinned copy luma conversion differs")
endif()
string(REPLACE "${_gcnllr_luma}" [=[fn intensity(rgb: vec3f) -> f32 {
    let raw = vec3u(clamp(floor(rgb * 255.0 + vec3f(0.5)), vec3f(0.0), vec3f(255.0)));
    let y = (66u * raw.r + 129u * raw.g + 25u * raw.b + 4096u + 128u) >> 8u;
    return f32(y) / 255.0;
}]=] _gcnllr_conversion "${_gcnllr_conversion}")
set(_gcnllr_conversion_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/tex_copy_conv.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_conversion_path}" CONTENT "${_gcnllr_conversion}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/tex_copy_conv.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_conversion_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_conversion_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# GX_MTX2x4 initializes only two rows. Requiring an unused third row discarded
# valid IPL environment-map matrices and substituted raw-normal coordinates.
file(READ "${GXRUNTIME_DIR}/graphics/gxcore/src/gxcore.cpp" _gcnllr_core)
set(_gcnllr_tex_rows "for (std::uint32_t k = 0; k < 3u; ++k)\n        resolved = load_matrix_row(draw, row + k, rows[k]) && resolved;")
string(FIND "${_gcnllr_core}" "${_gcnllr_tex_rows}" _gcnllr_tex_rows_offset)
if(_gcnllr_tex_rows_offset EQUAL -1)
  message(FATAL_ERROR "Pinned GX texture matrix adaptation differs")
endif()
string(REPLACE "${_gcnllr_tex_rows}"
  "identity_rows(rows);\n      const std::uint32_t required_rows = key.tex_gens[i].projection != 0u ? 3u : 2u;\n      for (std::uint32_t k = 0; k < required_rows; ++k)\n        resolved = load_matrix_row(draw, row + k, rows[k]) && resolved;"
  _gcnllr_core "${_gcnllr_core}")
# Direct-vertex BIOS quads may batch just like indexed triangles. The inner
# consumer still requires identical vertex layout and XF version, preserves
# every primitive's topology, and stops at resources, state and copy fences.
# Donor interpolation identity was the reason for the indexed-only restriction;
# this service presents actual VI frames with interpolation disabled.
set(_gcnllr_direct_fusion "!pending_state_.bp_valid(GxCoreState::kDrawTagRegister) && pending_state_.position_indexed())")
string(FIND "${_gcnllr_core}" "${_gcnllr_direct_fusion}" _gcnllr_direct_fusion_offset)
if(_gcnllr_direct_fusion_offset EQUAL -1)
  message(FATAL_ERROR "Pinned GX direct-primitive fusion adaptation differs")
endif()
string(REPLACE "${_gcnllr_direct_fusion}" "!pending_state_.bp_valid(GxCoreState::kDrawTagRegister))"
  _gcnllr_core "${_gcnllr_core}")
set(_gcnllr_core_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gxcore.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_core_path}" CONTENT "${_gcnllr_core}" @ONLY)
get_target_property(_gcnllr_core_sources gxruntime_gxcore SOURCES)
list(REMOVE_ITEM _gcnllr_core_sources src/gxcore.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_core_path}")
set_property(TARGET gxruntime_gxcore PROPERTY SOURCES "${_gcnllr_core_sources}")

# Keep partial copy clears inside the recorded GPU pass sequence. A display
# fence can then submit all BIOS bands together, rather than map/unmap the
# large staging arena and wait for the GPU at every individual band.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/common.cpp" _gcnllr_common)
string(REPLACE "bool captureDepthSnapshot = false;"
  "bool captureDepthSnapshot = false;\n  wgpu::RenderPipeline machineClearPipeline;\n  ClipRect machineClearRect{};\n  wgpu::Color machineClearColor{};\n  float machineClearZ = 0;"
  _gcnllr_common "${_gcnllr_common}")
set(_gcnllr_pass_anchor "auto pass = cmd.BeginRenderPass(&renderPassDescriptor);\n  render_pass(pass, frame, passInfo, 0, split);")
string(FIND "${_gcnllr_common}" "${_gcnllr_pass_anchor}" _gcnllr_pass_offset)
if(_gcnllr_pass_offset EQUAL -1)
  message(FATAL_ERROR "Pinned recorded clear adaptation differs")
endif()
string(REPLACE "${_gcnllr_pass_anchor}" [=[auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  if (passInfo.machineClearPipeline) {
    const auto& r = passInfo.machineClearRect;
    pass.SetPipeline(passInfo.machineClearPipeline);
    pass.SetBlendConstant(&passInfo.machineClearColor);
    pass.SetViewport(0, 0, float(passInfo.targetSize.width), float(passInfo.targetSize.height),
                     passInfo.machineClearZ, passInfo.machineClearZ);
    if (r.width > 0 && r.height > 0) {
      pass.SetScissorRect(r.x, r.y, r.width, r.height);
      pass.Draw(3);
    }
  }
  render_pass(pass, frame, passInfo, 0, split);]=]
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "void queue_palette_conv(tex_palette_conv::ConvRequest req) {" [=[void machine_copy_clear(const wgpu::RenderPipeline& pipeline, ClipRect rect,
                        wgpu::Color color, float depth) {
  auto& pass = current_render_passes()[g_currentRenderPass];
  pass.machineClearPipeline = pipeline;
  pass.machineClearRect = rect;
  pass.machineClearColor = color;
  pass.machineClearZ = depth;
  pass.observable = true;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req) {]=]
  _gcnllr_common "${_gcnllr_common}")
set(_gcnllr_common_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/common.cpp")
set(_gcnllr_encode_anchor "void queue_palette_conv(tex_palette_conv::ConvRequest req) {")
string(FIND "${_gcnllr_common}" "${_gcnllr_encode_anchor}" _gcnllr_encode_offset)
if(_gcnllr_encode_offset LESS 0)
  message(FATAL_ERROR "Pinned ordered native encoding boundary differs")
endif()
string(REPLACE "${_gcnllr_encode_anchor}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/ordered_encode.inc\"\n${_gcnllr_encode_anchor}"
  _gcnllr_common "${_gcnllr_common}")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_resident_frame.cmake")
file(CONFIGURE OUTPUT "${_gcnllr_common_path}" CONTENT "${_gcnllr_common}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/common.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_common_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_common_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")
