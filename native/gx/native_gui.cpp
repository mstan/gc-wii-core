// SPDX-License-Identifier: MIT
#include "native_gui.hpp"
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
}
