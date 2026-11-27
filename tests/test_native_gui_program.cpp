// SPDX-License-Identifier: MIT
#include "native_gui_program.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
}
int main(){try {
    gcnllr::NativeScene scene;GcWiiNativeVertex vertices[4]{};
    for(auto& v:vertices)for(float& c:v.color)c=1;
    const uint16_t indices[]{0,1,2,0,2,3};const GcWiiNativeMesh mesh{vertices,indices,4,6};
    float extra[16]{};const auto handle=scene.create_mesh(mesh,extra,2);require(handle!=0,"Retained three-UV mesh");
    GcWiiNativeGuiProgramDraw input{};input.draw.mesh=handle;
    input.draw.model_view[0]=input.draw.model_view[5]=input.draw.model_view[10]=1;
    input.draw.projection[0]=input.draw.projection[5]=input.draw.projection[10]=input.draw.projection[15]=1;
    input.draw.width=input.draw.clip_width=640;input.draw.height=input.draw.clip_height=480;input.draw.far_depth=1;
    input.texture_count=input.coordinate_count=3;input.stage_count=2;
    for(unsigned i=0;i<3;++i) {
        auto& t=input.textures[i];t.address=0x200000+i*0x10000;t.width=t.height=4;t.format=6;t.size=64;
        input.coordinate_source[i]=i;input.uv[i][0][0]=input.uv[i][1][1]=1;
    }
    input.colors[1][0]=-1;input.constants[2][1]=137;
    for(unsigned i=0;i<2;++i) {
        auto& s=input.stages[i];s.color={{2,4,8,15},0,0,0,1,0};s.alpha={{1,2,4,7},0,0,0,1,0};
        s.texture_enabled=1;s.texture=i?2:0;s.coordinate=i;s.color_constant=12;s.alpha_constant=28;
        for(unsigned c=0;c<4;++c)s.texture_swap[c]=s.raster_swap[c]=c;
    }
    auto* plan=gcnllr::prepare_gui_program(scene,input);require(plan!=nullptr,"Complete native program lowered");
    require(plan->pipeline.shader.num_tev_stages==2 && plan->pipeline.shader.num_tex_gens==3 && plan->pipeline.shader.uv_mask==7,"Shader graph and retained coordinates");
    require(plan->pipeline.shader.tev_stages[1].tevorders_texmap==2 && plan->pipeline.shader.tev_stages[1].tevorders_texcoord==1,"Texture and coordinate bindings are independent");
    require(plan->pixel_constants.colors[1][0]==-1 && plan->pixel_constants.kcolors[2][1]==137 && plan->textures[2].address==0x220000,"Signed colors and immutable texture bindings");
    const auto previous=plan->pipeline.shader;const auto rejected=scene.status().rejected;
    input.stages[1].alpha.inputs[0]=8;
    require(!gcnllr::prepare_gui_program(scene,input) && scene.status().rejected==rejected+1,"Invalid alpha selector rejected");
    require(!std::memcmp(&previous,&plan->pipeline.shader,sizeof previous),"Rejected program leaves prepared mesh state untouched");
    input.stages[1].alpha.inputs[0]=1;input.coordinate_source[2]=3;
    require(!gcnllr::prepare_gui_program(scene,input),"Unavailable retained UV rejected");input.coordinate_source[2]=2;
    input.colors[1][0]=-1025;require(!gcnllr::prepare_gui_program(scene,input),"Out-of-range signed shader color rejected");input.colors[1][0]=-1;
    input.stages[0].color_constant=8;require(!gcnllr::prepare_gui_program(scene,input),"Reserved constant selector rejected");input.stages[0].color_constant=12;
    input.stages[0].texture_enabled=0;input.stages[0].texture=input.stages[0].coordinate=255;
    plan=gcnllr::prepare_gui_program(scene,input);require(plan && !plan->pipeline.shader.tev_stages[0].tevorders_texmap && !plan->pipeline.shader.tev_stages[0].tevorders_texcoord,"Inactive texture indices are canonicalized");
    require(scene.destroy_mesh(handle) && !scene.uv_count(handle),"Original scene resource lifetime retained");
    std::cout<<"native GUI integer-program lowering PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
