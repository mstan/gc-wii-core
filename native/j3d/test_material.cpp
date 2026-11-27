// SPDX-License-Identifier: MIT
#include "material.hpp"
#include <bit>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace gc_wii::j3d;
namespace {
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
Material material() {
    Material m;m.available=true;m.channels=1;m.texgens=1;m.textures.fill(65535);m.textures[3]=0;
    m.color.fill({255,255,255,255});m.lighting.fill({0,1,0,0,2,0,255,255});
    m.texcoord_generators[0]={1,4,60,255};m.alpha_compare={7,0,0,7,0,255,255,255};
    for(auto& s:m.swaps)s={0,1,2,3};
    MaterialStage s;s.equation={0,15,8,10,15,0,0,0,1,0,7,4,5,7,0,0,0,1,0,0};
    s.texcoord=0;s.texture=3;s.raster=4;m.stages.push_back(s);return m;
}
void unit() {
    Mesh mesh;mesh.attribute_mask=(1u<<9)|(1u<<11)|(1u<<13);mesh.vertices.resize(3);
    mesh.vertices[0].position={-1,-1,.5};mesh.vertices[1].position={1,-1,.5};mesh.vertices[2].position={0,1,.5};
    mesh.indices={0,1,2};mesh.primitives.push_back({Topology::Triangles,0,3});
    auto m=material();auto compiled=compile_material(m,mesh);auto plan=compile_mesh(mesh,mesh.primitives[0]);
    Texture t;t.format=1;t.width=8;t.height=4;t.image.resize(32,255);t.image_address=0x100;
    bind_material(compiled,std::span(&t,1),plan,640);
    const auto& k=plan.pipeline.shader;require(k.tev_valid && k.num_tev_stages==1,"Missing native equation");
    require(k.tev_stages[0].cc_b==8 && k.tev_stages[0].cc_c==10,"Texture/color equation changed");
    require(k.tev_stages[0].tevorders_texmap==3 && plan.tex_slot==3 && !plan.texmap_mask,"Nonzero single texture map");
    require(plan.tex_data==t.image.data() && plan.pixel_constants.texdims[0][0]==8,"Native texture ownership/binding");
    require(plan.pixel_constants.texdims[0][2]==8 && k.litchan[0].matsource==1,"Coordinate scale / vertex material");
    m.stages[0].equation[5]=14;m.stages[0].equation[14]=15;
    m.stages[0].equation[6]=m.stages[0].equation[15]=3; // ignored by compare operations
    auto compare=compile_material(m,mesh);
    require(compare.pipeline.shader.tev_stages[0].cc_bias==3 && compare.pipeline.shader.tev_stages[0].cc_scale==3 && compare.pipeline.shader.tev_stages[0].ac_op==1,"Compare equations");
    m=material();m.lighting[2]={1,0,8,2,1,1,255,255};m.channels=2;
    auto lit=compile_material(m,mesh);require(lit.light_mask==8 && lit.pipeline.shader.litchan[1].light_mask==8 && !lit.pipeline.shader.litchan[2].light_mask,"Lighting channel permutation");
    m=material();m.texcoord_generators[0][2]=30;m.texture_matrix_mask=1;
    auto matrix=compile_material(m,mesh);require(matrix.texture_matrix_mask==1 && matrix.texture_matrix_slots[0]==0,"Lost animated texture matrix requirement");
    m=material();m.blend={2,1,0,6};bool rejected=false;
    try{(void)compile_material(m,mesh);}catch(const std::exception&){rejected=true;}
    require(rejected,"Unsupported logic operation silently accepted");
    t.lod_bias=100;rejected=false;try{bind_material(compiled,std::span(&t,1),plan,640);}catch(const std::exception&){rejected=true;}
    require(rejected,"Sampler bias silently discarded");
    t.lod_bias=0;t.edge_lod=1;rejected=false;try{bind_material(compiled,std::span(&t,1),plan,640);}catch(const std::exception&){rejected=true;}
    require(rejected,"Sampler LOD policy silently discarded");
    std::cout<<"Native material equations, lighting, texture binding and explicit gaps passed\n";
}
}
int main(int argc,char** argv) {
    try {
        unit();if(argc==1)return 0;
        std::ifstream input(argv[1],std::ios::binary);require(bool(input),"Missing model fixture");
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(input),{}};auto model=import_model(data);
        size_t count=0;
        for(const auto& mesh:model.meshes)for(const auto& m:model.materials)if(m.available) {
            auto c=compile_material(m,mesh);
            for(const auto& primitive:mesh.primitives){auto plan=compile_mesh(mesh,primitive);bind_material(c,model.textures,plan,640);++count;}
        }
        std::cout<<"native_model_material_mesh_combinations="<<count<<'\n';return count?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
