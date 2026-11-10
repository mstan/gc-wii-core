# SPDX-License-Identifier: MIT
"""Compose a private, GPL-preserving native retail Aurora command owner.

The donor checkout stays immutable. Its existing renderer/decoder supplies the
operation; our adaptation adds partial-input framing and machine-owned effects.
"""
from pathlib import Path
import argparse

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('Pinned Aurora retail adapter differs: '+old[:90])
    return text.replace(old,new,1)

def compose(text,root):
    text=once(text,'return (reg & 0x001FFFFFu) << 5;',
        'return (reg & (GC_WII_GX_WII ? 0x00FFFFFFu : 0x001FFFFFu)) << 5;')
    text=once(text,'#include "command_processor.hpp"',
        '#include "command_processor.hpp"\n#include "'+(root/'retail_vertex_plan.hpp').as_posix()+'"')
    text=once(text,'static u32 s_display_list_depth = 0;',
        '#include "'+(root/'retail_raster.inc').as_posix()+'"\nstatic std::array<bool,256> machine_bp_valid{};\nstatic void machine_flush_batch();\nstatic void machine_invalidate_vertex_plans();\nstatic u32 s_display_list_depth = 0;')
    # A second entry shares the entire donor command loop and live GX state.
    # Recursive display lists use the same adapted complete-input entry.
    start=text.index('void process(const u8* data, u32 size, bool bigEndian) {')
    end=text.index('\n// Helper to extract bit fields',start)
    body=text[start:end].rstrip()
    if not body.endswith('}'):raise ValueError('Unknown command-loop boundary')
    native=body.replace('void process(const u8* data, u32 size, bool bigEndian)',
        'extern "C" u32 gcnllr_aurora_retail_process(const u8* data, u32 size)',1)
    native=once(native,'  u32 pos = 0;','  constexpr bool bigEndian=true;\n  u32 pos = 0;')
    native=once(native,'  while (pos < size) {',r'''  while (pos < size) {
    const u32 framed=gcnllr::retail_command_size(data+pos,size-pos,[](u8 fmt) {
      return g_gxState.lastVtxFmt==fmt?g_gxState.lastVtxSize:calculate_last_vtx_size(static_cast<GXVtxFmt>(fmt));
    });
    if(!framed)break;''')
    native=native[:-1]+'  return pos;\n}\n'
    # Hooks see the masked register value, including repeated PE notifications.
    old='    if (g_gxState.bpRegCache[regId] == value && regId != 0x52)\n      return;\n    g_gxState.bpRegCache[regId] = value;'
    new='''    const bool handled=gcnllr_aurora_retail_bp(regId,value&0xffffffu);
    // LOAD_TLUT1 executes a transfer even when its control word repeats.
    // LOAD_TLUT0 or the source RAM may have changed since the last transfer.
    if (machine_bp_valid[regId] && g_gxState.bpRegCache[regId] == value && regId != 0x52 && regId != 0x65 && !handled)
      return;
    machine_bp_valid[regId]=true;
    g_gxState.bpRegCache[regId] = value;
    if(handled)return;'''
    new=new.replace('    g_gxState.bpRegCache[regId] = value;', '''    if(!((regId>=0x8c && regId<=0x93) || (regId>=0xac && regId<=0xb3)))machine_flush_batch();
    g_gxState.bpRegCache[regId] = value;''')
    text=once(text,old,new)
    text=once(text,'static void handle_cp(u8 addr, u32 value, bool bigEndian) {',r'''static std::array<u32,256> machine_cp{};
static std::array<bool,256> machine_cp_valid{};
extern "C" void gcnllr_aurora_retail_reset(){machine_cp_valid.fill(false);machine_bp_valid.fill(false);machine_invalidate_vertex_plans();machine_viewport_valid=machine_projection_valid=false;machine_raster_dirty=true;}
static void handle_cp(u8 addr, u32 value, bool bigEndian) {
  // These fields have one writer in this selected pipeline. Matrix indices
  // can also be written by XF, so deliberately do not cache those here.
  if(addr==0x50 || addr==0x60 || (addr>=0x70 && addr<=0x97) || addr>=0xa0) {
    if(machine_cp_valid[addr] && machine_cp[addr]==value)return;
    machine_cp[addr]=value;machine_cp_valid[addr]=true;
  }
  machine_flush_batch();
  if(addr==0x50 || addr==0x60 || (addr>=0x70 && addr<=0x97))machine_invalidate_vertex_plans();''')
    text=once(text,'static void calculate_indexed_array_sizes(GXVtxFmt fmt,',
        'static void calculate_indexed_array_sizes_reference(GXVtxFmt fmt,')
    marker='static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange,'
    # One forward declaration and a later body; insert before the declaration.
    position=text.index(marker)
    text=text[:position]+'#include "'+(root/'retail_vertex_plan.inc').as_posix()+'"\n'+text[position:]
    text=once(text,'''      case TexBpRegMapping::Kind::Image2:
        // GXTexRegion regs
        break;''','''      case TexBpRegMapping::Kind::Image2:
        // Native resources have no hardware TMEM placement dependency.
        return;''')
    text=once(text,'  u32 totalVtxBytes = vtxCount * vtxSize;',
        '  machine_raster();machine_efb_state();\n  gcnllr_aurora_retail_draw();\n  u32 totalVtxBytes = vtxCount * vtxSize;')
    signature='static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, const u8* data, u32& pos, u32 size, bool bigEndian) {'
    text=once(text,signature,signature.replace('draw_prim(','draw_prim_reference('))
    marker='static void handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {'
    text=once(text,marker,'static void draw_prim(GXPrimitive,GXVtxFmt,u16,const u8*,u32&,u32,bool);\n'+marker)
    text=once(text,'static void machine_flush_batch();',
        '#include "'+(root/'retail_pe.inc').as_posix()+'"\nstatic void machine_flush_batch();')
    text=once(text,'std::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {',
        '#include "'+(root/'retail_batch.inc').as_posix()+'"\nstd::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {')
    text=once(text,'static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {',
        'static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {\n  machine_flush_batch();')
    text=once(text,'      ZoneScopedN("LOAD_INDX");','      ZoneScopedN("LOAD_INDX");\n      machine_flush_batch();')
    memory_boundary='  calculate_indexed_array_sizes(fmt, vtxCount, data + pos, bigEndian, arraySizes);'
    if text.count(memory_boundary)!=2:raise ValueError('Pinned native resource boundaries differ')
    text=text.replace(memory_boundary,r'''  calculate_indexed_array_sizes(fmt, vtxCount, data + pos, bigEndian, arraySizes);
  static std::array<uint64_t,GX_VA_MAX_ATTR> array_versions{};
  size_t storage=0;
  for(u32 a=0;a<GX_VA_MAX_ATTR;++a)if(arraySizes[a]) {
    auto& array=g_gxState.arrays[a];
    const auto version=gcnllr_aurora_retail_array(array.data,arraySizes[a]);
    if(version && version!=array_versions[a]){array.cachedRange={};g_gxState.stateDirty=true;array_versions[a]=version;}
    storage+=arraySizes[a]+256u;
  }
  const auto room=[&]{return gfx::staging_has_capacity(size_t(vtxCount)*vtxSize,
    size_t(vtxCount)*6u*sizeof(u16),MaxUniformSize*2u,0,storage);};
  if(!room()) {
    if(!gfx::segment_frame())throw std::runtime_error("Native retail staging segment failed");
    for(auto& array:g_gxState.arrays)array.cachedRange={};
    g_gxState.stateDirty=true;
    if(!room())throw std::runtime_error("Native retail draw exceeds an empty staging segment");
  }
  // Texture observation occurs at the actual resolver, after a live GPU-owned
  // view has been considered. It must not force a download before that lookup.
''')
    # Retail source performs the same staging check in its instanced extension,
    # whose local names differ. Our real FIFO scope never uses that host opcode.
    instanced=text.index('void handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian) {')
    tail=text[instanced:]
    tail=tail.replace('size_t(vtxCount)*vtxSize','size_t(vtxCount)*calculate_last_vtx_size(fmt)')
    text=text[:instanced]+tail
    first=text.index('  // Scissor registers (0x20, 0x21)')
    last=text.index('  // Line/point size (0x22)',first)
    text=text[:first]+'''  case 0x20:case 0x21:case 0x59:
    machine_raster_dirty=true;break;

'''+text[last:]
    text=once(text,'          f32 width = sx * 2.0f;',r'''          const float captured[]={sx,sy,sz,ox,oy,oz};
          std::memcpy(machine_viewport,captured,sizeof(captured));
          machine_viewport_valid=true;machine_raster_dirty=true;
          f32 width = sx * 2.0f;''')
    text=once(text,'''          g_gxState.stateDirty = true;
        }
        break;
      }
      case 0x3F:''','''          machine_projection=g_gxState.proj;machine_projection_valid=true;machine_raster_dirty=true;
          g_gxState.stateDirty = true;
        }
        break;
      }
      case 0x3F:''')
    # The pinned raw decoder still used the older raster-origin constant.
    text=text.replace('ox - 340.0f','ox - 342.0f').replace('oy - 340.0f','oy - 342.0f')
    # A failed nested resource must fail this provider, never silently omit it.
    text=once(text,'        Log.warn("Unable to resolve GX_CMD_CALL_DL address=0x{:08X} size={}", address, dlSize);\n        break;',
        '        throw std::runtime_error("Native retail display-list address did not resolve");')
    text=once(text,'        Log.warn("Skipping GX_CMD_CALL_DL recursion at depth {} (address=0x{:08X}, size={})", s_display_list_depth,\n                 address, dlSize);\n        break;',
        '        throw std::runtime_error("Native retail display-list recursion exceeded");')
    # Re-extract the now-adapted complete loop so recursive lists receive the
    # same failure/effect handling as the incremental top-level stream.
    start=text.index('void process(const u8* data, u32 size, bool bigEndian) {')
    adapted_end=text.index('\n// Helper to extract bit fields',start)
    adapted=text[start:adapted_end].rstrip()
    native=adapted.replace('void process(const u8* data, u32 size, bool bigEndian)',
        'extern "C" u32 gcnllr_aurora_retail_process(const u8* data, u32 size)',1)
    native=once(native,'  u32 pos = 0;','  constexpr bool bigEndian=true;\n  u32 pos = 0;')
    native=once(native,'  while (pos < size) {',r'''  while (pos < size) {
    const u32 framed=gcnllr::retail_command_size(data+pos,size-pos,[](u8 fmt) {
      return g_gxState.lastVtxFmt==fmt?g_gxState.lastVtxSize:calculate_last_vtx_size(static_cast<GXVtxFmt>(fmt));
    });
    if(!framed)break;''')
    native=native[:-1]+'  return pos;\n}\n'
    text=text[:adapted_end]+'\nstatic u32 calculate_last_vtx_size(GXVtxFmt);\n'+native+text[adapted_end:]
    # Native host SDK initialization uses this same command processor before a
    # machine binding exists; hooks are inert until its real FIFO takes over.
    prefix='''// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from pinned RecompCore338a3dbc Aurora gx/command_processor.cpp.
#include <stdexcept>
#include "'''+(root/'raster_viewport.hpp').as_posix()+'''"
#include "'''+(root/'retail_command.hpp').as_posix()+'''"
extern "C" bool gcnllr_aurora_retail_bp(uint32_t,uint32_t);
extern "C" void gcnllr_aurora_retail_draw();
extern "C" void gcnllr_aurora_retail_observe(const void*,uint32_t);
extern "C" uint64_t gcnllr_aurora_retail_array(const void*,uint32_t);
'''
    return prefix+text

def compose_texture(text):
    text=once(text,'      .msaaSamples = gfx::get_sample_count(),',
        '      .version = GXPipelineConfigVersion+1,\n      .msaaSamples = gfx::get_sample_count(),')
    for name,args in [('resolve_static_texture','const GXTexObj_& obj'),
                      ('resolve_static_palette_texture','const GXTexObj_& obj, const GXTlutObj_& tlut')]:
        marker='gfx::TextureHandle '+name+'('+args+') {'
        replacement=marker.replace('const GXTexObj_& obj','const GXTexObj_& input')
        if name=='resolve_static_texture':
            replacement+=r'''
  if(auto live=gcnllr_aurora_retail_owned(input.data,recomp::texture_byte_size(input),
      input.width(),input.height(),input.format(),input.mip_count()))return live;
'''
        replacement+=r'''
  gcnllr_aurora_retail_observe(input.data,recomp::texture_byte_size(input));
  GXTexObj_ owned=input;
  if(gcnllr_aurora_retail_texture(owned.data,recomp::texture_byte_size(owned),owned.width(),owned.height(),
      owned.format(),owned.mip_count(),&owned.texObjId,&owned.texDataVersion))owned.set_no_cache(false);
  const GXTexObj_& obj=owned;'''
        text=once(text,marker,replacement)
    return '''// SPDX-License-Identifier: GPL-3.0-or-later
// Pinned Aurora raw-renderer texture cache, bound to actual machine writers.
#include "recomp.hpp"
#include <memory>
namespace aurora::gfx {struct TextureRef;}
std::shared_ptr<aurora::gfx::TextureRef> gcnllr_aurora_retail_owned(const void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" void gcnllr_aurora_retail_observe(const void*,uint32_t);
extern "C" bool gcnllr_aurora_retail_texture(const void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t*,uint32_t*);
'''+text

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--root',type=Path,required=True);p.add_argument('--texture',action='store_true');a=p.parse_args()
    a.out.parent.mkdir(parents=True,exist_ok=True)
    original=a.source.read_text(encoding='utf-8')
    a.out.write_text(compose_texture(original) if a.texture else compose(original,a.root),encoding='utf-8')
