// SPDX-License-Identifier: MIT
#pragma once
#include <gcnllr/native_render.h>
#include <gxruntime/gxcore/shader.hpp>
#include <unordered_map>

namespace gcnllr {
// Owns converted native geometry, not a shadow CP/XF/BP state machine.
// The GPU backend consumes DrawPlan directly: no FIFO or register decoder.
class NativeScene {
public:
    uint64_t create_mesh(const GcWiiNativeMesh&);
    bool destroy_mesh(uint64_t);
    gxruntime::gxcore::DrawPlan* prepare(const GcWiiNativeDraw&);
    void submitted() { ++status_.draws; }
    void rejected() { ++status_.rejected; }
    void reset();
    GcWiiNativeRenderStatus status() const { return status_; }
private:
    uint64_t next_handle_{1};
    GcWiiNativeRenderStatus status_{};
    std::unordered_map<uint64_t, gxruntime::gxcore::DrawPlan> meshes_;
};
}
