// SPDX-License-Identifier: MIT
#include "native_gui_effects.hpp"
#include <cmath>
namespace gcnllr {
namespace {
bool byte_color(const GcWiiNativeGuiMaterial& color) {
    for(unsigned c=0;c<4;++c)if(color.low[c]<0 || color.low[c]>255 || color.high[c]<0 || color.high[c]>255)return false;
    return true;
}
void stage(gxruntime::gxcore::TevStageKey& value) {
    value={};value.cc_clamp=value.ac_clamp=1;
    for(unsigned c=0;c<4;++c)value.ras_swap[c]=value.tex_swap[c]=uint8_t(c);
}
void range(gxruntime::gxcore::DrawPlan& plan,const GcWiiNativeGuiMaterial& color) {
    for(unsigned c=0;c<4;++c){plan.pixel_constants.colors[1][c]=color.low[c];plan.pixel_constants.colors[2][c]=color.high[c];}
    auto& first=plan.pipeline.shader.tev_stages[0];first.tevorders_enable=1;
    first.cc_a=2;first.cc_b=4;first.cc_c=8;first.cc_d=15;
}
}
gxruntime::gxcore::DrawPlan* prepare_gui_cutout(NativeScene& scene,const GcWiiNativeGuiCutoutDraw& input) {
    if(input.cutoff>255 || !input.image.draw.material.texture.size || !byte_color(input.image.color)){scene.rejected();return nullptr;}
    auto* plan=scene.prepare(input.image.draw);if(!plan)return nullptr;
    auto& shader=plan->pipeline.shader;shader.tev_valid=1;shader.num_tev_stages=2;
    stage(shader.tev_stages[0]);stage(shader.tev_stages[1]);range(*plan,input.image.color);
    auto& first=shader.tev_stages[0];first.ac_a=4;first.ac_b=6;first.ac_c=4;first.ac_d=7;
    first.ac_bias=3;first.ac_op=0;first.ac_scale=3;first.ksel_ka=28;
    plan->pixel_constants.kcolors[0][3]=int32_t(input.cutoff);
    auto& alpha=shader.tev_stages[1];alpha.cc_a=alpha.cc_b=alpha.cc_c=15;alpha.cc_d=0;
    alpha.ac_a=7;alpha.ac_b=5;alpha.ac_c=0;alpha.ac_d=7;
    return plan;
}
gxruntime::gxcore::DrawPlan* prepare_gui_add(NativeScene& scene,const GcWiiNativeGuiAddDraw& input) {
    if(!input.image.draw.material.texture.size || !input.addition.size || !valid_native_texture(input.addition) || !byte_color(input.image.color)){scene.rejected();return nullptr;}
    for(int32_t value:input.weight)if(value<0 || value>255){scene.rejected();return nullptr;}
    for(const auto& row:input.uv)for(float f:row)if(!std::isfinite(f)){scene.rejected();return nullptr;}
    auto* plan=scene.prepare(input.image.draw);if(!plan)return nullptr;
    auto& shader=plan->pipeline.shader;shader.tev_valid=1;shader.num_tev_stages=2;shader.num_tex_gens=2;plan->texmap_mask=3;
    bind_gui_texture(*plan,0,input.image.draw.material.texture,nullptr);bind_gui_texture(*plan,1,input.addition,input.uv);
    stage(shader.tev_stages[0]);stage(shader.tev_stages[1]);range(*plan,input.image.color);
    auto& first=shader.tev_stages[0];first.ac_a=first.ac_b=first.ac_c=7;first.ac_d=5;
    auto& added=shader.tev_stages[1];added.tevorders_enable=1;added.tevorders_texcoord=added.tevorders_texmap=1;
    added.cc_a=15;added.cc_b=6;added.cc_c=8;added.cc_d=0;
    added.ac_a=added.ac_b=added.ac_c=7;added.ac_d=0;
    for(unsigned c=0;c<4;++c)plan->pixel_constants.colors[3][c]=input.weight[c];
    return plan;
}
gxruntime::gxcore::DrawPlan* prepare_gui_product(NativeScene& scene,const GcWiiNativeGuiProductDraw& input) {
    if(input.secondary_uv>1 || (input.secondary_uv && scene.uv_count(input.image.draw.mesh)<2)){scene.rejected();return nullptr;}
    if(input.secondary_uv)for(const auto& row:input.primary_uv)for(float f:row)if(!std::isfinite(f)){scene.rejected();return nullptr;}
    auto* plan=prepare_gui_mask(scene,input.image);if(!plan)return nullptr;
    auto& shader=plan->pipeline.shader;shader.tev_stages[0].ac_d=4;
    if(input.secondary_uv){bind_gui_texture(*plan,0,input.image.draw.material.texture,input.primary_uv);shader.uv_mask|=2;shader.tex_gens[1].sourcerow=uint8_t(gxruntime::gxcore::TexSourceRow::Tex0)+1;}
    if(input.image.opacity!=255) {
        shader.num_tev_stages=3;auto& opacity=shader.tev_stages[2];stage(opacity);
        opacity.cc_a=opacity.cc_b=opacity.cc_c=15;opacity.cc_d=0;
        opacity.ac_a=7;opacity.ac_b=0;opacity.ac_c=1;opacity.ac_d=7;
    }
    return plan;
}
}
