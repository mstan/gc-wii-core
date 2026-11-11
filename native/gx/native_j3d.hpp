// SPDX-License-Identifier: MIT
#pragma once
#include <gcnllr/native_j3d.h>
#include "../j3d/material.hpp"
#include "resident_draw.hpp"
#include <unordered_map>
#include <map>

namespace gcnllr {
class NativeJ3D {
public:
    uint64_t create(std::span<const uint8_t>,const gc_wii::j3d::ImportOptions&);
    bool destroy(uint64_t);
    // All state is prepared before any submission, so an invalid input never
    // submits half of this mesh. A caller must likewise validate a packet batch
    // before using it to replace a whole original material/shape draw.
    struct Prepared {
        std::span<const aurora::gfx::gxcore::ResidentMesh> meshes;
        aurora::gfx::gxcore::ResidentDraw draw;
    };
    Prepared* prepare(const GcWiiJ3DDraw&);
    void submitted(){++status_.draws;}
    void rejected(const char*);
    GcWiiJ3DStatus status()const{return status_;}
    void reset();
private:
    struct Resource {
        struct Material {
            gc_wii::j3d::CompiledMaterial compiled;
            uint32_t position_mask{1},texture_matrix_mask{};
        };
        gc_wii::j3d::Model model;
        std::vector<std::vector<aurora::gfx::gxcore::ResidentMesh>> meshes;
        std::map<std::pair<uint32_t,uint32_t>,Material> materials;
    };
    std::unordered_map<uint64_t,Resource> resources_;
    uint64_t next_handle_{1};
    GcWiiJ3DStatus status_{};
    Prepared prepared_;
};
}
