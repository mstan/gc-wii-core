get_filename_component(GC_WII_GX_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
option(GC_WII_GX_WII "Wii GPU physical-address fields (GameCube preserves its original masks)" OFF)
option(GCNLLR_NATIVE_TYPED_OUTPUT "Typed native frame/raster presentation profile" OFF)
if(GCNLLR_NATIVE_TYPED_OUTPUT AND (GCNLLR_GPU_OWNED_DISPLAY OR GCNLLR_RESIDENT_XFB OR GCNLLR_GX_COOPERATIVE_VI OR GCNLLR_GX_FIFO_WORKER OR GCNLLR_ASYNC_COPIES))
  message(FATAL_ERROR "Typed output requires its guarded serial/queued machine owner")
endif()
target_compile_definitions(aurora_gx PRIVATE GC_WII_GX_WII=$<BOOL:${GC_WII_GX_WII}>)
set(GCNLLR_RENDER_PIPELINE "retail" CACHE STRING "Native Aurora command/batch owner; packet is a legacy comparison build")
set_property(CACHE GCNLLR_RENDER_PIPELINE PROPERTY STRINGS packet retail)
if(NOT GCNLLR_RENDER_PIPELINE MATCHES "^(packet|retail)$")
  message(FATAL_ERROR "Unknown native render pipeline")
endif()
set(GCNLLR_VERTEX_PREPARATION "planned" CACHE STRING "Native vertex preparation: planned or reference")
set_property(CACHE GCNLLR_VERTEX_PREPARATION PROPERTY STRINGS planned reference)
if(NOT GCNLLR_VERTEX_PREPARATION MATCHES "^(planned|reference)$")
  message(FATAL_ERROR "Unknown native vertex preparation")
endif()
target_compile_definitions(aurora_gx PRIVATE GCNLLR_RETAIL_VERTEX_PLAN=$<STREQUAL:${GCNLLR_VERTEX_PREPARATION},planned>)
option(GCNLLR_NATIVE_PIPELINE "Consolidated owner memory and FIFO ingress provider" OFF)
option(GCNLLR_COMPILED_SURFACES "Submit geometry-only display lists as prepared host surfaces" OFF)
option(GCNLLR_PERSISTENT_GEOMETRY "Retain epoch-validated native vertex and index data on the GPU" OFF)
if(GCNLLR_PERSISTENT_GEOMETRY AND (NOT GCNLLR_COMPILED_SURFACES OR
   NOT GCNLLR_RENDER_PIPELINE STREQUAL "retail" OR NOT GCNLLR_GPU_VISIBLE_CACHE))
  message(FATAL_ERROR "Persistent geometry requires compiled Aurora surfaces and visible memory epochs")
endif()
option(GCNLLR_DIRECT_DISPLAY_COPY "Encode eager display copies directly from the resolved EFB" OFF)
target_compile_definitions(aurora_gx PRIVATE GCNLLR_COMPILED_SURFACES=$<BOOL:${GCNLLR_COMPILED_SURFACES}>
  GCNLLR_PERSISTENT_GEOMETRY=$<BOOL:${GCNLLR_PERSISTENT_GEOMETRY}>)
option(GCNLLR_NATIVE_DRAW_SINK "Unqualified experimental single-state draw sink" OFF)
option(GCNLLR_ASYNC_COPIES "Batch GPU readbacks with ordered owner retirement" OFF)
if(GCNLLR_ASYNC_COPIES AND (NOT GCNLLR_RESIDENT_XFB OR NOT GCNLLR_GX_COOPERATIVE_VI))
  message(FATAL_ERROR "Streamed copies require observed memory and cooperative completion")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/adapt_window.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_worker_timing.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_retail.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_efb.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/adapt_viewport_depth.cmake")
set(GCNLLR_GPU_BACKEND "auto" CACHE STRING "Native GPU backend: auto, d3d12 or vulkan")
option(GCNLLR_GX_FIFO_WORKER "Translate native FIFO on a cooperating worker" OFF)
if(GCNLLR_NATIVE_PIPELINE AND GCNLLR_GX_FIFO_WORKER)
  message(FATAL_ERROR "Native memory/FIFO ingress requires one machine-thread owner")
endif()
option(GCNLLR_GX_COOPERATIVE_VI "Present completed VI scanout with donor-style recording ownership" OFF)
option(GCNLLR_COMPOSED_SCANOUT "Keep streamed generations owned for native VI and demand RAM reads" OFF)
option(GCNLLR_GPU_OWNED_DISPLAY "Present retained display color and encode guest XFB bytes on demand" OFF)
option(GCNLLR_GPU_OWNED_TEXTURES "Bind EFB copy textures directly until an actual byte observation" OFF)
option(GCNLLR_GPU_FRAME_PACKETS "Keep owned EFB copies in the consuming frame submission" OFF)
if(GCNLLR_GPU_FRAME_PACKETS AND NOT GCNLLR_GPU_OWNED_TEXTURES)
  message(FATAL_ERROR "Single frame packets require GPU ownership of display and texture copies")
endif()
if(GCNLLR_GPU_OWNED_TEXTURES AND NOT GCNLLR_GPU_OWNED_DISPLAY)
  message(FATAL_ERROR "GPU-owned textures require the same renderer to own display copies")
endif()
set(GCNLLR_GX_SUBMISSION "segmented" CACHE STRING "Build-time native submission: segmented, batched or queued")
set_property(CACHE GCNLLR_GX_SUBMISSION PROPERTY STRINGS segmented batched queued)
set(GCNLLR_NATIVE_OUTPUT_SUBMISSION "serial" CACHE STRING "Guarded native output presentation: serial or queued")
set_property(CACHE GCNLLR_NATIVE_OUTPUT_SUBMISSION PROPERTY STRINGS serial queued)
if(NOT GCNLLR_NATIVE_OUTPUT_SUBMISSION MATCHES "^(serial|queued)$")
  message(FATAL_ERROR "Unknown native output submission provider")
endif()
if(GCNLLR_NATIVE_OUTPUT_SUBMISSION STREQUAL "queued" AND (GCNLLR_GX_FIFO_WORKER OR
    GCNLLR_ASYNC_COPIES OR GCNLLR_COMPOSED_SCANOUT OR GCNLLR_GX_COOPERATIVE_VI OR GCNLLR_RESIDENT_XFB))
  message(FATAL_ERROR "Queued native output requires the guarded serial machine owner")
endif()
set(GCNLLR_GX_COMPLETION "physical" CACHE STRING "Build-time PE completion: physical or owned")
set_property(CACHE GCNLLR_GX_COMPLETION PROPERTY STRINGS physical owned)
if(NOT GCNLLR_GX_COMPLETION MATCHES "^(physical|owned)$")
  message(FATAL_ERROR "Unknown graphics completion provider")
endif()
if(GCNLLR_GX_COMPLETION STREQUAL "owned" AND NOT GCNLLR_COMPOSED_SCANOUT)
  message(FATAL_ERROR "Owned completion requires composed observed GPU ownership")
endif()
if(NOT GCNLLR_GX_SUBMISSION MATCHES "^(segmented|batched|queued)$")
  message(FATAL_ERROR "Unknown native graphics submission provider")
endif()
if(GCNLLR_DIRECT_DISPLAY_COPY AND (GCNLLR_RESIDENT_XFB OR NOT GCNLLR_GX_SUBMISSION STREQUAL "segmented"))
  message(FATAL_ERROR "Direct eager display copies require the segmented, eager ownership provider")
endif()
if(GCNLLR_GPU_OWNED_DISPLAY AND (NOT GCNLLR_RESIDENT_XFB OR NOT GCNLLR_GX_COOPERATIVE_VI OR
    GCNLLR_GX_FIFO_WORKER OR GCNLLR_ASYNC_COPIES OR NOT GCNLLR_GX_SUBMISSION STREQUAL "queued"))
  message(FATAL_ERROR "GPU-owned display requires queued owner-thread VI with resident XFB storage")
endif()
if(NOT GCNLLR_GX_SUBMISSION STREQUAL "segmented" AND NOT GCNLLR_COMPOSED_SCANOUT AND NOT GCNLLR_GPU_OWNED_DISPLAY)
  message(FATAL_ERROR "Batched submission requires composed observed GPU ownership")
endif()
set(_gcnllr_batched_submission OFF)
if(NOT GCNLLR_GX_SUBMISSION STREQUAL "segmented" OR GCNLLR_NATIVE_OUTPUT_SUBMISSION STREQUAL "queued")
  set(_gcnllr_batched_submission ON)
endif()
if(GCNLLR_COMPOSED_SCANOUT AND (NOT GCNLLR_ASYNC_COPIES OR NOT GCNLLR_RESIDENT_XFB OR NOT GCNLLR_GX_COOPERATIVE_VI OR GCNLLR_GX_FIFO_WORKER))
  message(FATAL_ERROR "Composed scanout requires the observed owner-thread streamed provider group")
endif()
if(GCNLLR_GX_COOPERATIVE_VI AND NOT GCNLLR_GX_FIFO_WORKER AND NOT GCNLLR_ASYNC_COPIES AND NOT GCNLLR_RESIDENT_XFB)
  message(FATAL_ERROR "Cooperating VI requires threaded translation or an observed owner-copy group")
endif()
if(NOT GCNLLR_GPU_BACKEND MATCHES "^(auto|d3d12|vulkan)$")
  message(FATAL_ERROR "Unknown native GPU backend: ${GCNLLR_GPU_BACKEND}")
endif()
add_library(gcnllr-native-gx SHARED "${CMAKE_CURRENT_LIST_DIR}/gx.cpp" "${CMAKE_CURRENT_LIST_DIR}/gpu_bridge.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/efb_depth.cpp" "${CMAKE_CURRENT_LIST_DIR}/native_scene.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/native_gui.cpp" "${CMAKE_CURRENT_LIST_DIR}/native_gui_effects.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/native_gui_program.cpp")
option(GC_WII_NATIVE_J3D "Build native J3D resource/material renderer (experimental consumer binding)" OFF)
target_compile_definitions(gcnllr-native-gx PRIVATE GC_WII_NATIVE_J3D=$<BOOL:${GC_WII_NATIVE_J3D}>)
if(GC_WII_NATIVE_J3D)
  if(GCNLLR_GX_FIFO_WORKER)
    message(FATAL_ERROR "Native J3D requires rendering on its caller thread")
  endif()
  target_sources(gcnllr-native-gx PRIVATE "${CMAKE_CURRENT_LIST_DIR}/native_j3d.cpp"
    "${GC_WII_GX_ROOT}/native/j3d/asset.cpp" "${GC_WII_GX_ROOT}/native/j3d/material.cpp")
endif()
option(GCNLLR_GPU_TRACE_CACHE "Read the GPU trace setting once at service startup" ON)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_GPU_TRACE_CACHE=$<BOOL:${GCNLLR_GPU_TRACE_CACHE}>)
if(GCNLLR_GPU_BACKEND STREQUAL "vulkan")
  target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_GPU_VULKAN=1)
elseif(GCNLLR_GPU_BACKEND STREQUAL "d3d12")
  if(NOT WIN32)
    message(FATAL_ERROR "D3D12 requires Windows")
  endif()
  target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_GPU_D3D12=1)
endif()
set_target_properties(gcnllr-native-gx PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gcnllr-services"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gcnllr-services")
target_compile_features(gcnllr-native-gx PRIVATE cxx_std_20)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_NATIVE_TYPED_OUTPUT=$<BOOL:${GCNLLR_NATIVE_TYPED_OUTPUT}>)
if(BUILD_TESTING)
  add_executable(test_native_gx_api "${CMAKE_CURRENT_LIST_DIR}/test_api.cpp")
  target_include_directories(test_native_gx_api PRIVATE "${GC_WII_GX_ROOT}/include")
  target_link_libraries(test_native_gx_api PRIVATE gcnllr-native-gx)
  target_compile_definitions(test_native_gx_api PRIVATE TEST_NATIVE_TYPED_OUTPUT=$<BOOL:${GCNLLR_NATIVE_TYPED_OUTPUT}>)
  add_test(NAME native_gx_capability_abi_contract COMMAND test_native_gx_api)
  set_tests_properties(native_gx_capability_abi_contract PROPERTIES ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:$<TARGET_FILE_DIR:gcnllr-native-gx>")
  find_package(Threads REQUIRED)
  add_executable(test_native_display "${GC_WII_GX_ROOT}/tests/test_native_display.cpp")
  target_include_directories(test_native_display PRIVATE "${GC_WII_GX_ROOT}/include")
  target_compile_features(test_native_display PRIVATE cxx_std_20)
  target_link_libraries(test_native_display PRIVATE Threads::Threads)
  add_test(NAME native_display_aspect_contract COMMAND test_native_display)
  add_executable(test_native_gui_program "${GC_WII_GX_ROOT}/tests/test_native_gui_program.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/native_scene.cpp" "${CMAKE_CURRENT_LIST_DIR}/native_gui.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/native_gui_program.cpp")
  target_compile_features(test_native_gui_program PRIVATE cxx_std_20)
  target_include_directories(test_native_gui_program PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${GC_WII_GX_ROOT}/include"
    "${GXRUNTIME_DIR}/include" "${GXRUNTIME_DIR}/graphics/gxcore/include")
  add_test(NAME native_gui_program_lowering COMMAND test_native_gui_program)
endif()
target_compile_definitions(gcnllr-native-gx PRIVATE GC_WII_GX_WII=$<BOOL:${GC_WII_GX_WII}>)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_RETAIL_PIPELINE=$<STREQUAL:${GCNLLR_RENDER_PIPELINE},retail>)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_NATIVE_PIPELINE=$<BOOL:${GCNLLR_NATIVE_PIPELINE}>
  GCNLLR_PERSISTENT_GEOMETRY=$<BOOL:${GCNLLR_PERSISTENT_GEOMETRY}>)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_NATIVE_DRAW_SINK=$<BOOL:${GCNLLR_NATIVE_DRAW_SINK}>)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_DIRECT_DISPLAY_COPY=$<BOOL:${GCNLLR_DIRECT_DISPLAY_COPY}>)
target_compile_definitions(gcnllr-native-gx PRIVATE
  GCNLLR_GPU_OWNED_DISPLAY=$<BOOL:${GCNLLR_GPU_OWNED_DISPLAY}>
  GCNLLR_GPU_OWNED_TEXTURES=$<BOOL:${GCNLLR_GPU_OWNED_TEXTURES}>
  GCNLLR_GPU_FRAME_PACKETS=$<BOOL:${GCNLLR_GPU_FRAME_PACKETS}>
  GCNLLR_GX_ONE_COPY=$<BOOL:${GCNLLR_GX_ONE_COPY}>)
target_compile_definitions(gcnllr-native-gx PRIVATE GCNLLR_GX_FIFO_WORKER=$<BOOL:${GCNLLR_GX_FIFO_WORKER}>
  GCNLLR_RESIDENT_XFB=$<BOOL:${GCNLLR_RESIDENT_XFB}>
  GCNLLR_GX_COOPERATIVE_VI=$<BOOL:${GCNLLR_GX_COOPERATIVE_VI}>
  GCNLLR_ASYNC_COPIES=$<BOOL:${GCNLLR_ASYNC_COPIES}>
  GCNLLR_COMPOSED_SCANOUT=$<BOOL:${GCNLLR_COMPOSED_SCANOUT}>
  GCNLLR_BATCHED_SUBMISSION=$<BOOL:${_gcnllr_batched_submission}>
  GCNLLR_NATIVE_QUEUED_OUTPUT=$<STREQUAL:${GCNLLR_NATIVE_OUTPUT_SUBMISSION},queued>
  GCNLLR_OWNED_COMPLETION=$<STREQUAL:${GCNLLR_GX_COMPLETION},owned>
  GCNLLR_QUEUED_PRESENT=$<OR:$<STREQUAL:${GCNLLR_GX_SUBMISSION},queued>,$<STREQUAL:${GCNLLR_NATIVE_OUTPUT_SUBMISSION},queued>>)
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/texture_readback.cpp" TARGET_DIRECTORY aurora_gx
  APPEND PROPERTY COMPILE_DEFINITIONS GCNLLR_ASYNC_COPIES=$<BOOL:${GCNLLR_ASYNC_COPIES}>)
target_include_directories(gcnllr-native-gx PRIVATE "${CMAKE_BINARY_DIR}/gcnllr-services/adapted" "${GC_WII_GX_ROOT}/include" "${GXRUNTIME_DIR}/graphics/aurora/lib")
target_link_libraries(gcnllr-native-gx PRIVATE
  aurora::core aurora::gx aurora::vi aurora::pad aurora::si
  absl::flat_hash_map)
if(GCNLLR_RENDER_PIPELINE STREQUAL "packet")
  target_link_libraries(gcnllr-native-gx PRIVATE GXRuntime::retail_gx_frontend GXRuntime::gxcore)
else()
  # Shared address/type declarations do not instantiate the legacy packet
  # frontend, trace decoder, or GXCore state/vertex preparation owner.
  target_include_directories(gcnllr-native-gx PRIVATE "${GXRUNTIME_DIR}/include" "${GXRUNTIME_DIR}/graphics/gxcore/include")
  if(GCNLLR_GX_FIFO_WORKER OR GCNLLR_NATIVE_DRAW_SINK)
    message(FATAL_ERROR "Native Aurora owns command/resource preparation on the machine thread")
  endif()
endif()
if(WIN32)
  target_link_libraries(gcnllr-native-gx PRIVATE avrt user32)
  add_custom_command(TARGET gcnllr-native-gx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:gcnllr-native-gx> $<TARGET_FILE_DIR:gcnllr-native-gx>
    COMMAND_EXPAND_LISTS)
endif()

# The graphics donor graph also exposes its generic DVD binding. Keep its
# existing seek semantics on the Windows C ABI without changing the donor.
if(TARGET gxruntime AND WIN32 AND (MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
   AND NOT CMAKE_PROJECT_NAME STREQUAL "bluewake_windows")
  # BlueWake already force-includes its 64-bit POSIX compatibility header.
  # Defining off_t before that header corrupts the CRT's typedef declaration.
  set_property(SOURCE "${GXRUNTIME_DIR}/src/dvd.c" TARGET_DIRECTORY gxruntime
    APPEND PROPERTY COMPILE_DEFINITIONS fseeko=_fseeki64 off_t=__int64)
endif()
if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
  # Older compatible donors use the CRT's optional math constants here.
  set_property(SOURCE "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/frame_interp.cpp"
    TARGET_DIRECTORY aurora_gx APPEND PROPERTY COMPILE_DEFINITIONS _USE_MATH_DEFINES)
endif()
