// SPDX-License-Identifier: MIT
#include "native_gui.hpp"
#include <cmath>
namespace gcnllr {
gxruntime::gxcore::DrawPlan* prepare_gui(NativeScene& scene,const GcWiiNativeGuiDraw& input) {
    for(unsigned i=0;i<4;++i)
        if(input.color.low[i]<0 || input.color.low[i]>255 ||
           input.color.high[i]<0 || input.color.high[i]>255) {
            scene.rejected();return nullptr;
        }
    auto* plan=scene.prepare(input.draw);if(!plan)return nullptr;
    auto& shader=plan->pipeline.shader;
    shader.tev_valid=1;shader.num_tev_stages=plan->has_texture?2:1;
    if(!plan->has_texture)plan->pixel_constants={};
    for(unsigned i=0;i<4;++i) {
        plan->pixel_constants.colors[1][i]=input.color.low[i];
        plan->pixel_constants.colors[2][i]=input.color.high[i];
    }
    // Lower this named GUI expression through the existing integer shader
    // compiler. This is a complete material, with no guest state replay.
    for(unsigned i=0;i<shader.num_tev_stages;++i) {
        auto& stage=shader.tev_stages[i];stage={};
        stage.cc_clamp=stage.ac_clamp=1;
        for(unsigned c=0;c<4;++c)stage.ras_swap[c]=stage.tex_swap[c]=uint8_t(c);
    }
    auto& first=shader.tev_stages[0];
    if(plan->has_texture) {
        first.tevorders_enable=1;
        first.cc_a=2;first.cc_b=4;first.cc_c=8;first.cc_d=15;
        first.ac_a=1;first.ac_b=2;first.ac_c=4;first.ac_d=7;
    }
    auto& tint=shader.tev_stages[plan->has_texture?1:0];
    tint.cc_a=15;tint.cc_b=plan->has_texture?0:4;tint.cc_c=10;tint.cc_d=15;
    tint.ac_a=7;tint.ac_b=plan->has_texture?0:2;tint.ac_c=5;tint.ac_d=7;
    return plan;
}

gxruntime::gxcore::DrawPlan* prepare_gui_layers(NativeScene& scene,const GcWiiNativeGuiLayersDraw& input) {
    const auto reject=[&](){scene.rejected();return static_cast<gxruntime::gxcore::DrawPlan*>(nullptr);};
    if(!input.draw.material.texture.size)return reject();
    for(unsigned c=0;c<4;++c) {
        if(input.color.low[c]<0 || input.color.low[c]>255 || input.color.high[c]<0 || input.color.high[c]>255)return reject();
        for(unsigned i=0;i<2;++i)if(input.weights[i][c]<0 || input.weights[i][c]>255)return reject();
    }
    for(unsigned i=0;i<2;++i) {
        if(!input.overlays[i].size || !valid_native_texture(input.overlays[i]))return reject();
        for(const auto& row:input.uv[i])for(float f:row)if(!std::isfinite(f))return reject();
    }
    auto* plan=scene.prepare(input.draw);if(!plan)return nullptr;
    auto& shader=plan->pipeline.shader;
    shader.tev_valid=1;shader.num_tev_stages=3;shader.num_tex_gens=3;
    plan->texmap_mask=7;
    for(unsigned i=0;i<3;++i) {
        const auto& texture=i?input.overlays[i-1]:input.draw.material.texture;
        auto& binding=plan->textures[i];binding={};binding.valid=true;
        binding.address=texture.address;binding.size=texture.size;binding.format=texture.format;
        binding.width=texture.width;binding.height=texture.height;
        binding.has_tlut=texture.palette_entries!=0;binding.tlut_address=texture.palette_address;
        binding.tlut_format=texture.palette_format;binding.tlut_entries=texture.palette_entries;
        auto& sampler=plan->samplers[i];sampler={};
        sampler.wrap_s=uint8_t(texture.wrap_s);sampler.wrap_t=uint8_t(texture.wrap_t);
        sampler.min_filter=sampler.mag_filter=uint8_t(texture.linear);sampler.mipmap_filter=0;sampler.max_lod=0;
        plan->pixel_constants.texdims[i][0]=plan->pixel_constants.texdims[i][2]=int32_t(texture.width);
        plan->pixel_constants.texdims[i][1]=plan->pixel_constants.texdims[i][3]=int32_t(texture.height);
        auto& generator=shader.tex_gens[i];generator={};generator.enabled=1;
        generator.sourcerow=uint8_t(gxruntime::gxcore::TexSourceRow::Tex0);
        if(i) {
            auto* rows=plan->constants.texmatrices+i*3;
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)rows[r][c]=0;
            for(unsigned r=0;r<2;++r){rows[r][0]=input.uv[i-1][r][0];rows[r][1]=input.uv[i-1][r][1];rows[r][3]=input.uv[i-1][r][2];}
            rows[2][2]=1;
        }
        auto& stage=shader.tev_stages[i];stage={};
        stage.tevorders_enable=1;stage.tevorders_texcoord=stage.tevorders_texmap=uint8_t(i);
        stage.cc_clamp=stage.ac_clamp=1;
        for(unsigned c=0;c<4;++c)stage.ras_swap[c]=stage.tex_swap[c]=uint8_t(c);
        stage.cc_a=i?15:2;stage.cc_b=i==0?4:(i==1?6:14);stage.cc_c=8;stage.cc_d=i?0:15;
        stage.ksel_kc=12; // Overlay two uses the fourth typed color, held in K0.
        stage.ac_a=stage.ac_b=stage.ac_c=7;stage.ac_d=i?0:2;
    }
    for(unsigned c=0;c<4;++c) {
        plan->pixel_constants.colors[1][c]=input.color.low[c];
        plan->pixel_constants.colors[2][c]=input.color.high[c];
        plan->pixel_constants.colors[3][c]=input.weights[0][c];
        plan->pixel_constants.kcolors[0][c]=input.weights[1][c];
    }
    return plan;
}
}
