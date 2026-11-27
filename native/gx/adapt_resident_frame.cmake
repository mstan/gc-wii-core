# SPDX-License-Identifier: MIT
# A draw record only contains a raw host pointer. The frame packet owns each
# referenced mesh until its deferred GPU encoding has consumed that record.
set(_resident_packet "struct FramePacket {\n  RenderPassList renderPasses;")
string(FIND "${_gcnllr_common}" "${_resident_packet}" _resident_packet_offset)
if(_resident_packet_offset LESS 0)
  message(FATAL_ERROR "Pinned native frame owner differs")
endif()
string(REPLACE "${_resident_packet}"
  "${_resident_packet}\n#ifdef AURORA_ENABLE_GXCORE\n  std::vector<gxcore::ResidentMesh> residentGeometry;\n#endif"
  _gcnllr_common "${_gcnllr_common}")
set(_resident_retain "void queue_palette_conv(tex_palette_conv::ConvRequest req) {")
string(REPLACE "${_resident_retain}"
  "#ifdef AURORA_ENABLE_GXCORE\nvoid gxcore::retain_resident_geometry(const gxcore::ResidentMesh& geometry) {\n  current_frame_packet().residentGeometry.push_back(geometry);\n}\n#endif\n${_resident_retain}"
  _gcnllr_common "${_gcnllr_common}")

# Independently rasterize the same immutable native job at display resolution.
# Keep the original EFB/copy/cache/CPU route authoritative at all scales.
set(_raster_anchor "static void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {")
string(FIND "${_gcnllr_common}" "${_raster_anchor}" _raster_offset)
if(_raster_offset LESS 0)
  message(FATAL_ERROR "Pinned native raster render boundary differs")
endif()
string(REPLACE "${_raster_anchor}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/native_raster.inc\"\n${_raster_anchor}"
  _gcnllr_common "${_gcnllr_common}")
set(_raster_copy "  if (passInfo.captureDepthSnapshot && !frame_interp::encoding_interpolated()) {")
string(FIND "${_gcnllr_common}" "${_raster_copy}" _raster_offset)
if(_raster_offset LESS 0)
  message(FATAL_ERROR "Pinned native raster snapshot boundary differs")
endif()
string(REPLACE "${_raster_copy}" [=[
  if (machineEncodingScale==1 && machineRasterScale>1 && !passInfo.offscreen &&
      passInfo.colorView.Get()==webgpu::g_frameBuffer.view.Get()) {
    auto enhanced=passInfo;
    enhanced.colorView=machineRasterColor.view;
    enhanced.resolveView=passInfo.msaaSamples>1?machineRasterResolved.view:nullptr;
    enhanced.depthStencilView=machineRasterDepth.view;
    enhanced.copySourceTexture=machineRasterResolved.texture;
    enhanced.copySourceView=machineRasterResolved.view;
    enhanced.copySourceDepthView=machineRasterDepth.view;
    enhanced.targetSize=machineRasterColor.size;
    enhanced.resolveTarget={};enhanced.captureDepthSnapshot=false;enhanced.observable=true;
    enhanced.machineClearRect.x*=machineRasterScale;enhanced.machineClearRect.y*=machineRasterScale;
    enhanced.machineClearRect.width*=machineRasterScale;enhanced.machineClearRect.height*=machineRasterScale;
    machineEncodingScale=machineRasterScale;
    render(cmd,frame,enhanced,passIndex);
    machineEncodingScale=1;
  }
  if (passInfo.captureDepthSnapshot && !frame_interp::encoding_interpolated()) {
]=] _gcnllr_common "${_gcnllr_common}")
string(REPLACE "pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);"
 "pass.SetViewport(vp.left*machineEncodingScale, vp.top*machineEncodingScale, vp.width*machineEncodingScale, vp.height*machineEncodingScale, minDepth, maxDepth);"
 _gcnllr_common "${_gcnllr_common}")
foreach(_raster_coord x y width height)
 string(REPLACE "static_cast<uint32_t>(sc.${_raster_coord})" "static_cast<uint32_t>(sc.${_raster_coord}*machineEncodingScale)"
   _gcnllr_common "${_gcnllr_common}")
endforeach()
