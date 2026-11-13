# SPDX-License-Identifier: MIT
# Keep the ordinary offscreen cache unchanged. Explicit native view generations
# supply fresh targets retained by their XFB, using the same pass machinery.
string(PREPEND _gcnllr_common "#include <stdexcept>\n")
set(_native_view_begin "void begin_offscreen(uint32_t width, uint32_t height) {")
set(_native_view_target "  auto offscreenEntry = get_offscreen_textures(width, height);")
foreach(_native_view_anchor IN ITEMS _native_view_begin _native_view_target)
  string(FIND "${_gcnllr_common}" "${${_native_view_anchor}}" _native_view_at)
  if(_native_view_at EQUAL -1)
    message(FATAL_ERROR "Pinned native view offscreen boundary differs: ${_native_view_anchor}")
  endif()
endforeach()
string(REPLACE "${_native_view_begin}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/native_view_target.inc\"\n${_native_view_begin}"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "${_native_view_target}"
  "  auto offscreenEntry = machineViewColor ? OffscreenCacheEntry{*machineViewColor,*machineViewDepth} : get_offscreen_textures(width, height);"
  _gcnllr_common "${_gcnllr_common}")
