// SPDX-License-Identifier: MIT
#include "native_j3d.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace gcnllr {
namespace j3d=gc_wii::j3d;
namespace gxc=gxruntime::gxcore;
namespace {
void need(bool b,const char* why){if(!b)throw std::runtime_error(why);}
void finite(const float* p,size_t n){for(size_t i=0;i<n;++i)need(std::isfinite(p[i]),"Nonfinite native J3D transform");}
}
uint64_t NativeJ3D::create(std::span<const uint8_t> bytes,const j3d::ImportOptions& options) {
    need(next_handle_!=std::numeric_limits<uint64_t>::max(),"Native J3D resource handles exhausted");
    Resource r;r.model=j3d::import_model(bytes,options);r.meshes.resize(r.model.meshes.size());
    for(size_t m=0;m<r.model.meshes.size();++m)for(const auto& primitive:r.model.meshes[m].primitives) {
        const auto compiled=j3d::compile_mesh(r.model.meshes[m],primitive);
        r.meshes[m].push_back(aurora::gfx::gxcore::create_resident_geometry(compiled.vertices,compiled.indices,compiled.pipeline.primitive_topology));
    }
    const auto handle=next_handle_++;status_.meshes+=r.meshes.size();resources_.emplace(handle,std::move(r));
    status_.resources=resources_.size();return handle;
}
bool NativeJ3D::destroy(uint64_t handle) {
    auto found=resources_.find(handle);if(found==resources_.end())return false;
    status_.meshes-=found->second.meshes.size();resources_.erase(found);status_.resources=resources_.size();return true;
}
NativeJ3D::Prepared* NativeJ3D::prepare(const GcWiiJ3DDraw& input) {
    auto found=resources_.find(input.resource);need(found!=resources_.end(),"Unknown native J3D resource");auto& r=found->second;
    need(input.mesh<r.meshes.size() && input.material<r.model.materials.size(),"Invalid native J3D mesh/material");
    const auto& mesh=r.model.meshes[input.mesh];const auto key=std::pair{input.material,input.mesh};
    auto material=r.materials.find(key);
    if(material==r.materials.end())material=r.materials.emplace(key,j3d::compile_material(r.model.materials[input.material],mesh)).first;
    auto c=material->second;const auto& shader=c.pipeline.shader;
    need((input.material_inputs&~1023u)==0,"Unknown native J3D material input");
    need(!(input.material_inputs&GC_WII_J3D_THREE_REGISTER_COLORS) || (input.material_inputs&GC_WII_J3D_REGISTER_COLORS),"Three-register mask requires register color inputs");
    need(!(input.material_inputs&(GC_WII_J3D_COLOR_WRITE|GC_WII_J3D_ALPHA_WRITE)) || (input.material_inputs&GC_WII_J3D_WRITE_MASKS),"Write enables require explicit native J3D write masks");
    if(input.material_inputs&GC_WII_J3D_WRITE_MASKS) {
        c.pipeline.color_update=bool(input.material_inputs&GC_WII_J3D_COLOR_WRITE);
        c.pipeline.alpha_update=bool(input.material_inputs&GC_WII_J3D_ALPHA_WRITE);
    }
    need((input.material_color_mask&~15u)==0,"Invalid native J3D material color mask");
    for(unsigned i=0;i<4;++i)for(unsigned ch=0;ch<4;++ch) {
        if((input.material_inputs&GC_WII_J3D_MATERIAL_COLORS) && (!input.material_color_mask || (input.material_color_mask&(1u<<i)))){auto v=input.material_colors[i][ch];need(v>=0 && v<=255,"Invalid native J3D material color");c.vertex.materials[i][ch]=v;}
        if((input.material_inputs&GC_WII_J3D_REGISTER_COLORS) && (i<3 || !(input.material_inputs&GC_WII_J3D_THREE_REGISTER_COLORS))){auto v=input.register_colors[i][ch];need(v>=-1024 && v<=1023,"Invalid native J3D register color");c.pixel.colors[(i+1)%4][ch]=v;}
        if(input.material_inputs&GC_WII_J3D_CONSTANT_COLORS){auto v=input.constant_colors[i][ch];need(v>=0 && v<=255,"Invalid native J3D constant color");c.pixel.kcolors[i][ch]=v;}
    }
    if(input.material_inputs&GC_WII_J3D_TEXTURE_INDICES)std::copy_n(input.texture_indices,8,c.textures.begin());
    if(input.material_inputs&GC_WII_J3D_LIGHTING) {
        need(input.channel_count<=2,"Invalid native J3D channel count");c.pipeline.shader.num_color_chans=uint8_t(input.channel_count);
        c.light_mask=0;
        for(unsigned i=0;i<4;++i) {
            const auto& src=input.channels[i];auto& dst=c.pipeline.shader.litchan[i];
            need(src.enabled<2 && src.material_source<2 && src.ambient_source<2 && src.diffuse<3 && src.attenuation<4,"Invalid native J3D lighting channel");
            dst.enablelighting=src.enabled;dst.matsource=src.material_source;dst.ambsource=src.ambient_source;
            dst.diffusefunc=src.diffuse;dst.attnfunc=src.attenuation;dst.light_mask=src.enabled?src.light_mask:0;
            if((i&1)<input.channel_count)c.light_mask|=dst.light_mask;
        }
        for(unsigned i=0;i<c.pipeline.shader.num_tex_gens;++i)if(c.pipeline.shader.tex_gens[i].texgentype==1)c.light_mask|=1u<<c.pipeline.shader.tex_gens[i].embosslightshift;
    }
    if(input.material_inputs&GC_WII_J3D_FOG){std::array<uint8_t,44> recipe;std::copy_n(input.fog_recipe,44,recipe.begin());j3d::set_fog(c,recipe);}
    need((input.position_mask&~1023u)==0 && (input.normal_mask&~1023u)==0 &&
        (input.texture_matrix_mask&~255u)==0 && (input.light_mask&~255u)==0,"Invalid native J3D dynamic input mask");
    uint32_t positions=1;
    if(shader.has_pos_mtx_idx) {
        positions=0;for(const auto& v:mesh.vertices){need(v.position_matrix<30 && v.position_matrix%3==0,"Invalid J3D position matrix row");positions|=1u<<(v.position_matrix/3);}
    }
    need((input.position_mask&positions)==positions,"Missing native J3D pose matrix");
    bool normal_used=false;for(unsigned i=0;i<4;++i)if((i&1)<shader.num_color_chans)normal_used|=shader.litchan[i].enablelighting!=0;
    for(unsigned i=0;i<shader.num_tex_gens;++i)normal_used|=shader.tex_gens[i].texgentype==1 || shader.tex_gens[i].sourcerow==1 || shader.tex_gens[i].sourcerow==3 || shader.tex_gens[i].sourcerow==4;
    if(normal_used)need((input.normal_mask&positions)==positions,"Missing native J3D normal matrix");
    need((input.light_mask&c.light_mask)==c.light_mask,"Missing native J3D light");
    uint32_t required_texture=c.texture_matrix_mask;
    for(unsigned i=0;i<8;++i)if(shader.tex_mtx_idx_mask&(1u<<i))for(const auto& v:mesh.vertices) {
        const auto row=v.texture_matrices[i];if(row==60)continue;
        need(row>=30 && row<=51 && (row-30)%3==0,"Unsupported indexed J3D texture matrix row");required_texture|=1u<<((row-30)/3);
    }
    need((input.texture_matrix_mask&required_texture)==required_texture,"Missing native J3D animated texture matrix");
    finite(input.projection,16);
    const float view[]{input.x,input.y,input.width,input.height,input.near_depth,input.far_depth};finite(view,6);
    need(input.width>0 && input.height>0 && input.near_depth>=0 && input.far_depth<=1 && input.near_depth<=input.far_depth && input.clip_width>0 && input.clip_height>0,"Invalid native J3D view");
    if(c.inherited_cull)need(input.cull_mode<=3,"Missing native J3D caller cull mode");
    for(unsigned i=0;i<10;++i){if(positions&(1u<<i))finite(input.positions[i],12);if(normal_used && (positions&(1u<<i)))finite(input.normals[i],9);}
    for(unsigned i=0;i<8;++i) {
        if(required_texture&(1u<<i))finite(input.texture_matrices[i],12);
        if(c.light_mask&(1u<<i)){const auto& light=input.lights[i];finite(light.cosatt,4);finite(light.distatt,4);finite(light.position,4);finite(light.direction,4);for(auto value:light.color)need(value>=0 && value<=255,"Invalid native J3D light color");}
    }
    finite(input.cached_normal,3);finite(input.cached_binormal,3);finite(input.cached_tangent,3);
    prepared_.meshes=r.meshes[input.mesh];
    prepared_.draw.material=j3d::bind_native_material(c,r.model.textures,input.width);
    auto& bound=prepared_.draw.material;auto& vertex=bound.vertex;
        if(c.inherited_cull)bound.pipeline.cull_mode=input.cull_mode==1?2:(input.cull_mode==2?1:input.cull_mode);
        std::memcpy(vertex.projection,input.projection,sizeof(input.projection));
        std::memcpy(vertex.posnormalmatrix,input.positions[0],sizeof(input.positions[0]));
        for(unsigned row=0;row<3;++row)std::copy_n(input.normals[0]+row*3,3,vertex.posnormalmatrix[3+row]);
        for(unsigned i=0;i<10;++i)if(positions&(1u<<i)) {
            std::memcpy(vertex.transformmatrices+i*3,input.positions[i],sizeof(input.positions[i]));
            for(unsigned row=0;row<3;++row)std::copy_n(input.normals[i]+row*3,3,vertex.normalmatrices[i*3+row]);
        }
        for(unsigned i=0;i<8;++i) {
            if(required_texture&(1u<<i))std::memcpy(vertex.transformmatrices+30+i*3,input.texture_matrices[i],sizeof(input.texture_matrices[i]));
            if(c.texture_matrix_slots[i]!=255)std::memcpy(vertex.texmatrices+i*3,input.texture_matrices[c.texture_matrix_slots[i]],sizeof(input.texture_matrices[0]));
            if(c.light_mask&(1u<<i)) {
                const auto& src=input.lights[i];auto& dst=vertex.lights[i];
                std::copy_n(src.color,4,dst.color);std::copy_n(src.cosatt,4,dst.cosatt);std::copy_n(src.distatt,4,dst.distatt);
                std::copy_n(src.position,4,dst.pos);std::copy_n(src.direction,4,dst.dir);
            }
        }
        // Indexed identity texture matrices name row 60 in this backend's
        // transform bank, rather than a resource-owned animated matrix.
        for(unsigned row=0;row<3;++row)vertex.transformmatrices[60+row][row]=1;
        std::copy_n(input.cached_normal,3,vertex.cached_normal);std::copy_n(input.cached_binormal,3,vertex.cached_binormal);std::copy_n(input.cached_tangent,3,vertex.cached_tangent);
    auto& draw=prepared_.draw;draw.x=input.x;draw.y=input.y;draw.width=input.width;draw.height=input.height;
    draw.near_depth=input.near_depth;draw.far_depth=input.far_depth;
    draw.clip_x=input.clip_x;draw.clip_y=input.clip_y;draw.clip_width=input.clip_width;draw.clip_height=input.clip_height;
    return &prepared_;
}
void NativeJ3D::rejected(const char* reason){++status_.rejected;std::snprintf(status_.error,sizeof(status_.error),"%s",reason);}
void NativeJ3D::reset(){
    const auto stats=aurora::gfx::gxcore::resident_geometry_stats();
    if(stats.uploads)std::fprintf(stderr,"[native-resident-geometry] uploads=%llu vertex-bytes=%llu index-bytes=%llu draws=%llu draw-vertex-staging=0\n",
        (unsigned long long)stats.uploads,(unsigned long long)stats.vertex_bytes,(unsigned long long)stats.index_bytes,(unsigned long long)stats.draws);
    prepared_={};resources_.clear();status_={};
}
}
