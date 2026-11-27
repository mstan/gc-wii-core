# SPDX-License-Identifier: MIT
# The Wii EFB has 528 rows even when VI displays 480. A 512-row texture copy
# must read its original rows, not stretch a clipped 480-row surface. Compose
# the existing generated sources and preserve the pinned donor checkout.
if(GC_WII_GX_WII)
  file(READ "${_gcnllr_window_path}" _efb_window)
  foreach(_axis width height)
    if(_axis STREQUAL "width")
      set(_efb_old ".fb_width = static_cast<uint32_t>(fb_w)")
      set(_efb_new ".fb_width = 640u")
    else()
      set(_efb_old ".fb_height = static_cast<uint32_t>(fb_h)")
      set(_efb_new ".fb_height = 528u")
    endif()
    string(REGEX MATCHALL "\\.fb_${_axis} = static_cast<uint32_t>\\(fb_[wh]\\)" _efb_matches "${_efb_window}")
    list(LENGTH _efb_matches _efb_count)
    if(NOT _efb_count EQUAL 2)
      message(FATAL_ERROR "Pinned window framebuffer-size return sites differ")
    endif()
    string(REPLACE "${_efb_old}" "${_efb_new}" _efb_window "${_efb_window}")
  endforeach()
  file(CONFIGURE OUTPUT "${_gcnllr_window_path}" CONTENT "${_efb_window}" @ONLY)

  if(GCNLLR_RENDER_PIPELINE STREQUAL "retail")
    set(_efb_gx_path "${_gcnllr_texture}")
  else()
    set(_efb_gx_path "${_gcnllr_gx_path}")
  endif()
  file(READ "${_efb_gx_path}" _efb_gx)
  set(_efb_old "return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();")
  string(FIND "${_efb_gx}" "${_efb_old}" _efb_site)
  if(_efb_site EQUAL -1)
    message(FATAL_ERROR "Pinned logical EFB-size helper differs")
  endif()
  string(REPLACE "${_efb_old}"
    "return gfx::is_offscreen() ? gfx::get_render_target_size() : Vec2<uint32_t>{640u, 528u};"
    _efb_gx "${_efb_gx}")
  file(CONFIGURE OUTPUT "${_efb_gx_path}" CONTENT "${_efb_gx}" @ONLY)
endif()
