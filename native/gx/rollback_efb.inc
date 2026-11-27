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
        .usage=wgpu::TextureUsage::CopySrc|wgpu::TextureUsage::CopyDst,
        .size=source.size,.format=source.format,.sampleCount=1};
    result.texture=gpu::g_device.CreateTexture(&descriptor);
    if(!result.texture)throw std::runtime_error("Rollback attachment allocation failed");
    return result;
}

static RollbackEfb rollback_live_efb() {
    return {gpu::g_frameBuffer,gpu::g_depthBuffer,raster_color,raster_depth,raster_scale};
}

static void rollback_copy(const RollbackEfb& source,const RollbackEfb& target) {
    worker::enqueue_work([source,target] {
        // Same-format whole-subresource copies preserve color and depth without
        // shaders, attachment views, bind groups or raster passes. One ordered
        // submission owns both native and high-resolution attachments.
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto copy=[&](const gpu::TextureWithSampler& from,const gpu::TextureWithSampler& to) {
            const wgpu::TexelCopyTextureInfo src{.texture=from.texture};
            const wgpu::TexelCopyTextureInfo dst{.texture=to.texture};
            encoder.CopyTextureToTexture(&src,&dst,&from.size);
        };
        copy(source.color,target.color);copy(source.depth,target.depth);
        if(source.scale>1) {
            copy(source.raster_color,target.raster_color);copy(source.raster_depth,target.raster_depth);
        }
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
        rollback_copy(rollback_live_efb(),*image);
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
    rollback_copy(*image,rollback_live_efb());
    worker::synchronize();validate_submission();
    return gfx::begin_frame(true);
}
