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
