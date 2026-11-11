// SPDX-License-Identifier: MIT
// Resource layouts: zeldaret/tww J3DGraphLoader and J3DStruct.h (CC0-1.0).
// This importer reads source assets, not the machine's live FIFO.
#include "asset.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace gc_wii::j3d {
namespace {
struct Bytes {
    std::span<const uint8_t> data;
    size_t origin{};
    Bytes sub(size_t at,size_t size) const {
        if(at>data.size() || size>data.size()-at)throw std::runtime_error(
            "J3D resource range exceeds block: origin="+std::to_string(origin)+
            " block="+std::to_string(data.size())+" at="+std::to_string(at)+" bytes="+std::to_string(size));
        return {data.subspan(at,size),origin+at};
    }
    uint8_t u8(size_t at) const {return sub(at,1).data[0];}
    Bytes tail(size_t at) const {
        if(at>data.size())throw std::runtime_error("J3D table offset exceeds block");
        return sub(at,data.size()-at);
    }
    uint16_t u16(size_t at) const {auto s=sub(at,2).data;return uint16_t(s[0])<<8|s[1];}
    uint32_t u32(size_t at) const {return uint32_t(u16(at))<<16|u16(at+2);}
    float f32(size_t at) const {
        const float f=std::bit_cast<float>(u32(at));
        if(!std::isfinite(f))throw std::runtime_error("Non-finite J3D resource value");
        return f;
    }
    template<size_t N> std::array<uint8_t,N> bytes(size_t at) const {
        std::array<uint8_t,N> out;auto s=sub(at,N).data;std::copy(s.begin(),s.end(),out.begin());return out;
    }
};
struct Format {uint32_t count{},type{};uint8_t fraction{};bool valid{};};
struct Attribute {uint32_t attribute{},input{};};
uint32_t numeric_size(uint32_t type) {
    if(type<=1)return 1;
    if(type<=3)return 2;
    if(type==4)return 4;
    throw std::runtime_error("Unsupported J3D numeric vertex format");
}
float number(Bytes bytes,size_t at,const Format& f,bool normal=false) {
    if(f.type==4)return bytes.f32(at);
    int32_t value;
    switch(f.type) {
    case 0:value=bytes.u8(at);break;
    case 1:value=static_cast<int8_t>(bytes.u8(at));break;
    case 2:value=bytes.u16(at);break;
    case 3:value=static_cast<int16_t>(bytes.u16(at));break;
    default:throw std::runtime_error("Unsupported J3D numeric vertex type");
    }
    const auto fraction=normal ? (f.type==1?6u:14u):f.fraction;
    if(fraction>31)throw std::runtime_error("Invalid J3D vertex fraction");
    return std::ldexp(float(value),-int(fraction));
}
uint32_t color_size(uint32_t type) {
    constexpr uint32_t sizes[]{2,3,4,2,3,4};
    if(type>=6)throw std::runtime_error("Unsupported J3D vertex color format");
    return sizes[type];
}
std::array<float,4> color(Bytes bytes,size_t at,uint32_t type) {
    uint32_t rgba[4]{0,0,0,255};
    if(type==0) {
        const auto v=bytes.u16(at);const uint32_t r=(v>>11)&31,g=(v>>5)&63,b=v&31;
        rgba[0]=(r<<3)|(r>>2);rgba[1]=(g<<2)|(g>>4);rgba[2]=(b<<3)|(b>>2);
    } else if(type==1 || type==2 || type==5) {
        for(unsigned i=0;i<3;++i)rgba[i]=bytes.u8(at+i);
        if(type==5)rgba[3]=bytes.u8(at+3);
    } else if(type==3) {
        const auto v=bytes.u16(at);for(unsigned i=0;i<4;++i)rgba[i]=((v>>(12-4*i))&15)*17;
    } else if(type==4) {
        const auto v=(uint32_t(bytes.u8(at))<<16)|(uint32_t(bytes.u8(at+1))<<8)|bytes.u8(at+2);
        for(unsigned i=0;i<4;++i){const auto c=(v>>(18-6*i))&63;rgba[i]=(c<<2)|(c>>4);}
    } else throw std::runtime_error("Unsupported J3D vertex color format");
    return {rgba[0]/255.f,rgba[1]/255.f,rgba[2]/255.f,rgba[3]/255.f};
}
size_t element_size(uint32_t attr,const Format& f) {
    if(attr==11 || attr==12)return color_size(f.type);
    uint32_t n;
    if(attr==9)n=f.count==0?2:3;
    else if(attr==10)n=f.count==1?9:3;
    else if(attr>=13 && attr<=20)n=f.count==0?1:2;
    else throw std::runtime_error("Unsupported J3D vertex attribute");
    return n*numeric_size(f.type);
}
void decode_attribute(Vertex& v,uint32_t attr,Bytes bytes,size_t at,const Format& f,unsigned nbt_part=0) {
    if(attr==11 || attr==12){v.color[attr-11]=color(bytes,at,f.type);return;}
    const size_t stride=numeric_size(f.type);
    if(attr==9) {
        const auto n=f.count==0?2u:3u;
        for(unsigned i=0;i<n;++i)v.position[i]=number(bytes,at+i*stride,f);
    } else if(attr==10) {
        const auto n=f.count==1?9u:3u;
        for(unsigned i=0;i<n;++i) {
            auto& target=(nbt_part==1 || i/3==1)?v.binormal:((nbt_part==2 || i/3==2)?v.tangent:v.normal);
            target[i%3]=number(bytes,at+i*stride,f,true);
        }
    } else {
        const auto n=f.count==0?1u:2u;
        for(unsigned i=0;i<n;++i)v.uv[attr-13][i]=number(bytes,at+i*stride,f);
    }
}
void topology(Mesh& mesh,uint8_t opcode,uint32_t first,uint32_t count) {
    const auto kind=opcode&0xf8;
    Topology mode=Topology::Triangles;
    if(kind==0xa8 || kind==0xb0)mode=Topology::Lines;
    else if(kind==0xb8)mode=Topology::Points;
    const auto begin=uint32_t(mesh.indices.size());
    auto emit=[&](uint32_t a){mesh.indices.push_back(first+a);};
    if(kind==0x80) {
        for(uint32_t i=0;i+3<count;i+=4){emit(i);emit(i+1);emit(i+2);emit(i);emit(i+2);emit(i+3);}
    } else if(kind==0x90) {
        for(uint32_t i=0;i+2<count;i+=3){emit(i);emit(i+1);emit(i+2);}
    } else if(kind==0x98) {
        for(uint32_t i=2;i<count;++i){emit(i%2?i-1:i-2);emit(i%2?i-2:i-1);emit(i);}
    } else if(kind==0xa0) {
        for(uint32_t i=2;i<count;++i){emit(0);emit(i-1);emit(i);}
    } else if(kind==0xa8) {
        for(uint32_t i=0;i+1<count;i+=2){emit(i);emit(i+1);}
    } else if(kind==0xb0) {
        for(uint32_t i=1;i<count;++i){emit(i-1);emit(i);}
    } else if(kind==0xb8) {
        for(uint32_t i=0;i<count;++i)emit(i);
    } else throw std::runtime_error("Unsupported J3D geometry primitive");
    const auto added=uint32_t(mesh.indices.size())-begin;
    if(!added)return;
    if(!mesh.primitives.empty() && mesh.primitives.back().topology==mode)
        mesh.primitives.back().index_count+=added;
    else mesh.primitives.push_back({mode,begin,added});
}
void geometry(Model& out,Bytes vtx,Bytes shp) {
    size_t total_vertices=0;
    std::array<Format,26> formats{};std::array<Bytes,26> arrays{};
    size_t cursor=vtx.u32(8);
    for(unsigned n=0;n<26;++n,cursor+=16) {
        const auto attr=vtx.u32(cursor);if(attr==255)break;
        if(attr>=formats.size())throw std::runtime_error("Unknown J3D vertex array format");
        const auto target=attr==25?10:attr;
        if(formats[target].valid)throw std::runtime_error("Duplicate J3D vertex format");
        formats[target]={vtx.u32(cursor+4),vtx.u32(cursor+8),vtx.u8(cursor+12),true};
        const auto& format=formats[target];
        if((target==9 && format.count>1) || (target==10 && format.count>2) ||
            (target>=13 && target<=20 && format.count>1))throw std::runtime_error("Invalid J3D attribute component count");
        if(target==10 && format.type!=1 && format.type!=3 && format.type!=4)
            throw std::runtime_error("Unsupported J3D normal representation");
        if(n==25)throw std::runtime_error("Unterminated J3D vertex formats");
    }
    std::array<uint32_t,13> starts{};
    for(unsigned i=0;i<13;++i)starts[i]=vtx.u32(12+4*i);
    for(unsigned i=0;i<13;++i)if(starts[i]) {
        auto end=vtx.data.size();
        for(auto offset:starts)if(offset>starts[i])end=std::min(end,size_t(offset));
        const uint32_t attr=i==0?9:(i==1 || i==2?10:i+8);
        if(i==2 && !formats[10].valid)continue;
        if(i==2 && formats[10].count==0)continue;
        arrays[attr]=vtx.sub(starts[i],end-starts[i]);
    }
    out.shape_count=shp.u16(8);
    if(!out.shape_count || out.shape_count>4096)throw std::runtime_error("Invalid J3D shape count");
    const auto records=shp.u32(12),remap=shp.u32(16),descriptors=shp.u32(24);
    const auto palette=shp.u32(28),data=shp.u32(32),matrices=shp.u32(36),draws=shp.u32(40);
    for(auto offset:{records,remap,descriptors,palette,data,matrices,draws})shp.sub(offset,0);
    for(uint16_t shape=0;shape<out.shape_count;++shape) {
        const auto record=size_t(records)+size_t(shp.u16(remap+shape*2))*40;
        const auto groups=shp.u16(record+2);
        if(groups>4096)throw std::runtime_error("Unbounded J3D matrix groups");
        std::vector<Attribute> layout;
        auto desc=size_t(descriptors)+shp.u16(record+4);
        uint32_t mask=0;
        for(unsigned n=0;n<26;++n,desc+=8) {
            auto attr=shp.u32(desc);if(attr==255)break;if(attr==25)attr=10;
            const auto input=shp.u32(desc+4);
            if(attr>20 || input>3 || (mask&(1u<<attr)))throw std::runtime_error("Invalid J3D vertex descriptor");
            if(input){layout.push_back({attr,input});mask|=1u<<attr;}
            if(n==25)throw std::runtime_error("Unterminated J3D vertex descriptor");
        }
        if(!(mask&(1u<<9)))throw std::runtime_error("J3D shape has no position input");
        for(uint16_t group=0;group<groups;++group) {
            if(out.meshes.size()>=65536)throw std::runtime_error("Unbounded J3D mesh count");
            Mesh mesh;mesh.shape=shape;mesh.group=group;mesh.matrix_type=shp.u8(record);mesh.attribute_mask=mask;
            if(mesh.matrix_type>3)throw std::runtime_error("Unsupported J3D shape matrix type");
            const auto mi=size_t(matrices)+(size_t(shp.u16(record+6))+group)*8;
            if(mesh.matrix_type==3) {
                const auto count=shp.u16(mi+2);const auto first=shp.u32(mi+4);
                if(count>10)throw std::runtime_error("J3D matrix palette exceeds vertex matrix bank");
                const auto available=shp.tail(palette).data.size()/2;
                if(first>available || count>available-first)throw std::runtime_error("J3D matrix palette exceeds resource");
                for(unsigned i=0;i<count;++i)mesh.matrix_palette.push_back(shp.u16(size_t(palette)+(size_t(first)+i)*2));
            } else mesh.matrix_palette.push_back(shp.u16(mi));
            const auto di=size_t(draws)+(size_t(shp.u16(record+8))+group)*8;
            auto stream=shp.sub(size_t(data)+shp.u32(di+4),shp.u32(di));size_t at=0;
            while(at<stream.data.size()) {
                const auto opcode=stream.u8(at++);if(!opcode)continue;
                if(opcode<0x80 || opcode>0xbf)throw std::runtime_error("J3D shape contains non-geometry commands");
                const uint32_t count=stream.u16(at);at+=2;
                const auto first=uint32_t(mesh.vertices.size());
                if(count>1048576u-total_vertices)throw std::runtime_error("Unbounded J3D vertex resource");
                total_vertices+=count;
                for(uint32_t v=0;v<count;++v) {
                    Vertex vertex;
                    for(const auto& attribute:layout) {
                        const auto attr=attribute.attribute;
                        if(attr<=8) {
                            if(attribute.input!=1)throw std::runtime_error("Indexed J3D matrix index");
                            auto index=stream.u8(at++);
                            if(attr==0)vertex.position_matrix=index;else vertex.texture_matrices[attr-1]=index;
                            continue;
                        }
                        const auto& f=formats[attr];
                        if(!f.valid)throw std::runtime_error("J3D attribute has no format");
                        if(attribute.input==1) {
                            auto direct=f;
                            if(attr==10 && f.count==2)direct.count=1;
                            decode_attribute(vertex,attr,stream,at,direct);at+=element_size(attr,direct);
                        } else {
                            const unsigned n=(attr==10 && f.count==2)?3:1;
                            for(unsigned part=0;part<n;++part) {
                                const uint32_t index=attribute.input==2?stream.u8(at):stream.u16(at);
                                at+=attribute.input==2?1:2;
                                decode_attribute(vertex,attr,arrays[attr],size_t(index)*element_size(attr,f),f,part);
                            }
                        }
                    }
                    mesh.vertices.push_back(vertex);
                }
                topology(mesh,opcode,first,count);
            }
            out.meshes.push_back(std::move(mesh));
        }
    }
}
void hierarchy(Model& out,Bytes info) {
    auto at=size_t(info.u32(20));unsigned depth=0;
    for(size_t n=0;n<65536;++n,at+=4) {
        const auto op=info.u16(at),index=info.u16(at+2);
        if(!op){if(depth)throw std::runtime_error("Unbalanced J3D hierarchy");return;}
        if(op==1)++depth;
        else if(op==2){if(!depth)throw std::runtime_error("Unbalanced J3D hierarchy");--depth;}
        else if(op!=16 && op!=17 && op!=18)throw std::runtime_error("Unsupported J3D hierarchy node");
        out.hierarchy.push_back({op,index});
    }
    throw std::runtime_error("Unterminated J3D hierarchy");
}
void skeleton(Model& out,const std::map<std::string,Bytes>& blocks) {
    if(auto it=blocks.find("DRW1");it!=blocks.end()) {
        auto b=it->second;const auto count=b.u16(8);const auto flags=b.u32(12),indices=b.u32(16);
        for(unsigned i=0;i<count;++i){const auto f=b.u8(flags+i);if(f>1)throw std::runtime_error("Invalid J3D draw matrix type");out.draw_matrices.push_back({f!=0,b.u16(indices+i*2)});}
    }
    if(auto it=blocks.find("EVP1");it!=blocks.end()) {
        auto b=it->second;const auto count=b.u16(8);
        const auto counts=b.u32(12),indices=b.u32(16),weights=b.u32(20);
        size_t first=0;uint16_t max_joint=0;
        for(unsigned i=0;i<count;++i) {
            std::vector<Weight> envelope;
            for(unsigned n=0;n<b.u8(counts+i);++n) {
                const auto joint=b.u16(indices+(first+n)*2);const auto weight=b.f32(weights+(first+n)*4);
                if(weight<0)throw std::runtime_error("Negative J3D envelope weight");
                envelope.push_back({joint,weight});max_joint=std::max(max_joint,joint);
            }
            first+=envelope.size();
            if(first>1048576)throw std::runtime_error("Unbounded J3D envelope weights");
            out.envelopes.push_back(std::move(envelope));
        }
        const auto inverse=b.u32(24);
        if(inverse && first)for(unsigned j=0;j<=max_joint;++j) {
            std::array<float,12> matrix;for(unsigned k=0;k<12;++k)matrix[k]=b.f32(size_t(inverse)+j*48+k*4);
            out.inverse_bind.push_back(matrix);
        }
    }
    if(auto it=blocks.find("JNT1");it!=blocks.end()) {
        auto b=it->second;const auto count=b.u16(8);
        const auto records=b.u32(12),indices=b.u32(16);
        for(unsigned i=0;i<count;++i) {
            const auto record=size_t(records)+size_t(b.u16(indices+i*2))*64;
            Joint joint;joint.kind=b.u16(record);joint.compensate_scale=b.u8(record+2);
            for(unsigned k=0;k<3;++k){joint.scale[k]=b.f32(record+4+k*4);joint.rotation[k]=static_cast<int16_t>(b.u16(record+16+k*2));joint.translation[k]=b.f32(record+24+k*4);}
            out.joints.push_back(joint);
        }
    }
}
void materials(Model& out,Bytes b) {
    const auto count=b.u16(8);const auto records=b.u32(12),indices=b.u32(16);
    if(count>4096)throw std::runtime_error("Unbounded J3D material count");
    out.materials.resize(count);
    // Some BDL resources contain only precompiled MDL3 material programs.
    // Geometry remains importable; the renderer must reject unavailable
    // recipes until their asset-time program compiler has supplied them.
    if(!records)return;
    auto table=[&](size_t field,uint16_t index,size_t stride) {
        const auto start=b.u32(field);
        if(!start)throw std::runtime_error("Missing J3D material table");
        return b.tail(start).sub(size_t(index)*stride,stride);
    };
    for(unsigned i=0;i<count;++i) {
        const auto id=indices?b.u16(indices+i*2):uint16_t(i);
        const auto r=b.tail(records).sub(size_t(id)*332,332);
        auto& m=out.materials[i];m.mode=r.u8(0);
        m.textures.fill(65535);
        m.color.fill({255,255,255,255});m.ambient.fill({50,50,50,50});
        m.lighting.fill({0,0,0,2,2,0,255,255});
        m.blend={1,4,5,5};m.alpha_compare={7,0,0,7,0,255,255,255};
        for(auto& c:m.registers)c.fill(255);
        for(auto& c:m.constants)c.fill(255);
        for(auto& s:m.swaps)s={0,1,2,3};
        const auto cull=r.u8(1)==255?255u:table(28,r.u8(1),4).u32(0);
        if(cull>3 && cull!=255)throw std::runtime_error("Invalid J3D material culling");
        m.cull=uint8_t(cull);
        m.channels=r.u8(2)==255?0:table(36,r.u8(2),1).u8(0);
        m.texgens=r.u8(3)==255?0:table(52,r.u8(3),1).u8(0);
        const unsigned stages=r.u8(4)==255?255u:table(88,r.u8(4),1).u8(0);
        if(m.channels>2 || m.texgens>8 || !stages || stages>16)throw std::runtime_error("Invalid J3D material program size");
        m.early_depth=r.u8(5)==255?0:table(120,r.u8(5),1).u8(0);
        m.dither=r.u8(7)==255?1:table(124,r.u8(7),1).u8(0);
        m.depth_test=1;m.depth_func=3;m.depth_write=1;
        if(r.u8(6)!=255){auto z=table(116,r.u8(6),4);m.depth_test=z.u8(0);m.depth_func=z.u8(1);m.depth_write=z.u8(2);}
        for(unsigned j=0;j<2;++j) {
            auto index=r.u16(8+j*2);if(index!=65535)m.color[j]=table(32,index,4).bytes<4>(0);
            index=r.u16(20+j*2);if(index!=65535)m.ambient[j]=table(44,index,4).bytes<4>(0);
        }
        for(unsigned j=0;j<4;++j) {
            auto index=r.u16(12+j*2);if(index!=65535)m.lighting[j]=table(40,index,8).bytes<8>(0);
            index=r.u16(148+j*2);if(index!=65535)m.constants[j]=table(84,index,4).bytes<4>(0);
            index=r.u16(220+j*2);if(index!=65535){auto c=table(80,index,8);for(unsigned k=0;k<4;++k)m.registers[j][k]=static_cast<int16_t>(c.u16(k*2));}
            index=r.u16(292+j*2);if(index!=65535)m.swaps[j]=table(100,index,4).bytes<4>(0);
        }
        for(unsigned j=0;j<8;++j) {
            auto index=r.u16(132+j*2);if(index!=65535)m.textures[j]=table(72,index,2).u16(0);
            index=r.u16(40+j*2);
            m.texcoord_generators[j]={1,uint8_t(4+j),60,255};
            if(index!=65535)m.texcoord_generators[j]=table(56,index,4).bytes<4>(0);
            index=r.u16(72+j*2);if(index!=65535){m.texture_matrices[j]=table(64,index,100).bytes<100>(0);m.texture_matrix_mask|=1u<<j;}
        }
        constexpr std::array<uint8_t,20> default_equation{4,10,15,15,0,0,0,0,1,0,5,7,7,0,0,0,0,1,0,0};
        for(unsigned j=0;j<stages;++j) {
            MaterialStage stage;stage.equation=default_equation;
            auto index=r.u16(228+j*2);if(index!=65535)stage.equation=table(92,index,20).bytes<20>(0);
            index=r.u16(188+j*2);if(index!=65535){auto order=table(76,index,4);stage.texcoord=order.u8(0);stage.texture=order.u8(1);stage.raster=order.u8(2);}
            stage.konst_color=r.u8(156+j)==255?12:r.u8(156+j);
            stage.konst_alpha=r.u8(172+j)==255?28:r.u8(172+j);
            index=r.u16(260+j*2);if(index!=65535){auto swap=table(96,index,4);stage.raster_swap=swap.u8(0);stage.texture_swap=swap.u8(1);}
            if(stage.raster_swap>3 || stage.texture_swap>3)throw std::runtime_error("Invalid J3D material channel swap");
            m.stages.push_back(stage);
        }
        auto index=r.u16(324);if(index!=65535)m.fog=table(104,index,44).bytes<44>(0);
        index=r.u16(326);if(index!=65535)m.alpha_compare=table(108,index,8).bytes<8>(0);
        index=r.u16(328);if(index!=65535)m.blend=table(112,index,4).bytes<4>(0);
        // Unlike material init records, indirect recipes use the logical
        // material number, not the remapped shared init-record number.
        const auto indirect=b.u32(24);if(indirect)m.indirect=b.tail(indirect).bytes<312>(size_t(i)*312);
        m.available=true;
    }
}
size_t texture_level_bytes(uint32_t format,uint32_t width,uint32_t height) {
    uint32_t x=4,y=4,bytes=32;
    switch(format) {
    case 0:case 8:case 14:x=y=8;break;
    case 1:case 2:case 9:x=8;break;
    case 3:case 4:case 5:case 10:break;
    case 6:bytes=64;break;
    default:throw std::runtime_error("Unsupported J3D texture storage format");
    }
    return size_t((width+x-1)/x)*((height+y-1)/y)*bytes;
}
void textures(Model& out,Bytes b,const ImportOptions& options) {
    const auto count=b.u16(8);const auto records=b.u32(12);
    if(count>4096)throw std::runtime_error("Unbounded J3D texture count");
    b.sub(records,size_t(count)*32);
    size_t copied=0;
    for(unsigned i=0;i<count;++i) {
        const auto at=size_t(records)+i*32;const auto r=b.sub(at,32);Texture t;
        t.format=r.u8(0);t.alpha=r.u8(1);t.width=r.u16(2);t.height=r.u16(4);
        t.wrap_s=r.u8(6);t.wrap_t=r.u8(7);t.palette_format=r.u8(9);t.palette_entries=r.u16(10);
        t.min_filter=r.u8(20);t.mag_filter=r.u8(21);t.min_lod=static_cast<int8_t>(r.u8(22));
        t.max_lod=static_cast<int8_t>(r.u8(23));t.mip_count=std::max<uint8_t>(1,r.u8(24));
        t.lod_bias=static_cast<int16_t>(r.u16(26));
        if(!t.width || !t.height || t.width>1024 || t.height>1024 || t.mip_count>11)
            throw std::runtime_error("Invalid J3D texture dimensions or mip levels");
        size_t image_bytes=0;auto w=uint32_t(t.width),h=uint32_t(t.height);
        for(unsigned mip=0;mip<t.mip_count;++mip){image_bytes+=texture_level_bytes(t.format,w,h);w=std::max(1u,w/2);h=std::max(1u,h/2);}
        t.external=r.u32(28)==0;
        auto resource_bytes=[&](uint32_t relative,size_t size,uint32_t& address) {
            const auto offset=uint64_t(at)+relative;
            address=options.resolve ? options.guest_address+uint32_t(b.origin+at)+relative:0;
            if(offset<=b.data.size() && size<=b.data.size()-size_t(offset))return b.sub(size_t(offset),size).data;
            if(options.resolve) {
                const auto result=options.resolve(options.user,address,size);
                if(result.size()!=size)throw std::runtime_error("External J3D resource did not resolve completely");
                t.external=true;return result;
            }
            throw std::runtime_error("External J3D resource requires a checked consumer resolver");
        };
        if(!t.external) {
            auto image=resource_bytes(r.u32(28),image_bytes,t.image_address);
            t.image.assign(image.begin(),image.end());copied+=image.size();
        }
        if(t.palette_entries && r.u32(12)) {
            if(t.palette_format>2)throw std::runtime_error("Invalid J3D palette format");
            auto palette=resource_bytes(r.u32(12),size_t(t.palette_entries)*2,t.palette_address);
            t.palette.assign(palette.begin(),palette.end());copied+=palette.size();
        }
        if(copied>128u*1024u*1024u)throw std::runtime_error("Unbounded imported J3D texture memory");
        out.textures.push_back(std::move(t));
    }
}
}

Model import_model(std::span<const uint8_t> resource,const ImportOptions& options) {
    Bytes file{resource};
    if(file.u32(0)!=0x4a334432 || (file.u32(4)!=0x626d6433 && file.u32(4)!=0x62646c34))
        throw std::runtime_error("Unsupported J3D model version");
    const auto size=file.u32(8),count=file.u32(12);
    if(size!=resource.size() || size>64u*1024u*1024u || !count || count>32)
        throw std::runtime_error("Invalid J3D resource size or section count");
    std::map<std::string,Bytes> blocks;size_t at=32;
    for(unsigned i=0;i<count;++i) {
        auto header=file.sub(at,8);const auto length=header.u32(4);
        if(length<8)throw std::runtime_error("Invalid J3D section length");
        std::string tag(reinterpret_cast<const char*>(header.data.data()),4);
        if(!blocks.emplace(tag,file.sub(at,length)).second)throw std::runtime_error("Duplicate J3D section");
        at+=length;
    }
    if(at!=resource.size())throw std::runtime_error("J3D sections do not cover resource");
    if(!blocks.contains("VTX1") || !blocks.contains("SHP1") || !blocks.contains("INF1"))
        throw std::runtime_error("J3D model lacks geometry or hierarchy");
    Model out;geometry(out,blocks.at("VTX1"),blocks.at("SHP1"));
    hierarchy(out,blocks.at("INF1"));skeleton(out,blocks);
    if(auto it=blocks.find("MAT3");it!=blocks.end())materials(out,it->second);
    if(auto it=blocks.find("TEX1");it!=blocks.end())textures(out,it->second,options);
    return out;
}
}
