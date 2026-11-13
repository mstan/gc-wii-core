// SPDX-License-Identifier: MIT
// Included inside Aurora's generated common.cpp namespace. Reuse its existing
// offscreen suspension/restoration while replacing only target allocation.
static const webgpu::TextureWithSampler* machineViewColor;
static const webgpu::TextureWithSampler* machineViewDepth;
void machine_begin_native_view(const webgpu::TextureWithSampler& color,
        const webgpu::TextureWithSampler& depth) {
    if(g_inOffscreen || machineViewColor || color.size.width!=depth.size.width ||
       color.size.height!=depth.size.height)
        throw std::runtime_error("Native view requires an exclusive matching offscreen target");
    machineViewColor=&color;machineViewDepth=&depth;
    try {begin_offscreen(color.size.width,color.size.height);}
    catch(...) {machineViewColor=nullptr;machineViewDepth=nullptr;throw;}
    machineViewColor=nullptr;machineViewDepth=nullptr;
}
