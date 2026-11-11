// SPDX-License-Identifier: MIT
#include "asset.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace gc_wii::j3d;
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void put16(std::vector<uint8_t>& b,size_t at,uint16_t v){b.at(at)=v>>8;b.at(at+1)=v&255;}
void put32(std::vector<uint8_t>& b,size_t at,uint32_t v){put16(b,at,v>>16);put16(b,at+2,v&65535);}
void putf(std::vector<uint8_t>& b,size_t at,float v){put32(b,at,std::bit_cast<uint32_t>(v));}
uint32_t get32(const std::vector<uint8_t>& b,size_t at){return uint32_t(b.at(at))<<24|uint32_t(b.at(at+1))<<16|uint32_t(b.at(at+2))<<8|b.at(at+3);}
std::vector<uint8_t> block(const char* tag,size_t size){std::vector<uint8_t>b(size);std::copy_n(tag,4,b.begin());put32(b,4,uint32_t(size));return b;}
std::vector<uint8_t> triangle() {
    auto info=block("INF1",32);put32(info,20,24);put16(info,24,18);
    auto vtx=block("VTX1",160);put32(vtx,8,64);put32(vtx,12,112);put32(vtx,24,148);
    put32(vtx,64,9);put32(vtx,68,1);put32(vtx,72,4);
    put32(vtx,80,11);put32(vtx,84,1);put32(vtx,88,5);put32(vtx,96,255);
    constexpr float positions[]{-1,-1,.5f,1,-1,.5f,0,1,.5f};
    for(unsigned i=0;i<9;++i)putf(vtx,112+i*4,positions[i]);
    for(unsigned i=0;i<3;++i){vtx[148+i*4+i]=255;vtx[151+i*4]=255;}
    auto shp=block("SHP1",144);put16(shp,8,1);
    for(auto [field,value]:{std::pair{12,44},std::pair{16,84},std::pair{24,88},
        std::pair{28,112},std::pair{32,116},std::pair{36,128},std::pair{40,136}})put32(shp,field,value);
    put16(shp,46,1);put32(shp,88,9);put32(shp,92,3);put32(shp,96,11);put32(shp,100,2);put32(shp,104,255);
    shp[116]=0x90;put16(shp,117,3);
    for(unsigned i=0;i<3;++i){put16(shp,119+i*3,uint16_t(i));shp[121+i*3]=uint8_t(i);}
    put16(shp,130,1);put32(shp,136,12);
    std::vector<uint8_t> file(32);std::copy_n("J3D2bmd3",8,file.begin());put32(file,12,3);
    for(const auto* b:{&info,&vtx,&shp})file.insert(file.end(),b->begin(),b->end());
    put32(file,8,uint32_t(file.size()));return file;
}
void unit() {
    auto data=triangle();auto model=import_model(data);
    require(model.shape_count==1 && model.meshes.size()==1,"Triangle mesh count");
    const auto& m=model.meshes[0];
    require(m.vertices.size()==3 && m.indices==std::vector<uint32_t>({0,1,2}),"Triangle topology");
    require(m.vertices[0].position==std::array<float,3>({-1,-1,.5f}),"Indexed positions");
    require(m.vertices[1].color[0]==std::array<float,4>({0,1,0,1}),"Independent indexed vertex color");
    require(m.matrix_palette==std::vector<uint16_t>({0}),"Shape matrix palette");
    auto corrupt=data;put32(corrupt,36,7);
    bool rejected=false;try{(void)import_model(corrupt);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"Short section accepted");
    corrupt=data;put16(corrupt,32+32+160+119,65535);
    rejected=false;try{(void)import_model(corrupt);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"Out-of-bounds index accepted");
    std::fill(data.begin(),data.end(),0);
    require(m.vertices[0].position[0]==-1,"Imported mesh borrowed asset storage");
    std::cout<<"ROM-free native geometry, ownership and malformed-resource checks passed\n";
}
void report(const Model& model,size_t offset) {
    size_t vertices=0,indices=0,programs=0;
    for(const auto& mesh:model.meshes){vertices+=mesh.vertices.size();indices+=mesh.indices.size();}
    for(const auto& material:model.materials)programs+=material.available;
    std::cout<<"model offset="<<offset<<" meshes="<<model.meshes.size()<<" vertices="<<vertices
        <<" indices="<<indices<<" material_recipes="<<programs<<" materials="<<model.materials.size()
        <<" textures="<<model.textures.size()<<" joints="<<model.joints.size()<<'\n';
}
}
int main(int argc,char** argv) {
    try {
        unit();if(argc==1)return 0;
        std::ifstream input(argv[1],std::ios::binary);require(bool(input),"Resource fixture missing");
        const std::vector<uint8_t> data{std::istreambuf_iterator<char>(input),{}};
        if(argc>2 && std::string(argv[2])=="--ram") {
            size_t passed=0,failed=0;
            for(size_t at=0;at+32<data.size();++at) {
                if(!std::equal(data.begin()+at,data.begin()+at+4,"J3D2"))continue;
                const auto version=get32(data,at+4),size=get32(data,at+8);
                if((version!=0x626d6433 && version!=0x62646c34) || size<32 || size>data.size()-at)continue;
                try {
                    ImportOptions options;options.guest_address=0x80000000u+uint32_t(at);
                    options.user=const_cast<std::vector<uint8_t>*>(&data);
                    options.resolve=[](void* user,uint32_t address,size_t length)->std::span<const uint8_t> {
                        const auto& ram=*static_cast<const std::vector<uint8_t>*>(user);
                        const auto offset=address&0x1fffffffu;
                        if(offset>ram.size() || length>ram.size()-offset)return {};
                        return std::span(ram).subspan(offset,length);
                    };
                    auto model=import_model(std::span(data).subspan(at,size),options);report(model,at);++passed;
                }
                catch(const std::exception& e){std::cout<<"FAILED offset="<<at<<" reason="<<e.what()<<'\n';++failed;}
            }
            std::cout<<"models_passed="<<passed<<" models_failed="<<failed<<'\n';return !passed || failed?1:0;
        }
        auto model=import_model(data);report(model,0);
        if(argc>=3) {
            std::ofstream output(argv[2],std::ios::binary);require(bool(output),"Could not write mesh comparison");
            for(const auto& mesh:model.meshes)for(const auto& vertex:mesh.vertices) {
                output.write(reinterpret_cast<const char*>(vertex.position.data()),12);
                output.write(reinterpret_cast<const char*>(vertex.normal.data()),12);
                output.write(reinterpret_cast<const char*>(vertex.uv[0].data()),8);
            }
        }
        if(argc==4) {
            std::ofstream output(argv[3],std::ios::binary);require(bool(output),"Could not write material comparison");
            for(uint32_t i=0;i<model.materials.size();++i)for(uint32_t j=0;j<model.materials[i].stages.size();++j) {
                const auto& stage=model.materials[i].stages[j];
                output.write(reinterpret_cast<const char*>(&i),4);output.write(reinterpret_cast<const char*>(&j),4);
                output.write(reinterpret_cast<const char*>(stage.equation.data()),stage.equation.size());
                output.put(char(stage.raster_swap));output.put(char(stage.texture_swap));
            }
        }
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
