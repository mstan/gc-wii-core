// SPDX-License-Identifier: MIT
// Included beside the existing host shader/texture plumbing. Asset creation
// owns vertex/index uploads; recording only copies pose/material uniforms.
static ResidentGeometryStats resident_stats;
ResidentGeometryStats resident_geometry_stats(){return resident_stats;}
void reset_resident_geometry_stats(){resident_stats={};}
ResidentMesh create_resident_geometry(std::span<const float> vertices,std::span<const uint16_t> indices,uint32_t topology) {
    if(vertices.empty() || vertices.size()%gxc::kVertexFloats || indices.empty())
        throw std::runtime_error("Invalid resident geometry");
    auto geometry=std::make_shared<ResidentGeometry>();
    geometry->vertex_count=uint32_t(vertices.size()/gxc::kVertexFloats);
    geometry->index_count=uint32_t(indices.size());geometry->topology=topology;
    geometry->vertex_bytes=vertices.size_bytes();geometry->index_bytes=indices.size_bytes();
    for(auto index:indices)if(index>=geometry->vertex_count)throw std::runtime_error("Resident mesh index exceeds vertices");
    const auto upload=[&](const void* data,uint64_t bytes,wgpu::BufferUsage usage,const char* label) {
        const uint64_t size=(bytes+3)&~uint64_t(3);
        const wgpu::BufferDescriptor descriptor{.label=label,.usage=usage,.size=size,.mappedAtCreation=true};
        auto buffer=g_device.CreateBuffer(&descriptor);auto* destination=buffer.GetMappedRange(0,size);
        if(!destination)throw std::runtime_error("Resident GPU geometry allocation failed");
        std::memcpy(destination,data,bytes);
        if(size>bytes)std::memset(static_cast<uint8_t*>(destination)+bytes,0,size-bytes);
        buffer.Unmap();return buffer;
    };
    geometry->vertices=upload(vertices.data(),vertices.size_bytes(),wgpu::BufferUsage::Vertex,"Native immutable vertices");
    geometry->indices=upload(indices.data(),indices.size_bytes(),wgpu::BufferUsage::Index,"Native immutable indices");
    ++resident_stats.uploads;resident_stats.vertex_bytes+=geometry->vertex_bytes;resident_stats.index_bytes+=geometry->index_bytes;
    return geometry;
}
bool submit_resident_draw(const ResidentMesh& geometry,const ResidentDraw& draw) {
    if(!geometry || !geometry->vertices || !geometry->indices)return false;
    if(frame_interp::enabled())throw std::runtime_error("Native resident geometry interpolation is not bound");
    auto key=draw.material.pipeline;key.primitive_topology=geometry->topology;
    if(!key.shader.tev_valid)throw std::runtime_error("Native resident material requires a compiled TEV recipe");
    gx::set_logical_viewport({draw.x,draw.y,draw.width,draw.height,draw.near_depth,draw.far_depth});
    gx::set_logical_scissor({draw.clip_x,draw.clip_y,draw.clip_width,draw.clip_height});
    const auto& material=draw.material;
    const bool multiple=std::popcount(material.texture_mask)>1;
    const uint32_t layoutMask=multiple?material.texture_mask:1u;
    BindGroupRef textureGroup=0;
    if(key.shader.textured) {
        if(!material.texture_mask)throw std::runtime_error("Native resident texture inputs are missing");
        std::array<TextureHandle,8> held;
        std::array<wgpu::Sampler,8> samplers;
        std::array<WGPUBindGroupEntry,16> entries{};uint32_t count=0;
        for(uint32_t t=0;t<8;++t)if(material.texture_mask&(1u<<t)) {
            const auto& image=material.textures[t];
            held[t]=resolve_texture_handle(image.address,image.size,image.format,image.width,image.height,
                image.data,image.available,image.has_tlut,image.tlut_address,image.tlut_format,image.tlut_entries,
                image.tlut_data,image.tlut_available);
            if(!held[t])throw std::runtime_error("Native resident texture is unresolved");
            samplers[t]=sampler_ref(sampler_descriptor(material.samplers[t],replacement_mips(held[t])));
            const uint32_t slot=multiple?t:0u;
            entries[count++]={.binding=slot*2,.textureView=held[t]->sampleTextureView.Get()};
            entries[count++]={.binding=slot*2+1,.sampler=samplers[t].Get()};
        }
        const WGPUBindGroupDescriptor descriptor{.label={"Native resident material",WGPU_STRLEN},
            .layout=texture_bind_group_layout(layoutMask).Get(),.entryCount=count,.entries=entries.data()};
        textureGroup=bind_group_ref(descriptor);
    }
    const auto& vertex=material.vertex;const auto& pixel=material.pixel;
    if(!staging_has_capacity(0,0,sizeof(vertex),sizeof(pixel))) {
        if(!segment_frame() || !staging_has_capacity(0,0,sizeof(vertex),sizeof(pixel)))
            throw std::runtime_error("Native pose/material exceeds an empty uniform segment");
    }
    // Static material shaders compile once, synchronously. A selected native
    // family never enters the generic interpreter shader while awaiting one.
    const PipelineConfig config{.version=GXCorePipelineConfigVersion,.key=key,.msaaSamples=get_sample_count()};
    const auto pipeline=resident_pipeline_ref(config);
    PipelineRef depthPipeline=0;
    if(needs_early_depth_emulation(key)) {
        auto depth=config;depth.depthOnly=1;depthPipeline=resident_pipeline_ref(depth);
    }
    const auto uniforms=push_uniform_dedup(g_vertexUniformCache,reinterpret_cast<const uint8_t*>(&vertex),sizeof(vertex));
    const auto pixels=push_uniform_dedup(g_pixelUniformCache,reinterpret_cast<const uint8_t*>(&pixel),sizeof(pixel));
    retain_resident_geometry(geometry);
    set_next_draw_flat(vertex.projection[3][3]==1.f);
    push_draw_command(DrawData{.pipeline=pipeline,.depthPipeline=depthPipeline,.uniformRange=uniforms,
        .pixelUniformRange=pixels,.indexCount=geometry->index_count,.textureBindGroup=textureGroup,
        .tev=true,.resident=geometry.get()});
    ++resident_stats.draws;return true;
}
