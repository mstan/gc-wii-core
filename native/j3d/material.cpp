// SPDX-License-Identifier: MIT
#include "material.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace gc_wii::j3d {
namespace gxc=gxruntime::gxcore;
namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
uint16_t u16(const uint8_t* p){return uint16_t(uint16_t(p[0])<<8|p[1]);}
float f32(const uint8_t* p) {
    const auto bits=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
    const float f=std::bit_cast<float>(bits);need(std::isfinite(f),"Nonfinite J3D material input");return f;
}
float reduced(float f){return std::bit_cast<float>(std::bit_cast<uint32_t>(f)&0xfffff000u);}
uint8_t raster(uint8_t ch) {
    constexpr uint8_t mapping[]{0,1,0,1,0,1,7,5,6};
    if(ch==255)return 7;
    need(ch<9,"Unsupported J3D raster channel");return mapping[ch];
}
void operation(uint8_t op,uint8_t bias,uint8_t scale,uint8_t& out_op,uint8_t& out_bias,uint8_t& out_scale) {
    need(op<2 || (op>=8 && op<=15),"Unsupported J3D combiner operation");
    if(op<2)need(bias<3 && scale<4,"Invalid J3D combiner arithmetic");
    out_op=op&1;out_bias=op>=8?3:bias;out_scale=op>=8?((op>>1)&3):scale;
}
void texture_stage(CompiledMaterial& c,uint8_t coord,uint8_t map) {
    need(coord<8 && coord<c.pipeline.shader.num_tex_gens && map<8,"Missing J3D texture coordinate or map");
    need(c.textures[map]!=65535,"Missing J3D texture binding");
    c.texture_mask|=1u<<map;
}
void fog(CompiledMaterial& c,const std::array<uint8_t,44>& recipe) {
    const auto* f=recipe.data();auto& key=c.pipeline.shader;auto& p=c.pixel;
    key.fog_fsel=f[0]&7;key.fog_proj=(f[0]>>3)&1;key.fog_range=f[1]!=0;
    if(!key.fog_fsel)return;
    need(key.fog_fsel==2 || key.fog_fsel>=4,"Unsupported J3D fog equation");
    // J3D's material loader uses the perspective coefficient construction for
    // every fog enum. Preserve that source behavior, including float20 input.
    const float start=f32(f+4),end=f32(f+8),nearz=f32(f+12),farz=f32(f+16);
    float a=0,b=.5f,z=0;
    if(farz!=nearz && end!=start){a=(farz*nearz)/((farz-nearz)*(end-start));b=farz/(farz-nearz);z=start/(end-start);}
    need(std::isfinite(a) && std::isfinite(b) && std::isfinite(z) && b>0,"Invalid J3D fog distances");
    int exponent=1;while(b>1.f && exponent<31){b*=.5f;++exponent;}
    while(b<.5f && exponent>-31){b*=2.f;--exponent;}
    need(exponent>=0 && exponent<31,"Unsupported J3D fog exponent");
    p.fogf[0]=reduced(std::ldexp(a,-exponent));p.fogf[1]=reduced(z);
    p.fogi[1]=int32_t(uint32_t(b*8388638.f)&0xffffffu);p.fogi[3]=exponent;
    for(unsigned i=0;i<3;++i)p.fogcolor[i]=f[20+i];
    c.fog_center=u16(f+2);p.fogf[3]=1;
    for(unsigned i=0;i<10;++i)p.fogrange[i/4][i%4]=float(u16(f+24+i*2)&4095)/64.f;
}
gxc::PlanSampler sampler(const Texture& t) {
    need(t.wrap_s<=2 && t.wrap_t<=2 && t.min_filter<=5 && t.mag_filter<=1,"Unsupported J3D sampler");
    // The current backend has no sampler LOD-bias field. Never discard it.
    need(t.lod_bias==0,"J3D sampler LOD bias needs native backend support");
    need(!t.edge_lod && !t.bias_clamp && t.anisotropy<=2,"J3D sampler LOD policy needs native backend support");
    need(t.min_lod>=0 && t.max_lod>=0,"Negative J3D sampler LOD");
    gxc::PlanSampler s{};s.wrap_s=t.wrap_s;s.wrap_t=t.wrap_t;s.mag_filter=t.mag_filter;
    s.min_filter=t.min_filter&1;s.mipmap_filter=t.min_filter<2?0:(t.min_filter<4?1:2);
    s.min_lod=uint8_t(std::min(255,int(t.min_lod)*2));s.max_lod=uint8_t(std::min(255,int(t.max_lod)*2));
    s.max_aniso=t.anisotropy;
    return s;
}
}
CompiledMaterial compile_material(const Material& m,const Mesh& mesh) {
    need(m.available,"J3D material has no native recipe");
    need(m.channels<=2 && m.texgens<=8 && !m.stages.empty() && m.stages.size()<=16,"Invalid J3D material program");
    need(m.cull<=3 || m.cull==255,"Invalid J3D culling");
    CompiledMaterial c;c.textures=m.textures;c.texture_matrix_slots.fill(255);
    auto& p=c.pipeline;auto& k=p.shader;
    p.cull_mode=m.cull==1?2:(m.cull==2?1:(m.cull==255?0:m.cull));c.inherited_cull=m.cull==255;
    need(m.depth_func<=7 && m.blend[0]<=3 && m.blend[1]<8 && m.blend[2]<8,"Invalid J3D pixel pipeline");
    p.depth_test=m.depth_test!=0;p.depth_update=m.depth_write!=0;p.depth_func=m.depth_func;
    p.early_depth_test=m.early_depth!=0;p.color_update=p.alpha_update=1;
    p.blend_enable=m.blend[0]==1 || m.blend[0]==3;p.blend_subtract=m.blend[0]==3;
    if(m.blend[0]==2)need(m.blend[3]==3,"J3D logic operation needs native backend support"); // COPY
    p.src_factor=m.blend[1];p.dst_factor=m.blend[2];
    p.src_factor_alpha=p.src_factor==2?6:(p.src_factor==3?7:p.src_factor);
    p.dst_factor_alpha=p.dst_factor==2?4:(p.dst_factor==3?5:p.dst_factor);
    need(m.alpha_compare[0]<8 && m.alpha_compare[3]<8 && m.alpha_compare[2]<4,"Invalid J3D alpha comparison");
    k.alpha_comp0=m.alpha_compare[0];k.alpha_comp1=m.alpha_compare[3];k.alpha_logic=m.alpha_compare[2];
    c.pixel.alpha_ref[0]=m.alpha_compare[1];c.pixel.alpha_ref[1]=m.alpha_compare[4];
    k.has_pos_mtx_idx=mesh.attribute_mask&1;k.tex_mtx_idx_mask=uint8_t(mesh.attribute_mask>>1);
    k.has_tex_mtx_idx=k.tex_mtx_idx_mask!=0;k.has_color0=(mesh.attribute_mask>>11)&1;k.has_color1=(mesh.attribute_mask>>12)&1;
    k.uv_mask=uint8_t(mesh.attribute_mask>>13);k.has_vertex_normal=mesh.normal_components!=0;
    k.has_vertex_binormal=k.has_vertex_tangent=mesh.normal_components==9;
    if(k.uv_mask&0xe0){need(!mesh.normal_components,"Native J3D vertex layout needs normals plus TEX5..7 support");k.raw_tex_hi_in_nbt=1;}
    k.lit_valid=1;k.chan_captured_mask=15;k.num_color_chans=m.channels;
    constexpr unsigned channel_order[]{0,2,1,3};
    for(unsigned i=0;i<4;++i) {
        const auto& src=m.lighting[channel_order[i]];auto& dst=k.litchan[i];
        need(src[1]<2 && src[4]<3 && src[3]<3 && src[5]<2,"Unsupported J3D lighting source");
        dst.enablelighting=src[0]!=0;dst.matsource=src[1];dst.ambsource=src[5];
        dst.diffusefunc=src[4]==0?0:src[3];dst.attnfunc=(src[4]!=2?1:0)|(src[4]!=0?2:0);
        dst.light_mask=dst.enablelighting?src[2]:0;
        if((i&1)<m.channels)c.light_mask|=dst.light_mask;
    }
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j){c.vertex.materials[i][j]=m.ambient[i][j];c.vertex.materials[i+2][j]=m.color[i][j];}
    k.num_tex_gens=m.texgens;
    for(unsigned i=0;i<m.texgens;++i) {
        const auto& src=m.texcoord_generators[i];auto& dst=k.tex_gens[i];dst.enabled=1;
        const auto type=src[0],source=src[1],matrix=src[2];
        if(type==10){need(source==19 || source==20,"Invalid J3D color texgen");dst.texgentype=source==19?2:3;dst.sourcerow=2;}
        else if(type>=2 && type<=9){need(source>=12 && source<=18,"Invalid J3D emboss source");dst.texgentype=1;dst.embosssourceshift=source-12;dst.embosslightshift=type-2;c.light_mask|=1u<<(type-2);}
        else {
            need(type<=1 && source<=11,"Unsupported J3D texgen");
            constexpr uint8_t row[]{0,1,3,4,5,6,7,8,9,10,11,12};
            dst.sourcerow=row[source];dst.inputform=source<=3;dst.projection=type==0;
        }
        auto* rows=c.vertex.texmatrices+i*3;rows[0][0]=rows[1][1]=rows[2][2]=1;
        if(matrix!=60 && type<=1 && !(k.tex_mtx_idx_mask&(1u<<i))) {
            need(matrix>=30 && matrix<=51 && (matrix-30)%3==0,"Unsupported J3D texture matrix selector");
            const auto slot=(matrix-30)/3;
            need((m.texture_matrix_mask&(1u<<slot))!=0,"Missing J3D texture matrix");
            c.texture_matrix_slots[i]=slot;c.texture_matrix_mask|=1u<<slot;
        }
    }
    k.tev_valid=1;k.num_tev_stages=uint8_t(m.stages.size());
    for(unsigned i=0;i<m.stages.size();++i) {
        const auto& src=m.stages[i];const auto& e=src.equation;auto& dst=k.tev_stages[i];
        need(e[1]<16 && e[2]<16 && e[3]<16 && e[4]<16 && e[10]<8 && e[11]<8 && e[12]<8 && e[13]<8,"Invalid J3D combiner input");
        need(e[8]<2 && e[9]<4 && e[17]<2 && e[18]<4 && src.konst_color<32 && src.konst_alpha<32,"Invalid J3D combiner output");
        dst.cc_a=e[1];dst.cc_b=e[2];dst.cc_c=e[3];dst.cc_d=e[4];
        operation(e[5],e[6],e[7],dst.cc_op,dst.cc_bias,dst.cc_scale);dst.cc_clamp=e[8];dst.cc_dest=e[9];
        dst.ac_a=e[10];dst.ac_b=e[11];dst.ac_c=e[12];dst.ac_d=e[13];
        operation(e[14],e[15],e[16],dst.ac_op,dst.ac_bias,dst.ac_scale);dst.ac_clamp=e[17];dst.ac_dest=e[18];
        dst.ksel_kc=src.konst_color;dst.ksel_ka=src.konst_alpha;dst.tevorders_colorchan=raster(src.raster);
        need(src.raster_swap<4 && src.texture_swap<4,"Invalid J3D swap selector");
        for(unsigned j=0;j<4;++j){dst.ras_swap[j]=m.swaps[src.raster_swap][j];dst.tex_swap[j]=m.swaps[src.texture_swap][j];need(dst.ras_swap[j]<4 && dst.tex_swap[j]<4,"Invalid J3D component swap");}
        if(src.texture!=255 && src.texcoord!=255) {
            texture_stage(c,src.texcoord,src.texture&7);dst.tevorders_enable=1;
            dst.tevorders_texcoord=src.texcoord;dst.tevorders_texmap=src.texture&7;
        }
    }
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j){c.pixel.colors[(i+1)%4][j]=m.registers[i][j];c.pixel.kcolors[i][j]=m.constants[i][j];}
    if(m.indirect[0]) {
        const auto* data=m.indirect.data();need(data[1]<=4,"Invalid J3D indirect stage count");k.num_ind_stages=data[1];
        for(unsigned i=0;i<k.num_ind_stages;++i) {
            auto& dst=k.ind_stages[i];const auto* order=data+4+i*4;const auto* scale=data+0x68+i*4;
            texture_stage(c,order[0],order[1]);dst.texcoord=order[0];dst.texmap=order[1];dst.scale_s=scale[0];dst.scale_t=scale[1];
            need(dst.scale_s<=8 && dst.scale_t<=8,"Invalid J3D indirect scale");
        }
        for(unsigned i=0;i<3;++i) {
            const auto* src=data+0x14+i*28;const auto exponent=static_cast<int8_t>(src[24]);
            for(unsigned r=0;r<2;++r)for(unsigned col=0;col<3;++col) {
                const float value=f32(src+(r*3+col)*4)*1024.f;
                need(value>=float(std::numeric_limits<int32_t>::min()) && double(value)<=double(std::numeric_limits<int32_t>::max()),"Unbounded J3D indirect coefficient");
                const auto bits=uint32_t(int32_t(value))&2047u;
                c.pixel.indtexmtx[i*2+r][col]=int32_t(bits^1024u)-1024;
            }
            c.pixel.indtexmtx[i*2][3]=c.pixel.indtexmtx[i*2+1][3]=17-((int(exponent)+17)&31);
        }
        for(unsigned i=0;i<m.stages.size();++i) {
            const auto* src=data+0x78+i*12;auto& dst=k.tev_stages[i];
            need(src[0]<4 && src[1]<4 && src[2]<8 && src[3]<12 && src[4]<7 && src[5]<7 && src[6]<2 && src[7]<2 && src[8]<4,"Invalid J3D indirect equation");
            dst.ind_stage=src[0];dst.ind_format=src[1];dst.ind_bias=src[2];dst.ind_matrix_index=src[3]&3;dst.ind_matrix_id=src[3]>>2;
            dst.ind_wrap_s=src[4];dst.ind_wrap_t=src[5];dst.ind_add_prev=src[6];dst.ind_use_original_lod=src[7];dst.ind_bump_alpha=src[8];
        }
    }
    k.textured=c.texture_mask!=0;fog(c,m.fog);return c;
}
void set_fog(CompiledMaterial& c,const std::array<uint8_t,44>& recipe){fog(c,recipe);}
gxc::DrawPlan compile_mesh(const Mesh& mesh,const Primitive& primitive) {
    need(mesh.vertices.size()<=65536,"Native J3D mesh needs index partitioning");
    need(primitive.first_index<=mesh.indices.size() && primitive.index_count<=mesh.indices.size()-primitive.first_index,"Invalid J3D topology span");
    gxc::DrawPlan plan;plan.vertex_count=uint32_t(mesh.vertices.size());plan.pipeline.primitive_topology=uint8_t(primitive.topology);
    plan.vertices.resize(mesh.vertices.size()*gxc::kVertexFloats);
    for(size_t i=0;i<mesh.vertices.size();++i) {
        const auto& src=mesh.vertices[i];auto* dst=plan.vertices.data()+i*gxc::kVertexFloats;
        std::copy(src.position.begin(),src.position.end(),dst+gxc::kVertexPosOffset/4);
        const uint32_t pm=src.position_matrix;std::memcpy(dst+gxc::kVertexPosMtxOffset/4,&pm,4);
        for(unsigned c=0;c<2;++c)std::copy(src.color[c].begin(),src.color[c].end(),dst+(c?gxc::kVertexColor1Offset:gxc::kVertexColor0Offset)/4);
        for(unsigned t=0;t<5;++t)std::copy(src.uv[t].begin(),src.uv[t].end(),dst+gxc::kVertexUvOffset/4+t*2);
        std::copy(src.normal.begin(),src.normal.end(),dst+gxc::kVertexNormalOffset/4);
        std::copy(src.binormal.begin(),src.binormal.end(),dst+gxc::kVertexBinormalOffset/4);
        std::copy(src.tangent.begin(),src.tangent.end(),dst+gxc::kVertexTangentOffset/4);
        if((mesh.attribute_mask&(0xe0u<<13)) && !mesh.normal_components) {
            const unsigned slots[]{gxc::kVertexNormalOffset,gxc::kVertexBinormalOffset,gxc::kVertexTangentOffset};
            for(unsigned t=0;t<3;++t)std::copy(src.uv[t+5].begin(),src.uv[t+5].end(),dst+slots[t]/4);
        }
        uint32_t lo=0,hi=0;for(unsigned t=0;t<4;++t){lo|=uint32_t(src.texture_matrices[t])<<(t*8);hi|=uint32_t(src.texture_matrices[t+4])<<(t*8);}
        std::memcpy(dst+gxc::kVertexTexMtxIdxOffset/4,&lo,4);std::memcpy(dst+gxc::kVertexTexMtxIdxHiOffset/4,&hi,4);
    }
    for(size_t i=primitive.first_index;i<size_t(primitive.first_index)+primitive.index_count;++i){need(mesh.indices[i]<mesh.vertices.size(),"Invalid native J3D vertex index");plan.indices.push_back(uint16_t(mesh.indices[i]));}
    return plan;
}
BoundMaterial bind_native_material(const CompiledMaterial& c,std::span<const Texture> textures,float width) {
    BoundMaterial out;out.pipeline=c.pipeline;out.vertex=c.vertex;out.pixel=c.pixel;out.texture_mask=c.texture_mask;
    const bool multiple=std::popcount(c.texture_mask)>1;
    if(c.pipeline.shader.fog_range){need(std::isfinite(width) && width>0,"Invalid native fog viewport");out.pixel.fogf[3]=width*2;out.pixel.fogf[2]=float(c.fog_center)/width-1.f;}
    for(unsigned t=0;t<8;++t)if(c.texture_mask&(1u<<t)) {
        need(c.textures[t]<textures.size(),"Native J3D texture index is unresolved");const auto& src=textures[c.textures[t]];
        need(!src.image.empty() && src.width && src.height,"Native J3D image is unresolved");
        need(src.palette_entries==0 || src.palette.size()==size_t(src.palette_entries)*2,"Native J3D palette is unresolved");
        out.samplers[t]=sampler(src);auto& dst=out.textures[t];dst.valid=true;
        dst.address=src.image_address;dst.size=dst.available=uint32_t(src.image.size());dst.format=src.format;
        dst.width=src.width;dst.height=src.height;dst.data=src.image.data();dst.has_tlut=src.palette_entries!=0;
        dst.tlut_address=src.palette_address;dst.tlut_format=src.palette_format;dst.tlut_entries=src.palette_entries;
        dst.tlut_data=src.palette.data();dst.tlut_available=uint32_t(src.palette.size());
        const auto uniform_slot=multiple?t:0;out.pixel.texdims[uniform_slot][0]=src.width;out.pixel.texdims[uniform_slot][1]=src.height;
    }
    // J3D assigns raster-coordinate scales in material-stage order, followed
    // by indirect orders. Several maps may intentionally share a coordinate.
    auto scale=[&](uint8_t coord,uint8_t map){const auto& t=textures[c.textures[map]];out.pixel.texdims[coord][2]=t.width;out.pixel.texdims[coord][3]=t.height;};
    for(unsigned i=0;i<c.pipeline.shader.num_tev_stages;++i){const auto& s=c.pipeline.shader.tev_stages[i];if(s.tevorders_enable)scale(s.tevorders_texcoord,s.tevorders_texmap);}
    for(unsigned i=0;i<c.pipeline.shader.num_ind_stages;++i){const auto& s=c.pipeline.shader.ind_stages[i];scale(s.texcoord,s.texmap);}
    return out;
}
void bind_material(const CompiledMaterial& c,std::span<const Texture> textures,gxc::DrawPlan& plan,float width) {
    const auto material=bind_native_material(c,textures,width);
    const auto topology=plan.pipeline.primitive_topology;plan.pipeline=material.pipeline;plan.pipeline.primitive_topology=topology;
    plan.constants=material.vertex;plan.pixel_constants=material.pixel;plan.constants_id=0;
    plan.has_texture=material.texture_mask!=0;plan.texmap_mask=std::popcount(material.texture_mask)>1?material.texture_mask:0;
    std::copy(material.textures.begin(),material.textures.end(),plan.textures);
    std::copy(material.samplers.begin(),material.samplers.end(),plan.samplers);
    for(unsigned t=0;t<8;++t)if(!plan.texmap_mask && (material.texture_mask&(1u<<t))) {
        const auto& dst=plan.textures[t];plan.tex_slot=t;plan.tex_address=dst.address;plan.tex_size=dst.size;plan.tex_format=dst.format;plan.tex_width=dst.width;plan.tex_height=dst.height;plan.tex_data=dst.data;plan.tex_available=dst.available;plan.has_tlut=dst.has_tlut;plan.tlut_address=dst.tlut_address;plan.tlut_format=dst.tlut_format;plan.tlut_entries=dst.tlut_entries;plan.tlut_data=dst.tlut_data;plan.tlut_available=dst.tlut_available;
    }
}
}
