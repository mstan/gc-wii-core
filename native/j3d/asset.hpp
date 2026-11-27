// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gc_wii::j3d {
// Host-owned model data. Import once when a resource is loaded; draw submission
// consumes these arrays and material equations without interpreting GX commands.
struct Vertex {
    std::array<float,3> position{}, normal{}, binormal{}, tangent{};
    std::array<std::array<float,4>,2> color{{{1,1,1,1},{1,1,1,1}}};
    std::array<std::array<float,2>,8> uv{};
    uint8_t position_matrix{};
    std::array<uint8_t,8> texture_matrices{};
};
enum class Topology : uint8_t { Triangles, Lines, Points };
struct Primitive {
    Topology topology{};
    uint32_t first_index{}, index_count{};
};
struct Mesh {
    uint16_t shape{}, group{};
    uint8_t matrix_type{}, normal_components{}; // 0 absent, 3 normal, 9 N/B/T
    std::vector<uint16_t> matrix_palette;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Primitive> primitives;
    uint32_t attribute_mask{};
};
struct MaterialStage {
    // Declarative combiner inputs/operations from MAT3, never a command stream.
    std::array<uint8_t,20> equation{};
    uint8_t texcoord{255}, texture{255}, raster{255}, konst_color{}, konst_alpha{};
    uint8_t raster_swap{}, texture_swap{};
};
struct Material {
    bool available{};
    uint8_t mode{}, cull{}, channels{}, texgens{};
    uint8_t depth_test{}, depth_func{7}, depth_write{}, early_depth{}, dither{};
    std::array<uint8_t,4> blend{};
    std::array<uint8_t,8> alpha_compare{};
    std::array<std::array<uint8_t,4>,2> color{}, ambient{};
    std::array<std::array<uint8_t,8>,4> lighting{};
    std::array<uint16_t,8> textures{};
    std::array<std::array<uint8_t,4>,8> texcoord_generators{};
    std::array<std::array<uint8_t,100>,8> texture_matrices{};
    uint8_t texture_matrix_mask{};
    std::array<std::array<int16_t,4>,4> registers{};
    std::array<std::array<uint8_t,4>,4> constants{}, swaps{};
    std::array<uint8_t,44> fog{};
    // Full indirect recipe: orders, matrices, scales and stage equations.
    std::array<uint8_t,312> indirect{};
    std::vector<MaterialStage> stages;
};
struct Texture {
    bool external{};
    uint32_t image_address{}, palette_address{};
    uint8_t format{}, alpha{}, wrap_s{}, wrap_t{}, min_filter{}, mag_filter{}, mip_count{};
    uint8_t edge_lod{}, bias_clamp{}, anisotropy{};
    uint16_t width{}, height{}, palette_format{}, palette_entries{};
    int16_t lod_bias{};
    int8_t min_lod{}, max_lod{};
    std::vector<uint8_t> image, palette;
};
struct Joint {
    uint16_t kind{};
    uint8_t compensate_scale{};
    std::array<float,3> scale{}, translation{};
    std::array<int16_t,3> rotation{};
};
struct MatrixReference { bool envelope{}; uint16_t index{}; };
struct Weight { uint16_t joint{}; float weight{}; };
struct HierarchyNode { uint16_t operation{}, index{}; };
struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Joint> joints;
    std::vector<MatrixReference> draw_matrices;
    std::vector<std::vector<Weight>> envelopes;
    std::vector<std::array<float,12>> inverse_bind;
    std::vector<HierarchyNode> hierarchy;
    uint16_t shape_count{};
};
// Throws on unsupported/corrupt input, without publishing a partial resource.
// No borrowed guest pointer or intermediate GX command survives the import.
struct ImportOptions {
    uint32_t guest_address{};
    void* user{};
    // Loaded models can reference textures shared with another resource.
    // Resolve through the consumer's checked memory map; file imports omit it.
    std::span<const uint8_t> (*resolve)(void*,uint32_t,size_t){};
};
Model import_model(std::span<const uint8_t> resource,const ImportOptions& = {});
}
