// SPDX-License-Identifier: GPL-3.0-or-later
// Ordered depth readback, adapted from pinned RecompCore/Aurora depth_peek.cpp.
// Aurora portions: Copyright (c) 2022 Luke Street, MIT. The original notice
// and permission text are retained in third_party/aurora/LICENSE.
// This uses the same GX Z24 conversion but waits for the requested GPU work,
// rather than consuming an optional, wall-clock-throttled previous snapshot.
#include "gpu_bridge.hpp"
#include "gfx/common.hpp"
#include "gfx/render_worker.hpp"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace gcnllr::gpu_bridge {
namespace gpu=aurora::webgpu;
namespace worker=aurora::gfx::render_worker;
namespace {
wgpu::ComputePipeline pipeline;
wgpu::Buffer storage,readback;
uint64_t capacity{};
}
std::vector<uint32_t> read_depth(uint32_t& width,uint32_t& height) {
    if (!aurora::gfx::segment_frame()) throw std::runtime_error("Depth EFB submission failed");
    const auto size=gpu::g_depthBuffer.size;
    const auto view=gpu::g_depthBuffer.view;
    width=size.width;height=size.height;
    if (!view || !width || !height) throw std::runtime_error("Depth EFB is unavailable");
    struct Result {std::vector<uint32_t> pixels;};
    auto result=std::make_shared<Result>();
    worker::enqueue_work([size,view,result] {
        if (!pipeline) {
            std::string code="@group(0) @binding(0) var src: ";
            code+=gpu::g_graphicsConfig.msaaSamples>1 ? "texture_depth_multisampled_2d;" : "texture_depth_2d;";
            code+=R"(
@group(0) @binding(1) var<storage,read_write> dst: array<u32>;
@compute @workgroup_size(8,8) fn main(@builtin(global_invocation_id) p: vec3u) {
  let size=textureDimensions(src);
  if (p.x>=size.x || p.y>=size.y) { return; }
  let native=textureLoad(src,vec2i(p.xy),0);
  let depth=)";
            code+=aurora::gx::UseReversedZ ? "1.0-native;" : "native;";
            code+="dst[p.y*size.x+p.x]=min(u32(clamp(depth,0.0,1.0)*16777215.0+0.5),16777215u);}";
            wgpu::ShaderSourceWGSL source{};source.code=code.c_str();
            const wgpu::ShaderModuleDescriptor module_desc{.nextInChain=&source,.label="Ordered GameCube depth read"};
            const auto module=gpu::g_device.CreateShaderModule(&module_desc);
            const wgpu::ComputePipelineDescriptor desc{.label="Ordered GameCube depth read",
                .compute={.module=module,.entryPoint="main"}};
            pipeline=gpu::g_device.CreateComputePipeline(&desc);
        }
        const uint64_t bytes=uint64_t(size.width)*size.height*4;
        if (capacity<bytes) {
            wgpu::BufferDescriptor desc{.label="GameCube depth words",
                .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
            storage=gpu::g_device.CreateBuffer(&desc);
            desc.usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst;
            readback=gpu::g_device.CreateBuffer(&desc);capacity=bytes;
        }
        const std::array<wgpu::BindGroupEntry,2> entries{{
            {.binding=0,.textureView=view},{.binding=1,.buffer=storage,.size=bytes}}};
        const wgpu::BindGroupDescriptor desc{.layout=pipeline.GetBindGroupLayout(0),
            .entryCount=entries.size(),.entries=entries.data()};
        const auto bind=gpu::g_device.CreateBindGroup(&desc);
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginComputePass();pass.SetPipeline(pipeline);pass.SetBindGroup(0,bind);
        pass.DispatchWorkgroups((size.width+7)/8,(size.height+7)/8);pass.End();
        encoder.CopyBufferToBuffer(storage,0,readback,0,bytes);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        const auto buffer=readback;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
            [buffer,bytes,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if (status!=wgpu::MapAsyncStatus::Success) return;
                const auto* mapped=buffer.GetConstMappedRange(0,bytes);
                if (mapped) {result->pixels.resize(bytes/4);std::memcpy(result->pixels.data(),mapped,bytes);}
                buffer.Unmap();
            });
        gpu::g_instance.WaitAny(future,5000000000ull);
    });
    worker::synchronize();
    if (result->pixels.size()!=size_t(width)*height) throw std::runtime_error("Ordered depth EFB read failed");
    return std::move(result->pixels);
}
void release_depth() {pipeline={};storage={};readback={};capacity=0;}
}
