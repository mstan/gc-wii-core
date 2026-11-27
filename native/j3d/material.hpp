// SPDX-License-Identifier: MIT
#pragma once
#include "asset.hpp"
#include <gxruntime/gxcore/shader.hpp>

namespace gc_wii::j3d {
// The backend shader compiler consumes these expressions directly. No emulated
// register file, command encoder or FIFO is used to construct a material.
struct CompiledMaterial {
    gxruntime::gxcore::PipelineKey pipeline{};
    gxruntime::gxcore::VertexShaderConstants vertex{};
    gxruntime::gxcore::PixelShaderConstants pixel{};
    std::array<uint16_t,8> textures{};
    std::array<uint8_t,8> texture_matrix_slots{};
    uint8_t texture_mask{}, light_mask{}, texture_matrix_mask{};
    bool inherited_cull{};
    uint16_t fog_center{};
};
/* Host material inputs, independent of geometry staging and guest commands. */
struct BoundMaterial {
    gxruntime::gxcore::PipelineKey pipeline{};
    gxruntime::gxcore::VertexShaderConstants vertex{};
    gxruntime::gxcore::PixelShaderConstants pixel{};
    std::array<gxruntime::gxcore::PlanTexture,8> textures{};
    std::array<gxruntime::gxcore::PlanSampler,8> samplers{};
    uint8_t texture_mask{};
};
CompiledMaterial compile_material(const Material&,const Mesh&);
void set_fog(CompiledMaterial&,const std::array<uint8_t,44>&);
BoundMaterial bind_native_material(const CompiledMaterial&,std::span<const Texture>,float viewport_width);
// Convert geometry once. Matrix palettes, poses and texture transforms are
// explicit per-draw inputs, supplied by the consumer rather than guest GX.
gxruntime::gxcore::DrawPlan compile_mesh(const Mesh&,const Primitive&);
// Apply a material and its owned texture resources to an existing native mesh.
// Caller supplies projection, pose/normal banks, referenced lights and computed
// texture matrices. Missing dynamic inputs must be rejected by that caller.
void bind_material(const CompiledMaterial&,std::span<const Texture>,
                   gxruntime::gxcore::DrawPlan&,float viewport_width);
}
