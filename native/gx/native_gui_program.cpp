// SPDX-License-Identifier: MIT
#include "native_gui_program.hpp"
#include <cmath>
namespace gcnllr {
namespace {
bool combine(const GcWiiNativeGuiCombine& c,bool alpha) {
    for(auto v:c.inputs)if(v>(alpha?7:15))return false;
    return c.operation<=1 && c.bias<=3 && c.scale<=3 && c.clamp<=1 && c.output<=3;
}
bool constant(unsigned v,bool alpha) {return v<=7 || v>=(alpha?16u:12u);}
}
gxruntime::gxcore::DrawPlan* prepare_gui_program(NativeScene& scene,const GcWiiNativeGuiProgramDraw& input) {
    const auto reject=[&](){scene.rejected();return static_cast<gxruntime::gxcore::DrawPlan*>(nullptr);};
    if(input.texture_count>8 || input.coordinate_count>8 || !input.stage_count || input.stage_count>16)return reject();
    for(const auto& row:input.colors)for(auto v:row)if(v<-1024 || v>1023)return reject();
    for(const auto& row:input.constants)for(auto v:row)if(v<0 || v>255)return reject();
    for(unsigned i=0;i<input.texture_count;++i)if(!input.textures[i].size || !valid_native_texture(input.textures[i]))return reject();
    const auto uv_count=scene.uv_count(input.draw.mesh);
    for(unsigned i=0;i<input.coordinate_count;++i) {
        if(input.coordinate_source[i]>=uv_count)return reject();
        for(const auto& row:input.uv[i])for(float v:row)if(!std::isfinite(v))return reject();
    }
    for(unsigned i=0;i<input.stage_count;++i) {
        const auto& s=input.stages[i];
        if(!combine(s.color,false) || !combine(s.alpha,true) || s.texture_enabled>1 || (s.raster!=0 && s.raster!=7) ||
           s.color_constant>31 || s.alpha_constant>31 || !constant(s.color_constant,false) || !constant(s.alpha_constant,true))return reject();
        if(s.texture_enabled && (s.texture>=input.texture_count || s.coordinate>=input.coordinate_count))return reject();
        for(auto v:s.texture_swap)if(v>3)return reject();
        for(auto v:s.raster_swap)if(v>3)return reject();
    }
    auto draw=input.draw;draw.material.texture=input.texture_count?input.textures[0]:GcWiiNativeTexture{};
    auto* plan=scene.prepare(draw);if(!plan)return nullptr;
    auto& shader=plan->pipeline.shader;
    shader.tev_valid=1;shader.num_tev_stages=uint8_t(input.stage_count);
    shader.textured=input.texture_count!=0;shader.num_tex_gens=uint8_t(input.coordinate_count);shader.uv_mask=0;
    plan->texmap_mask=input.texture_count?(1u<<input.texture_count)-1u:0;
    for(unsigned i=0;i<input.texture_count;++i)bind_gui_texture(*plan,i,input.textures[i],nullptr);
    for(unsigned i=0;i<8;++i)shader.tex_gens[i]={};
    for(unsigned i=0;i<input.coordinate_count;++i) {
        shader.uv_mask|=1u<<input.coordinate_source[i];auto& gen=shader.tex_gens[i];gen.enabled=1;
        gen.sourcerow=uint8_t(gxruntime::gxcore::TexSourceRow::Tex0)+input.coordinate_source[i];
        auto* rows=plan->constants.texmatrices+i*3;
        for(unsigned r=0;r<3;++r)for(float& v:rows[r])v=0;
        for(unsigned r=0;r<2;++r){rows[r][0]=input.uv[i][r][0];rows[r][1]=input.uv[i][r][1];rows[r][3]=input.uv[i][r][2];}
        rows[2][2]=1;
    }
    for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c){plan->pixel_constants.colors[i][c]=input.colors[i][c];plan->pixel_constants.kcolors[i][c]=input.constants[i][c];}
    for(unsigned i=0;i<16;++i)shader.tev_stages[i]={};
    for(unsigned i=0;i<input.stage_count;++i) {
        const auto& s=input.stages[i];auto& out=shader.tev_stages[i];
        out.cc_a=s.color.inputs[0];out.cc_b=s.color.inputs[1];out.cc_c=s.color.inputs[2];out.cc_d=s.color.inputs[3];
        out.cc_op=s.color.operation;out.cc_bias=s.color.bias;out.cc_scale=s.color.scale;out.cc_clamp=s.color.clamp;out.cc_dest=s.color.output;
        out.ac_a=s.alpha.inputs[0];out.ac_b=s.alpha.inputs[1];out.ac_c=s.alpha.inputs[2];out.ac_d=s.alpha.inputs[3];
        out.ac_op=s.alpha.operation;out.ac_bias=s.alpha.bias;out.ac_scale=s.alpha.scale;out.ac_clamp=s.alpha.clamp;out.ac_dest=s.alpha.output;
        out.tevorders_enable=s.texture_enabled;out.tevorders_texmap=s.texture_enabled?s.texture:0;
        out.tevorders_texcoord=s.texture_enabled?s.coordinate:0;out.tevorders_colorchan=s.raster;
        out.ksel_kc=s.color_constant;out.ksel_ka=s.alpha_constant;
        for(unsigned c=0;c<4;++c){out.tex_swap[c]=s.texture_swap[c];out.ras_swap[c]=s.raster_swap[c];}
    }
    return plan;
}
}
