// SPDX-License-Identifier: MIT
#pragma once
#include "../j3d/material.hpp"
#include <webgpu/webgpu_cpp.h>
#include <memory>
#include <span>
#include <bit>

namespace aurora::gfx::gxcore {
struct ResidentGeometry {
    wgpu::Buffer vertices,indices;
    uint32_t vertex_count{},index_count{},topology{};
    uint64_t vertex_bytes{},index_bytes{};
};
using ResidentMesh=std::shared_ptr<const ResidentGeometry>;
struct ResidentDraw {
    gc_wii::j3d::BoundMaterial material;
    float x{},y{},width{},height{},near_depth{},far_depth{};
    int32_t clip_x{},clip_y{},clip_width{},clip_height{};
};
struct ResidentGeometryStats {uint64_t uploads{},vertex_bytes{},index_bytes{},draws{};};
ResidentMesh create_resident_geometry(std::span<const float>,std::span<const uint16_t>,uint32_t topology);
bool submit_resident_draw(const ResidentMesh&,const ResidentDraw&);
ResidentGeometryStats resident_geometry_stats();
void reset_resident_geometry_stats();
// The frame packet retains ownership until deferred worker encoding is done.
// The recorded draw's raw pointer is never a guest address or recycled handle.
void retain_resident_geometry(const ResidentMesh&);
}
