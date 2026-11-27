// SPDX-License-Identifier: MIT
// Included in gpu_bridge's namespace. A checkpoint owns separate textures;
// later rendering never writes a retained snapshot's attachments.
struct RollbackEfb {
    gpu::TextureWithSampler color,depth,raster_color,raster_depth;
    uint32_t scale{};
};

bool rollback_efb_supported() {
    return gpu::g_graphicsConfig.msaaSamples==1 && gpu::g_frameBuffer.texture &&
        gpu::g_depthBuffer.view && gpu::g_frameBuffer.size.width &&
        gpu::g_frameBuffer.size.height &&
        gpu::g_depthBuffer.format==wgpu::TextureFormat::Depth32Float;
}

static gpu::TextureWithSampler rollback_attachment(const gpu::TextureWithSampler& source) {
    gpu::TextureWithSampler result{};
    result.size=source.size;result.format=source.format;
    const wgpu::TextureDescriptor descriptor{.label="Retained rollback EFB",
        .usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding,
        .size=source.size,.format=source.format,.sampleCount=1};
    result.texture=gpu::g_device.CreateTexture(&descriptor);
    if(!result.texture)throw std::runtime_error("Rollback attachment allocation failed");
    result.view=result.texture.CreateView();
    if(!result.view)throw std::runtime_error("Rollback attachment view failed");
    return result;
}

static void rollback_copy(const gpu::TextureWithSampler& color,const gpu::TextureWithSampler& depth,
                          const gpu::TextureWithSampler& target,const gpu::TextureWithSampler& target_depth) {
    worker::enqueue_work([color,depth,target,target_depth] {
        constexpr auto code=R"(
@group(0) @binding(0) var color:texture_2d<f32>;
@group(0) @binding(1) var depth:texture_depth_2d;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4<f32> {
    let x=f32((i<<1u)&2u);let y=f32(i&2u);
    return vec4<f32>(x*2.0-1.0,1.0-y*2.0,0.0,1.0);
}
struct Output { @location(0) color:vec4<f32>, @builtin(frag_depth) depth:f32 };
@fragment fn fs(@builtin(position) p:vec4<f32>)->Output {
    return Output(textureLoad(color,vec2<i32>(p.xy),0),textureLoad(depth,vec2<i32>(p.xy),0));
})";
        wgpu::ShaderSourceWGSL source{};source.code=code;
        const wgpu::ShaderModuleDescriptor shader_desc{.nextInChain=&source,.label="Rollback EFB copy"};
        const auto shader=gpu::g_device.CreateShaderModule(&shader_desc);
        const wgpu::ColorTargetState color_state{.format=target.format};
        const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&color_state};
        const wgpu::DepthStencilState depth_state{.format=target_depth.format,.depthWriteEnabled=true,
            .depthCompare=wgpu::CompareFunction::Always};
        const wgpu::RenderPipelineDescriptor descriptor{.label="Rollback EFB color and depth",
            .vertex={.module=shader,.entryPoint="vs"},.depthStencil=&depth_state,.fragment=&fragment};
        const auto pipeline=gpu::g_device.CreateRenderPipeline(&descriptor);
        const std::array entries{wgpu::BindGroupEntry{.binding=0,.textureView=color.view},
            wgpu::BindGroupEntry{.binding=1,.textureView=depth.view}};
        const wgpu::BindGroupDescriptor binding{.layout=pipeline.GetBindGroupLayout(0),
            .entryCount=entries.size(),.entries=entries.data()};
        const wgpu::RenderPassColorAttachment attachment{.view=target.view,
            .loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store};
        const wgpu::RenderPassDepthStencilAttachment depth_attachment{.view=target_depth.view,
            .depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=0.f};
        const wgpu::RenderPassDescriptor pass_desc{.label="Retained rollback attachments",
            .colorAttachmentCount=1,.colorAttachments=&attachment,.depthStencilAttachment=&depth_attachment};
        auto encoder=gpu::g_device.CreateCommandEncoder();auto pass=encoder.BeginRenderPass(&pass_desc);
        pass.SetPipeline(pipeline);pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&binding));pass.Draw(3);pass.End();
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    });
}

RollbackEfbHandle capture_rollback_efb() {
    if(!rollback_efb_supported())return {};
    auto image=std::make_shared<RollbackEfb>();image->scale=raster_scale;
    image->color=rollback_attachment(gpu::g_frameBuffer);
    image->depth=rollback_attachment(gpu::g_depthBuffer);
    if(raster_scale>1) {
        image->raster_color=rollback_attachment(raster_color);
        image->raster_depth=rollback_attachment(raster_depth);
    }
    // Allocate before closing the live frame, so a refused allocation leaves
    // the running renderer intact. Every later exit reopens its continuation.
    finish();
    try {
        rollback_copy(gpu::g_frameBuffer,gpu::g_depthBuffer,image->color,image->depth);
        if(raster_scale>1)rollback_copy(raster_color,raster_depth,image->raster_color,image->raster_depth);
        worker::synchronize();validate_submission();
    } catch(...) {
        (void)gfx::begin_frame(true);
        throw;
    }
    if(!gfx::begin_frame(true))throw std::runtime_error("Rollback EFB continuation failed");
    return image;
}

bool restore_rollback_efb(const RollbackEfbHandle& image) {
    if(!image || !rollback_efb_supported() || image->scale!=raster_scale ||
       image->color.size.width!=gpu::g_frameBuffer.size.width ||
       image->color.size.height!=gpu::g_frameBuffer.size.height)return false;
    finish();
    rollback_copy(image->color,image->depth,gpu::g_frameBuffer,gpu::g_depthBuffer);
    if(raster_scale>1)rollback_copy(image->raster_color,image->raster_depth,raster_color,raster_depth);
    worker::synchronize();validate_submission();
    return gfx::begin_frame(true);
}
