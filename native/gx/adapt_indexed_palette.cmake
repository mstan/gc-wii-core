# SPDX-License-Identifier: MIT
# Extend a generated decoder only for an explicit native asset tag. The guest
# GX palette namespace and pinned dependency sources are left untouched.
file(READ "${GXRUNTIME_DIR}/graphics/gxcore/src/texture_decode.cpp" _indexed_decoder)
set(_indexed_anchor "  if (entries == 0 || data == nullptr)")
string(FIND "${_indexed_decoder}" "${_indexed_anchor}" _indexed_offset)
if(_indexed_offset EQUAL -1)
  message(FATAL_ERROR "Pinned indexed palette decoder boundary differs")
endif()
string(PREPEND _indexed_decoder "#include \"${CMAKE_CURRENT_LIST_DIR}/indexed_palette.hpp\"\n")
string(REPLACE "${_indexed_anchor}"
  "  if (tlut_format == GC_WII_NATIVE_PALETTE_RGB5551_ZERO)\n    return gcnllr::decode_rgb5551_zero_palette(entries, data, size);\n${_indexed_anchor}"
  _indexed_decoder "${_indexed_decoder}")
set(_indexed_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/indexed_texture_decode.cpp")
file(CONFIGURE OUTPUT "${_indexed_path}" CONTENT "${_indexed_decoder}" @ONLY)
set_source_files_properties("${_indexed_path}" TARGET_DIRECTORY gxruntime_gxcore
  PROPERTIES INCLUDE_DIRECTORIES "${GC_WII_GX_ROOT}/include")
get_target_property(_indexed_sources gxruntime_gxcore SOURCES)
list(FIND _indexed_sources src/texture_decode.cpp _indexed_source_index)
if(_indexed_source_index EQUAL -1)
  message(FATAL_ERROR "Pinned indexed decoder target source differs")
endif()
list(REMOVE_ITEM _indexed_sources src/texture_decode.cpp)
list(APPEND _indexed_sources "${_indexed_path}")
set_property(TARGET gxruntime_gxcore PROPERTY SOURCES "${_indexed_sources}")
