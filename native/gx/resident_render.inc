// SPDX-License-Identifier: MIT
// Deferred worker encoding of a direct native draw. GPU geometry was uploaded
// at asset creation; there is no vertex staging, draw matching or batching.
static void render_resident(const DrawData& data,const wgpu::RenderPassEncoder& pass) {
    const auto bind=[&](PipelineRef pipeline) {
        if(!bind_pipeline(pipeline,pass))throw std::runtime_error("Native resident shader pipeline is unavailable");
        if(g_pass.pipeline!=pipeline) {
            g_pass.pipeline=pipeline;g_pass.group1=g_pass.group2=g_pass.group3=nullptr;
        }
    };
    const auto draw=[&](bool color) {
        const uint32_t vertexOffset=data.uniformRange.offset,pixelOffset=data.pixelUniformRange.offset;
        pass.SetBindGroup(1,g_uniformBindGroup,1,&vertexOffset);
        g_pass.group1=g_uniformBindGroup.Get();g_pass.offset1=vertexOffset;
        if(color) {
            pass.SetBindGroup(2,g_uniformBindGroup,1,&pixelOffset);
            g_pass.group2=g_uniformBindGroup.Get();g_pass.offset2=pixelOffset;
            if(data.textureBindGroup) {
                const auto& group=find_bind_group(data.textureBindGroup);pass.SetBindGroup(3,group);g_pass.group3=group.Get();
            }
        }
        pass.SetVertexBuffer(0,data.resident->vertices);
        g_pass.vertexBuffer=data.resident->vertices.Get();g_pass.vertexOffset=0;
        pass.SetIndexBuffer(data.resident->indices,wgpu::IndexFormat::Uint16);
        // A following staged GXCore draw must restore its own index buffer.
        g_pass.indexBound=false;
        pass.DrawIndexed(data.indexCount);
    };
    if(data.depthPipeline){bind(data.depthPipeline);draw(false);}
    bind(data.pipeline);draw(true);
}
