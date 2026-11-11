# SPDX-License-Identifier: MIT
# Keep the donor immutable. These three adapted translation units are the
# complete owners of GXCore DrawData; each includes this generated header.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/gxcore_draw.hpp" _resident_header)
string(REPLACE "#include \"common.hpp\""
  "#include \"${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/common.hpp\"\n#include \"${CMAKE_CURRENT_LIST_DIR}/resident_draw.hpp\""
  _resident_header "${_resident_header}")
set(_resident_field "  uint32_t batchSize = 1;\n};")
string(FIND "${_resident_header}" "${_resident_field}" _resident_offset)
if(_resident_offset LESS 0)
  message(FATAL_ERROR "Pinned native draw-record tail differs")
endif()
string(REPLACE "${_resident_field}" "  uint32_t batchSize = 1;\n  const ResidentGeometry* resident = nullptr;\n};"
  _resident_header "${_resident_header}")
string(REPLACE "wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);"
  "wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);\nPipelineRef resident_pipeline_ref(const PipelineConfig& config);"
  _resident_header "${_resident_header}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gxcore_draw.hpp" CONTENT "${_resident_header}" @ONLY)

# Share the existing coherent texture resolver. Native geometry never enters
# submit_draw_plan, interpolation planning or any vertex/index staging path.
set(_resident_lambda_start "  auto resolve_texture_handle =\n      [&](" )
set(_resident_lambda_end "\n  BindGroupRef textureBindGroup = 0;")
string(FIND "${_gcnllr_draw}" "${_resident_lambda_start}" _resident_start)
string(FIND "${_gcnllr_draw}" "${_resident_lambda_end}" _resident_end)
if(_resident_start LESS 0 OR _resident_end LESS _resident_start)
  message(FATAL_ERROR "Pinned native texture resolver differs")
endif()
math(EXPR _resident_length "${_resident_end}-${_resident_start}")
string(SUBSTRING "${_gcnllr_draw}" ${_resident_start} ${_resident_length} _resident_lambda)
string(REPLACE "${_resident_lambda_start}" "static TextureHandle resolve_texture_handle(" _resident_resolver "${_resident_lambda}")
string(REPLACE ") -> TextureHandle {" ") {" _resident_resolver "${_resident_resolver}")
string(REGEX REPLACE "  };[ \t\r\n]*$" "}\n\n" _resident_resolver "${_resident_resolver}")
string(REPLACE "${_resident_lambda}" "" _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "if (last != nullptr && uberPipeline == 0" "if (last != nullptr && !last->resident && uberPipeline == 0"
  _gcnllr_draw "${_gcnllr_draw}")
set(_resident_submit "bool submit_draw_plan(const gxc::DrawPlan& plan) {")
string(REPLACE "${_resident_submit}"
  "${_resident_resolver}\n#include \"${CMAKE_CURRENT_LIST_DIR}/resident_draw.inc\"\n${_resident_submit}"
  _gcnllr_draw "${_gcnllr_draw}")
set(_resident_render "void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {")
string(REPLACE "${_resident_render}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/resident_render.inc\"\n${_resident_render}\n  if(data.resident){render_resident(data,pass);return;}"
  _gcnllr_draw "${_gcnllr_draw}")
