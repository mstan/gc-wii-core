// SPDX-License-Identifier: MIT
#include "gpu_bridge.hpp"
#include "efb_geometry.hpp"
#include <gxruntime/gxcore/shader.hpp>
#include <aurora/aurora.h>
#include "gfx/common.hpp"
#include "gfx/texture.hpp"
#include "gfx/clear.hpp"
#include "gxcore_draw.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/render_worker.hpp"
#include "gfx/efb_readback.hpp"
#include "gfx/depth_peek.hpp"
#include "gx/gx.hpp"
#include "imgui.hpp"
#include "window.hpp"
#include "webgpu/gpu.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#endif

namespace aurora::gfx {
void machine_copy_clear(const wgpu::RenderPipeline&,ClipRect,wgpu::Color,float);
void machine_encode_after_resolve(std::function<void(wgpu::CommandEncoder&)>);
void machine_encode_tail(std::function<void(wgpu::CommandEncoder&)>);
}

namespace gcnllr::gpu_bridge {
namespace gfx=aurora::gfx;
namespace gpu=aurora::webgpu;
namespace worker=aurora::gfx::render_worker;
struct EncodedImage {
    uint32_t width{},height{};
    wgpu::Buffer buffer;
};
static gpu::TextureWithSampler scanout;
#if GCNLLR_QUEUED_PRESENT
// Aurora also borrows the override on the caller while preparing a viewport.
// Keep one stable object per geometry until a resize/shutdown drain, rather
// than letting a worker clear destroy it concurrently with that caller read.
static std::shared_ptr<gpu::TextureWithSampler> queued_scanout;
#endif
static std::array<wgpu::RenderPipeline,8> clear_pipelines;
static std::vector<uint8_t> pixels;
static wgpu::Buffer readback_buffer;
static uint64_t readback_capacity{};
static wgpu::Buffer display_encoded;
static uint64_t display_encoded_capacity{};
static wgpu::ComputePipeline display_encoder;
static wgpu::Buffer scanout_input;
static uint64_t scanout_input_capacity{};
static wgpu::ComputePipeline scanout_decoder;
static std::array<wgpu::Buffer,2> scanout_levels;
static bool idle_dimming{}; // machine-owner state, captured by each presentation
void set_idle_dimming(bool enabled){idle_dimming=enabled;}
struct DisplayBand {
    gfx::TextureHandle texture;
    uint32_t width{},height{},first_row{};
    std::array<uint32_t,3> weights{};
    EncodedImageHandle encoded;
};
static std::vector<DisplayBand> display_bands;
static bool recorded_copies{};
static uint64_t recorded_encodes{},observation_submissions{},publication_joins{};
static uint64_t scanout_frames{},scanout_joins{};
static std::mutex submission_mutex;
static std::exception_ptr submission_error;
void validate_submission() {
    std::lock_guard lock(submission_mutex);
    if(submission_error)std::rethrow_exception(submission_error);
}
void acknowledge_submission() {
    worker::synchronize();
    validate_submission();
}
void submit_copies() {
#if GCNLLR_BATCHED_SUBMISSION
    validate_submission();
    if(recorded_copies) {
        if(!gfx::segment_frame())throw std::runtime_error("Observed GPU packet submission failed");
        recorded_copies=false;++observation_submissions;
    }
#endif
}
static wgpu::Buffer display_parameters(const DisplayBand& band) {
    const std::array<uint32_t,8> values{band.weights[0],band.weights[1],band.weights[2],band.first_row,
        band.width,band.height,0,0};
    const wgpu::BufferDescriptor descriptor{.label="Immutable GameCube copy filter",
        .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=sizeof(values)};
    auto buffer=gpu::g_device.CreateBuffer(&descriptor);
    gpu::g_queue.WriteBuffer(buffer,0,values.data(),sizeof(values));
    return buffer;
}
static size_t pending_displays{};
// Same integer BT.601 conversion as the portable CPU copy. The storage word
// represents the actual four YUY2 bytes, not a presentation-only approximation.
static constexpr char display_encode_shader[]=R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> dst: array<u32>;
struct CopyParameters { weights: vec4<u32>, geometry: vec4<u32> };
@group(0) @binding(2) var<uniform> parameters: CopyParameters;
fn rgb(x: i32,y: i32) -> vec3<i32> {
    let size=textureDimensions(src);
    return vec3<i32>(textureLoad(src,clamp(vec2<i32>(x,y),vec2<i32>(0),vec2<i32>(size)-1),0).rgb*255.0+0.5);
}
fn filtered(x: i32,y: i32) -> vec3<i32> {
    let row=y+i32(parameters.weights.w);
    let middle=rgb(x,row);
    let weights=parameters.weights.xyz;
    // The SDK's middle-only taps sum to64. Preserve the fast identity path;
    // every other set, including all-zero taps, follows integer arithmetic.
    if (all(weights==vec3<u32>(0u,64u,0u))) { return middle; }
    let sum=rgb(x,row-1)*i32(weights.x)+middle*i32(weights.y)+rgb(x,row+1)*i32(weights.z);
    return min(sum>>vec3<u32>(6u),vec3<i32>(255));
}
fn yuv(rgb: vec3<i32>) -> vec3<i32> {
    let y=((66*rgb.r+129*rgb.g+25*rgb.b+128)>>8u)+16;
    let u=((-38*rgb.r-74*rgb.g+112*rgb.b+128)>>8u)+128;
    let v=((112*rgb.r-94*rgb.g-18*rgb.b+128)>>8u)+128;
    return vec3<i32>(y,u,v);
}
@compute @workgroup_size(16,8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let size=parameters.geometry.xy;
    if (id.x>=size.x/2u || id.y>=size.y) { return; }
    let a=yuv(filtered(i32(id.x*2u),i32(id.y)));
    let b=yuv(filtered(i32(id.x*2u+1u),i32(id.y)));
    let left=yuv(filtered(i32(id.x*2u)-1,i32(id.y)));
    let y0=u32(clamp(a.x,0,255));let y1=u32(clamp(b.x,0,255));
    // Hardware chroma is centered on the even pixel: quarter left, half
    // even, quarter right. Keeping integer precision also preserves the
    // signed chroma rounding used by the portable copy engine.
    let u=u32(clamp((left.y+2*a.y+b.y)>>2u,0,255));
    let v=u32(clamp((left.z+2*a.z+b.z)>>2u,0,255));
    dst[id.y*(size.x/2u)+id.x]=y0|(u<<8u)|(y1<<16u)|(v<<24u);
}
)";
static constexpr char scanout_decode_shader[]=R"(
@group(0) @binding(0) var<storage, read> src: array<u32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> display: vec4<f32>;
@compute @workgroup_size(16,8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let size=textureDimensions(dst);
    if (id.x>=size.x/2u || id.y>=size.y) { return; }
    let pair=src[id.y*(size.x/2u)+id.x];
    let d=i32((pair>>8u)&255u)-128;let e=i32(pair>>24u)-128;
    let chroma=vec3<i32>(409*e+128,-100*d-208*e+128,516*d+128);
    for (var n=0u;n<2u;n++) {
        let c=298*(i32((pair>>(n*16u))&255u)-16);
        let rgb=clamp((vec3<i32>(c)+chroma)>>vec3<u32>(8u),vec3<i32>(0),vec3<i32>(255));
        textureStore(dst,vec2<i32>(i32(id.x*2u+n),i32(id.y)),vec4<f32>(vec3<f32>(rgb)/255.0*display.x,1.0));
    }
}
)";
#ifdef _WIN32
static HANDLE main_schedule{},render_schedule{};
#endif

void initialize() {
    idle_dimming=false;
    recorded_copies=false;recorded_encodes=observation_submissions=publication_joins=0;
    scanout_frames=scanout_joins=0;
    {std::lock_guard lock(submission_mutex);submission_error={};}
    wgpu::ShaderSourceWGSL source{};source.code=display_encode_shader;
    const wgpu::ShaderModuleDescriptor module_descriptor{.nextInChain=&source,.label="GameCube exact YUY2 encode"};
    const auto module=gpu::g_device.CreateShaderModule(&module_descriptor);
    const wgpu::ComputePipelineDescriptor pipeline_descriptor{.label="GameCube exact YUY2 encode",
        .compute={.module=module,.entryPoint="main"}};
    display_encoder=gpu::g_device.CreateComputePipeline(&pipeline_descriptor);
    source.code=scanout_decode_shader;
    const auto decode_module=gpu::g_device.CreateShaderModule(&module_descriptor);
    const wgpu::ComputePipelineDescriptor decode_descriptor{.label="GameCube exact VI decode",
        .compute={.module=decode_module,.entryPoint="main"}};
    scanout_decoder=gpu::g_device.CreateComputePipeline(&decode_descriptor);
    for(unsigned n=0;n<2;n++) {
        const std::array<float,4> values{n?0.25f:1.f,0,0,0};
        const wgpu::BufferDescriptor descriptor{.label="Immutable VI presentation brightness",
            .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=sizeof values};
        scanout_levels[n]=gpu::g_device.CreateBuffer(&descriptor);
        gpu::g_queue.WriteBuffer(scanout_levels[n],0,values.data(),sizeof values);
    }
#ifdef _WIN32
    // Real-time media work must not compete as a background batch job.
    // MMCSS scopes the scheduling request to the two deadline-bearing threads
    // and reverts it on those same threads during normal service shutdown.
    DWORD index{};main_schedule=AvSetMmThreadCharacteristicsW(L"Games",&index);
    worker::enqueue_work([] {DWORD index{};render_schedule=AvSetMmThreadCharacteristicsW(L"Games",&index);});
    worker::synchronize();
    std::fprintf(stderr,"[native-gx-scheduler] main=%s render=%s\n",
        main_schedule ? "MMCSS Games" : "normal",render_schedule ? "MMCSS Games" : "normal");
#endif
    // Compile the general fallback before any firmware time or audio starts.
    // Other fixed-function variants share these shaders and remain small;
    // first-use guest geometry must neither disappear nor pause boot for DXC.
    if (gfx::ubershader_mode()!=0) {
        gfx::gxcore::PipelineConfig config{};
        config.msaaSamples=gpu::g_graphicsConfig.msaaSamples;config.depthOnly=2;
        config.key.color_update=1;config.key.blend_enable=1;config.key.depth_func=3;
        config.key.src_factor=config.key.src_factor_alpha=4;
        config.key.dst_factor=config.key.dst_factor_alpha=5;
        const auto ref=gfx::pipeline_ref(config);
        wgpu::RenderPipeline pipeline;
        if (!gfx::get_pipeline(ref,pipeline) || !pipeline)
            throw std::runtime_error("Native GPU fallback pipeline did not compile");
    }
}
bool settle_framebuffer() {
    // Aurora's first begin binds the window-sized EFB before VI's pending
    // native framebuffer resize. Start a continuation on the settled target
    // before any retail copy or draw can reference the obsolete attachment.
    finish();
    return gfx::begin_frame(false);
}
static void record_copy_clear(const gxruntime::gxcore::EfbCopyCommand& copy) {
    if (!copy.clear || (!copy.color_update && !copy.alpha_update && !copy.depth_update)) return;
    const auto& fb=gpu::g_frameBuffer;
    const gfx::ClipRect rect{int(copy.src_x*fb.size.width/efb_width),int(copy.src_y*fb.size.height/efb_height),
        int(copy.width*fb.size.width/efb_width),int(copy.height*fb.size.height/efb_height)};
    const unsigned key=unsigned(copy.color_update)|unsigned(copy.alpha_update)*2|unsigned(copy.depth_update)*4;
    auto& pipeline=clear_pipelines[key];
    if (!pipeline) pipeline=gfx::clear::create_pipeline({.msaaSamples=gpu::g_graphicsConfig.msaaSamples,
        .clearColor=copy.color_update,.clearAlpha=copy.alpha_update,.clearDepth=copy.depth_update});
    float z=copy.clear_z/16777215.f;if (aurora::gx::UseReversedZ) z=1-z;
    gfx::machine_copy_clear(pipeline,rect,{copy.clear_r/255.,copy.clear_g/255.,copy.clear_b/255.,copy.clear_a/255.},z);
}
void record_efb_clear(const gxruntime::gxcore::EfbCopyCommand& copy) {
    // A null resolve target seals the preceding render pass without creating
    // a texture or a copy. The clear starts the new pass, before its draws.
    const auto& fb=gpu::g_frameBuffer;
    gfx::resolve_pass({}, {0,0,int(fb.size.width),int(fb.size.height)},false,false,false,{},0,GX_TF_RGBA8);
    record_copy_clear(copy);
}
void complete_frame(bool physical) {
    if (!gfx::segment_frame()) throw std::runtime_error("Native frame completion submission failed");
    recorded_copies=false;
    if (physical) {
        struct Completion {bool done{},success{};};
        auto result=std::make_shared<Completion>();
        worker::enqueue_work([result] {
            const auto future=gpu::g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                [result](wgpu::QueueWorkDoneStatus status,wgpu::StringView) {
                    result->success=status==wgpu::QueueWorkDoneStatus::Success;result->done=true;
                });
            gpu::g_instance.WaitAny(future,5000000000ull);
        });
        worker::synchronize();
        if (!result->done || !result->success) throw std::runtime_error("Native frame GPU completion failed");
    } else worker::synchronize();
    validate_submission();
}
void record_display_copy(const gxruntime::gxcore::EfbCopyCommand& copy,const DisplayCopyFilter& filter) {
    if (!copy.width || !copy.height || copy.src_x+copy.width>efb_width || copy.src_y+copy.height>efb_height)
        throw std::runtime_error("Display copy outside supported EFB");
    const auto tap=[](uint32_t word,unsigned index){return (word>>(index*6u))&63u;};
    const std::array<uint32_t,3> weights{tap(filter.low,0)+tap(filter.low,1),
        tap(filter.low,2)+tap(filter.low,3)+tap(filter.high,0),tap(filter.high,1)+tap(filter.high,2)};
    // Capture required neighbors now, before copy-clear or subsequent draws.
    // Cropped boundaries clamp to the crop only when the corresponding BP
    // bit requests it; physical EFB boundaries always clamp to the last row.
    const uint32_t above=weights[0] && !filter.clamp_top && copy.src_y ? 1u:0u;
    const uint32_t below=weights[2] && !filter.clamp_bottom && copy.src_y+copy.height<efb_height ? 1u:0u;
    const uint32_t capture_y=copy.src_y-above,capture_height=copy.height+above+below;
    if (pending_displays==display_bands.size()) display_bands.emplace_back();
    auto& band=display_bands[pending_displays++];
    band.width=copy.width;band.height=copy.height;band.first_row=above;band.weights=weights;
    if (!band.texture || band.texture->size.width!=copy.width || band.texture->size.height!=capture_height)
        band.texture=gfx::new_render_texture(copy.width,capture_height,GX_TF_RGBA8,"GameCube display band and filter halo");
    const auto& fb=gpu::g_frameBuffer;
    const gfx::ClipRect capture_rect{int(copy.src_x*fb.size.width/efb_width),int(capture_y*fb.size.height/efb_height),
        int(copy.width*fb.size.width/efb_width),int(capture_height*fb.size.height/efb_height)};
    if (capture_rect.width!=int(copy.width) || capture_rect.height!=int(capture_height))
        throw std::runtime_error("Display-band GPU scaling is unsupported");
    gfx::resolve_pass(band.texture,capture_rect,false,false,false,{},0,GX_TF_RGBA8);
    record_copy_clear(copy);
    // The GPU snapshot and filter metadata are immutable before publication.
    // Encoding may happen later; it never samples the mutable EFB again.
#if GCNLLR_BATCHED_SUBMISSION
    auto image=std::make_shared<EncodedImage>();
    image->width=band.width;image->height=band.height;
    const uint64_t bytes=uint64_t(image->width)*image->height*2;
    const wgpu::BufferDescriptor descriptor{.label="Immutable GameCube XFB generation",
        .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
    image->buffer=gpu::g_device.CreateBuffer(&descriptor);
    band.encoded=image;
    gfx::machine_encode_after_resolve([band,image,bytes](wgpu::CommandEncoder& encoder) {
      try {
        const auto parameters=display_parameters(band);
        const std::array entries{
            wgpu::BindGroupEntry{.binding=0,.textureView=band.texture->texture.CreateView()},
            wgpu::BindGroupEntry{.binding=1,.buffer=image->buffer,.size=bytes},
            wgpu::BindGroupEntry{.binding=2,.buffer=parameters,.size=32}};
        const wgpu::BindGroupDescriptor binding{.layout=display_encoder.GetBindGroupLayout(0),
            .entryCount=entries.size(),.entries=entries.data()};
        auto pass=encoder.BeginComputePass();pass.SetPipeline(display_encoder);
        pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&binding));
        pass.DispatchWorkgroups((image->width/2+15)/16,(image->height+7)/8);pass.End();
      }catch(...) {std::lock_guard lock(submission_mutex);if(!submission_error)submission_error=std::current_exception();}
    });
    recorded_copies=true;++recorded_encodes;
#else
    if (!gfx::segment_frame()) throw std::runtime_error("Display band submission failed");
#endif
}

std::vector<CopiedImage> take_display_copies() {
    if (!pending_displays) return {};
#if GCNLLR_BATCHED_SUBMISSION
    auto encoded=retain_display_copies();std::vector<CopiedImage> images;
    images.reserve(encoded.size());
    for(const auto& image:encoded)images.push_back(materialize_display_copy(image));
    return images;
#endif
    if (!gfx::segment_frame()) throw std::runtime_error("Display fence submission failed");
    struct Result {std::vector<CopiedImage> images;};
    auto result=std::make_shared<Result>();
    std::vector<DisplayBand> bands(display_bands.begin(),display_bands.begin()+pending_displays);
    worker::enqueue_work([bands=std::move(bands),result] {
        uint64_t total=0;std::vector<uint64_t> offsets;
        for (const auto& band:bands) {
            // Storage bindings require 256-byte aligned offsets. Row data is
            // tightly packed; the unused inter-image alignment is never copied.
            total=(total+255u)&~uint64_t(255u);
            offsets.push_back(total);total+=uint64_t(band.width)*2*band.height;
        }
        if (!readback_buffer || readback_capacity<total) {
            const wgpu::BufferDescriptor descriptor{.label="GameCube completed display copies",
                .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=total};
            readback_buffer=gpu::g_device.CreateBuffer(&descriptor);readback_capacity=total;
        }
        if (!display_encoded || display_encoded_capacity<total) {
            const wgpu::BufferDescriptor descriptor{.label="GameCube encoded XFB bands",
                .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=total};
            display_encoded=gpu::g_device.CreateBuffer(&descriptor);display_encoded_capacity=total;
        }
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginComputePass();pass.SetPipeline(display_encoder);
        for (size_t i=0;i<bands.size();i++) {
            const auto& band=bands[i];
            const auto parameters=display_parameters(band);
            const std::array entries{
                wgpu::BindGroupEntry{.binding=0,.textureView=band.texture->texture.CreateView()},
                wgpu::BindGroupEntry{.binding=1,.buffer=display_encoded,.offset=offsets[i],
                    .size=uint64_t(band.width)*2*band.height},
                wgpu::BindGroupEntry{.binding=2,.buffer=parameters,.size=32}};
            const wgpu::BindGroupDescriptor descriptor{.layout=display_encoder.GetBindGroupLayout(0),
                .entryCount=entries.size(),.entries=entries.data()};
            pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&descriptor));
            pass.DispatchWorkgroups((band.width/2+15)/16,(band.height+7)/8);
        }
        pass.End();encoder.CopyBufferToBuffer(display_encoded,0,readback_buffer,0,total);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        auto buffer=readback_buffer;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,total,wgpu::CallbackMode::WaitAnyOnly,
            [buffer,total,bands,offsets,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if (status!=wgpu::MapAsyncStatus::Success) return;
                const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,total));
                if (mapped) for (size_t i=0;i<bands.size();i++) {
                    const auto& band=bands[i];const auto w=band.width,h=band.height;
                    CopiedImage image{w,h,std::vector<uint8_t>(size_t(w)*h*2)};
                    std::memcpy(image.yuy2.data(),mapped+offsets[i],image.yuy2.size());
                    result->images.push_back(std::move(image));
                }
                buffer.Unmap();
            });
        gpu::g_instance.WaitAny(future,5000000000ull);
    });
    worker::synchronize();
    if (result->images.size()!=pending_displays) throw std::runtime_error("Display fence readback failed");
    pending_displays=0;
    return std::move(result->images);
}

std::vector<EncodedImageHandle> retain_display_copies() {
    if(!pending_displays)return {};
#if GCNLLR_BATCHED_SUBMISSION
    validate_submission();std::vector<EncodedImageHandle> images;images.reserve(pending_displays);
    for(size_t i=0;i<pending_displays;i++)images.push_back(display_bands[i].encoded);
    pending_displays=0;return images;
#endif
    if(!gfx::segment_frame())throw std::runtime_error("Resident display submission failed");
    struct Result {std::vector<EncodedImageHandle> images;std::exception_ptr error;};
    auto result=std::make_shared<Result>();
    std::vector<DisplayBand> bands(display_bands.begin(),display_bands.begin()+pending_displays);
    worker::enqueue_work([bands=std::move(bands),result] {
      try {
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginComputePass();pass.SetPipeline(display_encoder);
        for(const auto& band:bands) {
            auto image=std::make_shared<EncodedImage>();
            image->width=band.width;image->height=band.height;
            const uint64_t bytes=uint64_t(image->width)*image->height*2;
            const wgpu::BufferDescriptor descriptor{.label="Immutable GameCube XFB generation",
                .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
            image->buffer=gpu::g_device.CreateBuffer(&descriptor);
            const auto parameters=display_parameters(band);
            const std::array entries{
                wgpu::BindGroupEntry{.binding=0,.textureView=band.texture->texture.CreateView()},
                wgpu::BindGroupEntry{.binding=1,.buffer=image->buffer,.size=bytes},
                wgpu::BindGroupEntry{.binding=2,.buffer=parameters,.size=32}};
            const wgpu::BindGroupDescriptor binding{.layout=display_encoder.GetBindGroupLayout(0),
                .entryCount=entries.size(),.entries=entries.data()};
            pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&binding));
            pass.DispatchWorkgroups((image->width/2+15)/16,(image->height+7)/8);
            result->images.push_back(std::move(image));
        }
        pass.End();const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
      }catch(...) {result->error=std::current_exception();}
    });
    // This waits for command submission, not GPU completion or mapping. Each
    // buffer is distinct: a later copy cannot overwrite a retained generation.
    worker::synchronize();
    ++publication_joins;
    if(result->error)std::rethrow_exception(result->error);
    if(result->images.size()!=pending_displays)throw std::runtime_error("Resident display encoding failed");
    pending_displays=0;return std::move(result->images);
}

#if GCNLLR_ASYNC_COPIES
gfx::AsyncReadback queue_display_readback(const EncodedImageHandle& image) {
    submit_copies();
    if(!image || !image->buffer)throw std::runtime_error("Unknown streamed XFB generation");
    return gfx::queue_buffer_readback(image->buffer,uint64_t(image->width)*image->height*2u);
}
#endif
CopiedImage materialize_display_copy(const EncodedImageHandle& image) {
    submit_copies();
    if(!image || !image->buffer)throw std::runtime_error("Unknown resident XFB generation");
    struct Result {CopiedImage image;std::exception_ptr error;bool done{};};
    auto result=std::make_shared<Result>();
    worker::enqueue_work([image,result] {
      try {
        const uint64_t bytes=uint64_t(image->width)*image->height*2;
        if(!readback_buffer || readback_capacity<bytes) {
            const wgpu::BufferDescriptor descriptor{.label="Observed GameCube XFB bytes",
                .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=bytes};
            readback_buffer=gpu::g_device.CreateBuffer(&descriptor);readback_capacity=bytes;
        }
        auto encoder=gpu::g_device.CreateCommandEncoder();
        encoder.CopyBufferToBuffer(image->buffer,0,readback_buffer,0,bytes);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        auto buffer=readback_buffer;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
            [image,result,buffer,bytes](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if(status==wgpu::MapAsyncStatus::Success) {
                    try {
                        const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,bytes));
                        if(mapped) {
                            result->image.width=image->width;result->image.height=image->height;
                            result->image.yuy2.assign(mapped,mapped+bytes);
                        }
                    }catch(...) {result->error=std::current_exception();}
                    buffer.Unmap();
                }
                result->done=true;
            });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        gpu::g_instance.WaitAny(future,5000000000ull);
        while(!result->done && std::chrono::steady_clock::now()<deadline) {
            gpu::g_instance.WaitAny(future,0);
            if(!result->done)std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if(!result->done) {readback_buffer={};readback_capacity=0;}
      }catch(...) {result->error=std::current_exception();readback_buffer={};readback_capacity=0;}
    });
    worker::synchronize();
    validate_submission();
    if(result->error)std::rethrow_exception(result->error);
    if(!result->done || result->image.yuy2.size()!=size_t(image->width)*image->height*2)
        throw std::runtime_error("Resident XFB materialization failed");
    return std::move(result->image);
}

std::vector<uint8_t> read_efb(uint32_t& width,uint32_t& height) {
    // End a recording segment without presenting or clearing the EFB.
    if (!gfx::segment_frame()) throw std::runtime_error("EFB submission failed");
    const auto& fb=gpu::g_graphicsConfig.msaaSamples>1 ? gpu::g_frameBufferResolved : gpu::g_frameBuffer;
    width=fb.size.width;height=fb.size.height;
    struct Result {std::vector<uint8_t> rgba;};
    auto result=std::make_shared<Result>();
    // This is a synchronous caller-visible copy, not an occasional screenshot.
    // Wait on the GPU completion event itself, without a timer/polling floor or
    // a new large staging allocation for every band of every BIOS frame.
    const auto texture=fb.texture;
    const auto size=fb.size;
    const bool bgra=fb.format==wgpu::TextureFormat::BGRA8Unorm;
    worker::enqueue_work([texture,size,bgra,result] {
        const uint32_t stride=(size.width*4+255)&~255u;
        const uint64_t bytes=uint64_t(stride)*size.height;
        if (!readback_buffer || readback_capacity<bytes) {
            const wgpu::BufferDescriptor descriptor{.label="GameCube EFB CPU copy",
                .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=bytes};
            readback_buffer=gpu::g_device.CreateBuffer(&descriptor);readback_capacity=bytes;
        }
        auto encoder=gpu::g_device.CreateCommandEncoder();
        const wgpu::TexelCopyTextureInfo source{.texture=texture};
        const wgpu::TexelCopyBufferInfo destination{
            .layout={.bytesPerRow=stride,.rowsPerImage=size.height},.buffer=readback_buffer};
        encoder.CopyTextureToBuffer(&source,&destination,&size);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        auto buffer=readback_buffer;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
            [buffer,bytes,stride,size,bgra,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if (status!=wgpu::MapAsyncStatus::Success) return;
                const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,bytes));
                if (mapped) {
                    result->rgba.resize(size_t(size.width)*size.height*4);
                    for (uint32_t y=0;y<size.height;y++) {
                        auto* row=result->rgba.data()+size_t(y)*size.width*4;
                        std::memcpy(row,mapped+size_t(y)*stride,size.width*4);
                        if (bgra) for (uint32_t x=0;x<size.width;x++) std::swap(row[x*4],row[x*4+2]);
                    }
                }
                buffer.Unmap();
            });
        gpu::g_instance.WaitAny(future,5000000000ull);
    });
    worker::synchronize();
    auto rgba=std::move(result->rgba);
    if (!width || !height || rgba.size()!=size_t(width)*height*4)
        throw std::runtime_error("Actual GPU EFB readback failed");
    return rgba;
}
void clear_efb(const gxruntime::gxcore::EfbCopyCommand& copy) {
    if (!copy.clear || (!copy.color_update && !copy.alpha_update && !copy.depth_update)) return;
    worker::enqueue_work([copy] {
        const unsigned key=unsigned(copy.color_update)|unsigned(copy.alpha_update)*2|unsigned(copy.depth_update)*4;
        auto& pipeline=clear_pipelines[key];
        if (!pipeline) pipeline=gfx::clear::create_pipeline({.msaaSamples=gpu::g_graphicsConfig.msaaSamples,
            .clearColor=copy.color_update,.clearAlpha=copy.alpha_update,.clearDepth=copy.depth_update});
        const auto& fb=gpu::g_frameBuffer;
        wgpu::RenderPassColorAttachment color{};
        color.view=fb.view;color.loadOp=wgpu::LoadOp::Load;color.storeOp=wgpu::StoreOp::Store;
        if (gpu::g_graphicsConfig.msaaSamples>1) color.resolveTarget=gpu::g_frameBufferResolved.view;
        wgpu::RenderPassDepthStencilAttachment depth{};
        depth.view=gpu::g_depthBuffer.view;depth.depthLoadOp=wgpu::LoadOp::Load;depth.depthStoreOp=wgpu::StoreOp::Store;
        const wgpu::RenderPassDescriptor descriptor{.label="GameCube EFB copy clear",
            .colorAttachmentCount=1,.colorAttachments=&color,.depthStencilAttachment=&depth};
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginRenderPass(&descriptor);
        pass.SetPipeline(pipeline);
        const wgpu::Color clear{copy.clear_r/255.,copy.clear_g/255.,copy.clear_b/255.,copy.clear_a/255.};
        pass.SetBlendConstant(&clear);
        float z=copy.clear_z/16777215.f;if (aurora::gx::UseReversedZ) z=1-z;
        pass.SetViewport(0,0,float(fb.size.width),float(fb.size.height),z,z);
        const uint32_t left=std::min(fb.size.width,uint32_t(uint64_t(copy.src_x)*fb.size.width/efb_width));
        const uint32_t top=std::min(fb.size.height,uint32_t(uint64_t(copy.src_y)*fb.size.height/efb_height));
        const uint32_t right=std::min(fb.size.width,uint32_t(uint64_t(copy.src_x+copy.width)*fb.size.width/efb_width));
        const uint32_t bottom=std::min(fb.size.height,uint32_t(uint64_t(copy.src_y+copy.height)*fb.size.height/efb_height));
        if (right>left && bottom>top) {pass.SetScissorRect(left,top,right-left,bottom-top);pass.Draw(3);}
        pass.End();const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    });
}
static void prepare_scanout(uint32_t width,uint32_t height) {
    validate_submission();
#if !GCNLLR_QUEUED_PRESENT
    submit_copies();
#endif
    if (!width || !height || width>640 || height>576 || (width&1))
        throw std::runtime_error("Invalid VI scanout");
    // The previous present must be submitted before its texture is updated.
    // Queue ordering then protects the previous frame without a GPU idle wait.
#if !GCNLLR_QUEUED_PRESENT
    worker::synchronize();++scanout_joins;
#endif
    if (!scanout.texture || scanout.size.width!=width || scanout.size.height!=height) {
#if GCNLLR_QUEUED_PRESENT
        if(scanout.texture){worker::synchronize();++scanout_joins;}
#endif
        scanout.size={width,height,1};scanout.format=wgpu::TextureFormat::RGBA8Unorm;
        const wgpu::TextureDescriptor descriptor{.label="GameCube VI scanout",.usage=wgpu::TextureUsage::StorageBinding|
            wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopySrc,.size=scanout.size,.format=scanout.format};
        scanout.texture=gpu::g_device.CreateTexture(&descriptor);scanout.view=scanout.texture.CreateView();
        const wgpu::SamplerDescriptor sampler{.magFilter=wgpu::FilterMode::Linear,.minFilter=wgpu::FilterMode::Linear};
        scanout.sampler=gpu::g_device.CreateSampler(&sampler);
#if GCNLLR_QUEUED_PRESENT
        queued_scanout=std::make_shared<gpu::TextureWithSampler>(scanout);
#endif
    }
}
static void decode_scanout(wgpu::CommandEncoder& encoder,const wgpu::Buffer& buffer,
        const gpu::TextureWithSampler& target,uint32_t width,uint32_t height,bool dimmed) {
    const uint64_t bytes=uint64_t(width)*height*2;
    const std::array entries{wgpu::BindGroupEntry{.binding=0,.buffer=buffer,.size=bytes},
        wgpu::BindGroupEntry{.binding=1,.textureView=target.view},
        wgpu::BindGroupEntry{.binding=2,.buffer=scanout_levels[dimmed],.size=16}};
    const wgpu::BindGroupDescriptor descriptor{.layout=scanout_decoder.GetBindGroupLayout(0),
        .entryCount=entries.size(),.entries=entries.data()};
    auto pass=encoder.BeginComputePass();
    pass.SetPipeline(scanout_decoder);pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&descriptor));
    pass.DispatchWorkgroups((width/2+15)/16,(height+7)/8);pass.End();
}
#if GCNLLR_QUEUED_PRESENT
static bool queue_scanout(const wgpu::Buffer& buffer,uint32_t width,uint32_t height,
        std::vector<DisplaySpan> spans={},std::vector<uint8_t> cpu={}) {
    // The override API borrows a pointer. Retain the exact texture generation
    // through the queued clear, even if the caller changes geometry meanwhile.
    const auto target=queued_scanout;++scanout_frames;
    const bool dimmed=idle_dimming;
    gfx::machine_encode_tail([buffer,target,width,height,dimmed,spans=std::move(spans),cpu=std::move(cpu)]
            (wgpu::CommandEncoder& encoder) {
        try {
            // Upload on this same FIFO worker. A later caller must not update a
            // reusable input buffer before the previous frame has been submitted.
            if(!cpu.empty())gpu::g_queue.WriteBuffer(buffer,0,cpu.data(),cpu.size());
            for(const auto& span:spans)if(span.image)
                encoder.CopyBufferToBuffer(span.image->buffer,span.source,buffer,span.destination,span.size);
            decode_scanout(encoder,buffer,*target,width,height,dimmed);
        }catch(...) {
            std::lock_guard lock(submission_mutex);
            if(!submission_error)submission_error=std::current_exception();
        }
    });
    worker::enqueue_work([target] {gpu::set_present_source_override(target.get());});
    aurora_end_frame();
    worker::enqueue_work([target] {gpu::set_present_source_override(nullptr);});
    recorded_copies=false;
    aurora::imgui::new_frame(aurora::window::get_window_size());
    // Aurora's bounded frame packets provide backpressure. Real CPU/PE reads,
    // screenshot observations and shutdown still wait for their required data.
    return gfx::begin_frame(true);
}
#endif
static bool show_encoded_buffer(const wgpu::Buffer& buffer,uint32_t width,uint32_t height) {
#if GCNLLR_QUEUED_PRESENT
    return queue_scanout(buffer,width,height);
#else
    ++scanout_frames;
    auto encoder=gpu::g_device.CreateCommandEncoder();
    decode_scanout(encoder,buffer,scanout,width,height,idle_dimming);
    const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    worker::enqueue_work([] {gpu::set_present_source_override(&scanout);});
    aurora_end_frame();
    worker::enqueue_work([] {gpu::set_present_source_override(nullptr);});
    aurora::imgui::new_frame(aurora::window::get_window_size());
    const bool ok=gfx::begin_frame(true);
    worker::synchronize();++scanout_joins;
    return ok;
#endif
}
bool present_display_copy(const EncodedImageHandle& image,uint32_t width,uint32_t height) {
    if(!image || image->width!=width || image->height!=height)return false;
    prepare_scanout(width,height);
    return show_encoded_buffer(image->buffer,width,height);
}
bool present_display_spans(const std::vector<DisplaySpan>& spans,uint32_t width,uint32_t height) {
    const uint64_t bytes=uint64_t(width)*height*2;
    uint64_t end=0;
    for(const auto& span:spans) {
        if(!span.size ||
           ((span.source|span.destination|span.size)&3u) || span.destination!=end ||
           uint64_t(span.destination)+span.size>bytes)return false;
        if(span.image) {
            if(!span.image->buffer || uint64_t(span.source)+span.size>
                uint64_t(span.image->width)*span.image->height*2)return false;
        } else if(span.cpu.size()!=span.size)return false;
        end+=span.size;
    }
    if(end!=bytes)return false;
    prepare_scanout(width,height);
    if(!scanout_input || scanout_input_capacity<bytes) {
        const wgpu::BufferDescriptor descriptor{.label="GameCube native VI composition",
            .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst,.size=bytes};
        scanout_input=gpu::g_device.CreateBuffer(&descriptor);scanout_input_capacity=bytes;
    }
    // Immutable encoded generations stay on the GPU. Queue order preserves
    // their earlier rendering and the previous scanout without a RAM round trip.
    std::vector<uint8_t> cpu;
    if(std::any_of(spans.begin(),spans.end(),[](const DisplaySpan& s){return !s.image;})) {
        cpu.resize(bytes);
        for(const auto& span:spans)if(!span.image)
            std::memcpy(cpu.data()+span.destination,span.cpu.data(),span.size);
        // One upload for all CPU-owned regions; GPU regions overwrite the
        // zero-filled holes below. Never inspect GPU-owned guest RAM.
#if !GCNLLR_QUEUED_PRESENT
        gpu::g_queue.WriteBuffer(scanout_input,0,cpu.data(),bytes);
#endif
    }
#if GCNLLR_QUEUED_PRESENT
    return queue_scanout(scanout_input,width,height,spans,std::move(cpu));
#else
    auto encoder=gpu::g_device.CreateCommandEncoder();
    for(const auto& span:spans)if(span.image)
        encoder.CopyBufferToBuffer(span.image->buffer,span.source,scanout_input,span.destination,span.size);
    const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    return show_encoded_buffer(scanout_input,width,height);
#endif
}
bool present(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride) {
    if(!yuy2 || stride<width*2)throw std::runtime_error("Invalid VI scanout bytes");
    prepare_scanout(width,height);
    const uint64_t bytes=uint64_t(width)*height*2;
    if (!scanout_input || scanout_input_capacity<bytes) {
        const wgpu::BufferDescriptor descriptor{.label="GameCube VI YUY2 bytes",
            .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst,.size=bytes};
        scanout_input=gpu::g_device.CreateBuffer(&descriptor);scanout_input_capacity=bytes;
    }
    if (stride!=width*2) {
        pixels.resize(bytes);
        for (uint32_t y=0;y<height;y++) std::memcpy(pixels.data()+size_t(y)*width*2,yuy2+size_t(y)*stride,width*2);
        yuy2=pixels.data();
    }
#if GCNLLR_QUEUED_PRESENT
    return queue_scanout(scanout_input,width,height,{},std::vector<uint8_t>(yuy2,yuy2+bytes));
#else
    gpu::g_queue.WriteBuffer(scanout_input,0,yuy2,bytes);
    return show_encoded_buffer(scanout_input,width,height);
#endif
}
void finish() {
    gfx::finish();
    gfx::end_frame([](wgpu::CommandEncoder& encoder) {
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);gfx::after_submit();
    });
    worker::synchronize();
    recorded_copies=false;
    validate_submission();
}
bool peek_scanout(uint16_t x,uint16_t y,uint32_t* value) {
    if (!value || !scanout.texture || x>=scanout.size.width || y>=scanout.size.height) return false;
    worker::synchronize();
    validate_submission();
    const auto rgba=gfx::read_texture_rgba8(scanout.texture,scanout.size,scanout.format);
    if (rgba.size()!=size_t(scanout.size.width)*scanout.size.height*4) return false;
    const auto* p=rgba.data()+(size_t(y)*scanout.size.width+x)*4;
    *value=(uint32_t(p[3])<<24)|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
    return true;
}
void release() {
    std::fprintf(stderr,"[native-vi-submission] queued=%u frames=%llu worker_joins=%llu\n",
        unsigned(GCNLLR_QUEUED_PRESENT),(unsigned long long)scanout_frames,(unsigned long long)scanout_joins);
    std::fprintf(stderr,"[native-gx-submission] batched=%u encodes=%llu observation_submissions=%llu publication_joins=%llu\n",
        unsigned(GCNLLR_BATCHED_SUBMISSION),(unsigned long long)recorded_encodes,
        (unsigned long long)observation_submissions,(unsigned long long)publication_joins);
    worker::synchronize();release_depth();
    worker::synchronize();scanout={};clear_pipelines={};pixels.clear();readback_buffer={};readback_capacity=0;
#if GCNLLR_QUEUED_PRESENT
    queued_scanout.reset();
#endif
    display_encoded={};display_encoded_capacity=0;display_encoder={};
    scanout_input={};scanout_input_capacity=0;scanout_decoder={};scanout_levels={};idle_dimming=false;
    display_bands.clear();pending_displays=0;
#ifdef _WIN32
    worker::enqueue_work([] {if (render_schedule) {AvRevertMmThreadCharacteristics(render_schedule);render_schedule=nullptr;}});
    worker::synchronize();
    if (main_schedule) {AvRevertMmThreadCharacteristics(main_schedule);main_schedule=nullptr;}
#endif
}
}
