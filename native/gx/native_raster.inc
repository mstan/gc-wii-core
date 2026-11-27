// SPDX-License-Identifier: MIT
// One prepared GPU draw stream, independent native authority and display raster.
// The owner drains before replacing these targets. Worker packets never borrow
// mutable guest geometry, and no extra CPU scene traversal/upload is performed.
static webgpu::TextureWithSampler machineRasterColor,machineRasterResolved,machineRasterDepth;
static uint32_t machineRasterScale=1,machineEncodingScale=1;
void machine_raster_targets(const webgpu::TextureWithSampler& color,
    const webgpu::TextureWithSampler& resolved,const webgpu::TextureWithSampler& depth,uint32_t scale) {
    machineRasterColor=color;machineRasterResolved=resolved;machineRasterDepth=depth;machineRasterScale=scale;
}
