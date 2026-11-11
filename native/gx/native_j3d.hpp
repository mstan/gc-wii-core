// SPDX-License-Identifier: MIT
#pragma once
#include <gcnllr/native_j3d.h>
#include "../j3d/material.hpp"
#include <unordered_map>
#include <map>

namespace gcnllr {
class NativeJ3D {
public:
    uint64_t create(std::span<const uint8_t>,const gc_wii::j3d::ImportOptions&);
    bool destroy(uint64_t);
    // All plans are prepared before any submission, so an invalid input never
    // submits half of this mesh. A caller must likewise validate a packet batch
    // before using it to replace a whole original material/shape draw.
    std::vector<gxruntime::gxcore::DrawPlan>* prepare(const GcWiiJ3DDraw&);
    void submitted(){++status_.draws;}
    void rejected(const char*);
    GcWiiJ3DStatus status()const{return status_;}
    void reset();
private:
    struct Resource {
        gc_wii::j3d::Model model;
        std::vector<std::vector<gxruntime::gxcore::DrawPlan>> meshes;
        std::map<std::pair<uint32_t,uint32_t>,gc_wii::j3d::CompiledMaterial> materials;
    };
    std::unordered_map<uint64_t,Resource> resources_;
    uint64_t next_handle_{1};
    GcWiiJ3DStatus status_{};
};
}
