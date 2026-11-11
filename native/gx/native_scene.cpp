// SPDX-License-Identifier: MIT
#include "native_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gcnllr {
namespace gxc = gxruntime::gxcore;

static bool valid_texture(const GcWiiNativeTexture& t) {
    if (!t.size) return true;
    if (!t.width || !t.height || t.width>1024 || t.height>1024 ||
        t.wrap_s>2 || t.wrap_t>2 || t.linear>1) return false;
    uint32_t block_width=4,block_height=4,block_bytes=32;
    switch (t.format) {
    case 0: case 8: case 14: block_width=block_height=8; break;
    case 1: case 2: case 9: block_width=8; break;
    case 3: case 4: case 5: case 10: break;
    case 6: block_bytes=64; break;
    default: return false;
    }
    const auto bytes=((t.width+block_width-1)/block_width)*
        ((t.height+block_height-1)/block_height)*block_bytes;
    if (t.size!=bytes) return false; // this material has one mip level
    if (t.format>=8 && t.format<=10) {
        const uint32_t entries=t.format==8 ? 16u:(t.format==9 ? 256u:16384u);
        if (t.palette_entries<entries || t.palette_entries>16384 || t.palette_format>2) return false;
    } else if (t.palette_entries) return false;
    return true;
}

uint64_t NativeScene::create_mesh(const GcWiiNativeMesh& input) {
    if (!input.vertices || !input.indices || !input.vertex_count ||
        input.vertex_count > 65536u || !input.index_count ||
        input.index_count > 1048576u || input.index_count % 3u ||
        next_handle_ == std::numeric_limits<uint64_t>::max()) {
        rejected(); return 0;
    }
    for (uint32_t i=0;i<input.index_count;++i)
        if (input.indices[i]>=input.vertex_count) { rejected(); return 0; }
    for (uint32_t i=0;i<input.vertex_count;++i) {
        const auto& v=input.vertices[i];
        for (float f:v.position) if (!std::isfinite(f)) { rejected(); return 0; }
        for (float f:v.color) if (!std::isfinite(f) || f<0.f || f>1.f) { rejected(); return 0; }
        for (float f:v.uv) if (!std::isfinite(f)) { rejected(); return 0; }
    }
    gxc::DrawPlan plan;
    plan.vertex_count=input.vertex_count;
    plan.vertices.resize(size_t(input.vertex_count)*gxc::kVertexFloats);
    plan.indices.assign(input.indices,input.indices+input.index_count);
    for (uint32_t i=0;i<input.vertex_count;++i) {
        const auto& source=input.vertices[i];
        auto* out=plan.vertices.data()+size_t(i)*gxc::kVertexFloats;
        std::copy_n(source.position,3,out+gxc::kVertexPosOffset/4);
        std::copy_n(source.color,4,out+gxc::kVertexColor0Offset/4);
        std::copy_n(source.uv,2,out+gxc::kVertexUvOffset/4);
    }
    const auto handle=next_handle_++;
    meshes_.emplace(handle,std::move(plan));
    ++status_.mesh_uploads;
    status_.live_meshes=meshes_.size();
    status_.uploaded_vertex_bytes+=uint64_t(input.vertex_count)*sizeof(GcWiiNativeVertex);
    return handle;
}

bool NativeScene::destroy_mesh(uint64_t handle) {
    if (!meshes_.erase(handle)) { rejected(); return false; }
    status_.live_meshes=meshes_.size();
    return true;
}

gxc::DrawPlan* NativeScene::prepare(const GcWiiNativeDraw& job) {
    const auto found=meshes_.find(job.mesh);
    const auto& texture=job.material.texture;
    if (found==meshes_.end() || (job.material.flags & ~127u) ||
        ((job.material.flags&(GC_WII_NATIVE_COLOR_WRITE|GC_WII_NATIVE_ALPHA_WRITE)) &&
         !(job.material.flags&GC_WII_NATIVE_WRITE_MASKS)) ||
        !std::isfinite(job.x) || !std::isfinite(job.y) ||
        !std::isfinite(job.width) || !std::isfinite(job.height) ||
        !std::isfinite(job.near_depth) || !std::isfinite(job.far_depth) ||
        job.width<=0 || job.height<=0 || job.clip_width<=0 || job.clip_height<=0 ||
        job.near_depth<0 || job.far_depth>1 || job.near_depth>job.far_depth ||
        !valid_texture(texture)) {
        rejected(); return nullptr;
    }
    for (float f:job.model_view) if (!std::isfinite(f)) { rejected(); return nullptr; }
    for (float f:job.projection) if (!std::isfinite(f)) { rejected(); return nullptr; }
    auto& plan=found->second;
    plan.pipeline={};
    plan.pipeline.shader.has_color0=1;
    plan.pipeline.shader.num_color_chans=1;
    const bool explicit_masks=(job.material.flags&GC_WII_NATIVE_WRITE_MASKS)!=0;
    plan.pipeline.color_update=!explicit_masks || (job.material.flags&GC_WII_NATIVE_COLOR_WRITE);
    plan.pipeline.alpha_update=!explicit_masks || (job.material.flags&GC_WII_NATIVE_ALPHA_WRITE);
    plan.pipeline.blend_enable=(job.material.flags&GC_WII_NATIVE_BLEND)!=0;
    plan.pipeline.src_factor=plan.pipeline.src_factor_alpha=4;
    plan.pipeline.dst_factor=plan.pipeline.dst_factor_alpha=5;
    plan.pipeline.depth_test=(job.material.flags&GC_WII_NATIVE_DEPTH_TEST)!=0;
    plan.pipeline.depth_update=(job.material.flags&GC_WII_NATIVE_DEPTH_WRITE)!=0;
    plan.pipeline.depth_func=uint8_t((job.material.flags&GC_WII_NATIVE_DEPTH_ALWAYS)
        ? gxc::CompareMode::Always:gxc::CompareMode::LEqual);
    std::memcpy(plan.constants.posnormalmatrix,job.model_view,sizeof(job.model_view));
    std::memcpy(plan.constants.projection,job.projection,sizeof(job.projection));
    plan.constants_id=0;
    plan.viewport_valid=true;
    // Internal backend convention only; no GX command is constructed.
    plan.viewport[0]=job.width*.5f; plan.viewport[1]=-job.height*.5f;
    plan.viewport[2]=(job.far_depth-job.near_depth)*16777215.f;
    plan.viewport[3]=job.x+342.f+job.width*.5f;
    plan.viewport[4]=job.y+342.f+job.height*.5f;
    plan.viewport[5]=job.far_depth*16777215.f;
    plan.scissor_valid=true;
    plan.scissor_x=job.clip_x; plan.scissor_y=job.clip_y;
    plan.scissor_width=job.clip_width; plan.scissor_height=job.clip_height;
    plan.has_texture=texture.size!=0;
    plan.texmap_mask=0; // backend's flat single-texture representation
    plan.tex_slot=0;
    plan.tex_address=texture.address; plan.tex_size=texture.size;
    plan.tex_format=texture.format; plan.tex_width=texture.width; plan.tex_height=texture.height;
    plan.has_tlut=texture.palette_entries!=0;
    plan.tlut_address=texture.palette_address; plan.tlut_entries=texture.palette_entries;
    plan.tlut_format=texture.palette_format;
    if (plan.has_texture) {
        auto& shader=plan.pipeline.shader;
        shader.textured=1; shader.num_tex_gens=1; shader.uv_mask=1;
        shader.tev_valid=1; shader.num_tev_stages=1;
        auto& stage=shader.tev_stages[0];
        stage.tevorders_enable=1;
        // One specialized texture * vertex-color material, not an emulated
        // register sequence. The shared shader compiler lowers this expression.
        stage.cc_a=15; stage.cc_b=8; stage.cc_c=10; stage.cc_d=15;
        stage.ac_a=7; stage.ac_b=4; stage.ac_c=5; stage.ac_d=7;
        stage.cc_clamp=stage.ac_clamp=1;
        for (uint8_t i=0;i<4;++i) stage.ras_swap[i]=stage.tex_swap[i]=i;
        shader.tex_gens[0].enabled=1;
        shader.tex_gens[0].sourcerow=uint8_t(gxc::TexSourceRow::Tex0);
        plan.pixel_constants={};
        plan.pixel_constants.texdims[0][0]=plan.pixel_constants.texdims[0][2]=int32_t(texture.width);
        plan.pixel_constants.texdims[0][1]=plan.pixel_constants.texdims[0][3]=int32_t(texture.height);
        plan.constants.texmatrices[0][0]=1.f;
        plan.constants.texmatrices[1][1]=1.f;
        auto& sampler=plan.samplers[0];
        sampler={}; sampler.wrap_s=uint8_t(texture.wrap_s); sampler.wrap_t=uint8_t(texture.wrap_t);
        sampler.min_filter=sampler.mag_filter=uint8_t(texture.linear);
        sampler.mipmap_filter=0; sampler.max_lod=0;
    }
    plan.ok=true;
    return &plan;
}

void NativeScene::reset() {
    meshes_.clear(); status_={};
    // Keep the monotonic handle identity: a stale handle from a previous
    // service lifetime must never select a newly uploaded mesh.
}
}
