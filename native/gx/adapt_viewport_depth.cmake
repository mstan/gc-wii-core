# SPDX-License-Identifier: MIT
# Keep guest/logical viewport state intact. Clamp only the host API's depth
# endpoints, after reversed-Z conversion. XF arithmetic can round a nominal
# zero just below zero, which WebGPU correctly rejects as a viewport range.
file(READ "${_gcnllr_common_path}" _viewport_depth_source)
foreach(_endpoint min max)
  if(_endpoint STREQUAL "min")
    set(_depth_expression "gx::UseReversedZ ? 1.f - vp.zfar : vp.znear")
  else()
    set(_depth_expression "gx::UseReversedZ ? 1.f - vp.znear : vp.zfar")
  endif()
  set(_depth_old "const float ${_endpoint}Depth = ${_depth_expression};")
  string(FIND "${_viewport_depth_source}" "${_depth_old}" _depth_site)
  if(_depth_site EQUAL -1)
    message(FATAL_ERROR "Pinned host viewport depth endpoint differs")
  endif()
  string(REPLACE "${_depth_old}"
    "const float ${_endpoint}Depth = std::clamp(${_depth_expression}, 0.f, 1.f);"
    _viewport_depth_source "${_viewport_depth_source}")
endforeach()
file(CONFIGURE OUTPUT "${_gcnllr_common_path}" CONTENT "${_viewport_depth_source}" @ONLY)
