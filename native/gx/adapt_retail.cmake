# SPDX-License-Identifier: MIT
if(GCNLLR_RENDER_PIPELINE STREQUAL "retail")
  if(NOT GCNLLR_PYTHON_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(GCNLLR_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/aurora_retail.py"
    "${CMAKE_CURRENT_LIST_DIR}/retail_command.hpp")
  set(_gcnllr_retail "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/command_processor.cpp")
  execute_process(COMMAND "${GCNLLR_PYTHON_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/aurora_retail.py"
    --source "${GXRUNTIME_DIR}/graphics/aurora/lib/gx/command_processor.cpp"
    --out "${_gcnllr_retail}" --root "${CMAKE_CURRENT_LIST_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
  get_target_property(_gcnllr_retail_sources aurora_gx SOURCES)
  list(REMOVE_ITEM _gcnllr_retail_sources lib/gx/command_processor.cpp)
  list(APPEND _gcnllr_retail_sources "${_gcnllr_retail}")
  set(_gcnllr_texture "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/retail_texture.cpp")
  execute_process(COMMAND "${GCNLLR_PYTHON_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/aurora_retail.py"
    --source "${_gcnllr_gx_path}" --texture
    --out "${_gcnllr_texture}" --root "${CMAKE_CURRENT_LIST_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
  # Compose with the existing shutdown fix and replace its actual target
  # source. Leaving both gx implementations in an archive silently selects
  # the first object instead of the intended cache provider.
  list(FIND _gcnllr_retail_sources "${_gcnllr_gx_path}" _gcnllr_texture_index)
  if(_gcnllr_texture_index EQUAL -1)
    message(FATAL_ERROR "Existing Aurora GX owner source was not found")
  endif()
  list(REMOVE_ITEM _gcnllr_retail_sources "${_gcnllr_gx_path}")
  list(APPEND _gcnllr_retail_sources "${_gcnllr_texture}")
  set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_retail_sources}")
  file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gx/recomp.cpp" _gx_memory_source)
  string(FIND "${_gx_memory_source}" "return (reg & 0x001FFFFFu) << 5;" _gx_memory_at)
  if(_gx_memory_at EQUAL -1)
    message(FATAL_ERROR "Pinned texture address helper differs")
  endif()
  string(REPLACE "return (reg & 0x001FFFFFu) << 5;"
    "return (reg & (GC_WII_GX_WII ? 0x00FFFFFFu : 0x001FFFFFu)) << 5;"
    _gx_memory_source "${_gx_memory_source}")
  set(_gx_memory_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/recomp.cpp")
  file(CONFIGURE OUTPUT "${_gx_memory_path}" CONTENT "${_gx_memory_source}" @ONLY)
  get_target_property(_gx_memory_sources aurora_gx SOURCES)
  list(FIND _gx_memory_sources lib/gx/recomp.cpp _gx_memory_index)
  if(_gx_memory_index EQUAL -1)
    message(FATAL_ERROR "Pinned texture address target source differs")
  endif()
  list(REMOVE_ITEM _gx_memory_sources lib/gx/recomp.cpp)
  list(APPEND _gx_memory_sources "${_gx_memory_path}")
  set_property(TARGET aurora_gx PROPERTY SOURCES "${_gx_memory_sources}")
  set_source_files_properties("${_gx_memory_path}" TARGET_DIRECTORY aurora_gx
    PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gx")
  set_source_files_properties("${_gcnllr_retail}" "${_gcnllr_texture}" TARGET_DIRECTORY aurora_gx
    PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gx")
endif()
