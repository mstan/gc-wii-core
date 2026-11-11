# SPDX-License-Identifier: MIT
"""Native persistent meshes/materials on the real GPU, with zero FIFO bytes.

This is a rendering-component check, not title or performance qualification.
Reuse the existing hidden, silent GPU service and its actual EFB readback.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import re
import struct

from test_memory_gpu import Api, Config, BP, WRITE, EPOCH, RESOLVE, LOCATE


class Vertex(c.Structure):
    _fields_=[('position',c.c_float*3),('color',c.c_float*4),('uv',c.c_float*2)]

class Mesh(c.Structure):
    _fields_=[('vertices',c.POINTER(Vertex)),('indices',c.POINTER(c.c_uint16)),
              ('vertex_count',c.c_uint32),('index_count',c.c_uint32)]

class Texture(c.Structure):
    _fields_=[(name,c.c_uint32) for name in ('address','size','format','width','height',
        'palette_address','palette_entries','palette_format','wrap_s','wrap_t','linear')]

class Material(c.Structure):
    _fields_=[('flags',c.c_uint32),('texture',Texture)]

class Draw(c.Structure):
    _fields_=[('mesh',c.c_uint64),('material',Material),('model_view',c.c_float*12),
        ('projection',c.c_float*16),*((name,c.c_float) for name in
        ('x','y','width','height','near_depth','far_depth')),
        *((name,c.c_int32) for name in ('clip_x','clip_y','clip_width','clip_height'))]

class Status(c.Structure):
    _fields_=[(name,c.c_uint64) for name in ('mesh_uploads','draws','rejected',
                                           'live_meshes','uploaded_vertex_bytes')]

class NativeApi(c.Structure):
    _fields_=[('abi',c.c_uint32),('create',c.CFUNCTYPE(c.c_uint64,c.POINTER(Mesh))),
        ('destroy',c.CFUNCTYPE(c.c_int,c.c_uint64)),('draw',c.CFUNCTYPE(c.c_int,c.POINTER(Draw))),
        ('status',c.CFUNCTYPE(None,c.POINTER(Status)))]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library',type=Path,required=True)
    parser.add_argument('--work-root',type=Path,required=True)
    args=parser.parse_args()
    work=args.work_root.resolve();work.mkdir(parents=True,exist_ok=True)
    (work/'cache').mkdir(exist_ok=True)
    os.environ.update(SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0',SDL_AUDIODRIVER='dummy',GCN_AUDIO='0')
    header=(Path(__file__).resolve().parents[2]/'include/gcnllr/native_gx.h').read_text(encoding='utf-8')
    abi=int(re.search(r'^#define GCNLLR_NATIVE_GX_ABI (\d+)u',header,re.M)[1])
    directory=os.add_dll_directory(str(args.library.resolve().parent)) if os.name=='nt' else None
    dll=c.CDLL(str(args.library.resolve()))
    dll.gcnllr_native_gx_get_api.argtypes=[c.c_uint32]
    dll.gcnllr_native_gx_get_api.restype=c.POINTER(Api)
    gx=dll.gcnllr_native_gx_get_api(abi).contents
    dll.gc_wii_native_render_get_api.argtypes=[c.c_uint32]
    dll.gc_wii_native_render_get_api.restype=c.POINTER(NativeApi)
    assert not dll.gc_wii_native_render_get_api(2), 'Unknown API accepted'
    native=dll.gc_wii_native_render_get_api(1).contents
    ram=(c.c_uint8*(24*1024*1024))()
    write=WRITE(lambda *args:0)
    bp=BP(lambda *args:None)
    config=Config(abi,b'Native renderer component check',str(work).encode(),str(work/'cache').encode(),
        1,c.addressof(ram),len(ram),None,bp,write,EPOCH(),EPOCH(),None,None,None,None,RESOLVE(),LOCATE())
    assert gx.start(c.byref(config)), 'GPU startup failed'
    observations=[]
    try:
        vertices=(Vertex*4)(*[Vertex((c.c_float*3)(x,y,.5),
            (c.c_float*4)(1,0,0,1),(c.c_float*2)(u,v))
            for x,y,u,v in ((-1,-1,0,1),(1,-1,1,1),(1,1,1,0),(-1,1,0,0))])
        indices=(c.c_uint16*6)(0,1,2,0,2,3)
        mesh=Mesh(vertices,indices,4,6)
        handle=native.create(c.byref(mesh));assert handle, 'Native mesh upload failed'
        # Conversion takes ownership: later mutation of the input must not
        # mutate a retained native mesh or force another guest geometry decode.
        vertices[0].color[0]=0
        job=Draw();job.mesh=handle
        job.model_view[:]=(1,0,0,0,0,1,0,0,0,0,1,0)
        job.projection[:]=(1,0,0,0,0,1,0,0,0,0,-1,0,0,0,0,1)
        job.width=640;job.height=480;job.far_depth=1
        job.clip_width=640;job.clip_height=480

        def pixel(x,y):
            value=c.c_uint32()
            assert gx.peek(0,x,y,c.byref(value)), 'Native image readback failed'
            return ((value.value>>16)&255,(value.value>>8)&255,value.value&255)

        def expect(name,x,y,rgb):
            actual=pixel(x,y)
            assert all(abs(a-b)<=2 for a,b in zip(actual,rgb)), (name,actual,rgb)
            observations.append({'check':name,'pixel':[x,y],'actual':actual,'expected':rgb})

        assert native.draw(c.byref(job)), 'Native color draw failed'
        expect('owned red mesh',320,240,(255,0,0))
        expect('copied vertex color',64,400,(255,0,0))
        # The same converted geometry is reused with a different material.
        c.memset(c.addressof(ram)+0x100,255,32)  # one I8 tile
        job.material.texture=Texture(0x100,32,1,8,4,0,0,0,0,0,0)
        assert native.draw(c.byref(job)), 'Native textured draw failed'
        expect('texture multiplied by vertex color',320,240,(255,0,0))
        c.memset(c.addressof(ram)+0x100,128,32)
        job.clip_x=320;job.clip_width=320
        assert native.draw(c.byref(job)), 'Native changed-texture/scissor draw failed'
        expect('scissor retains prior color',160,240,(255,0,0))
        expect('CPU texture rewrite is observed',480,240,(128,0,0))
        c.memset(c.addressof(ram)+0x100,0,32)
        job.clip_x=0;job.clip_width=640;job.model_view[3]=-1
        assert native.draw(c.byref(job)), 'Native transformed draw failed'
        expect('model-view translation moves geometry',160,240,(0,0,0))
        expect('translation preserves uncovered pixels',480,240,(128,0,0))
        bad=Draw.from_buffer_copy(job);bad.material.flags=0x80000000
        assert not native.draw(c.byref(bad)), 'Unknown material silently accepted'
        bad=Draw.from_buffer_copy(job);bad.material.texture.format=99
        assert not native.draw(c.byref(bad)), 'Unknown texture format silently accepted'
        bad=Draw.from_buffer_copy(job);bad.material.texture.size=1
        assert not native.draw(c.byref(bad)), 'Truncated tiled texture silently accepted'
        status=Status();native.status(c.byref(status))
        assert (status.mesh_uploads,status.draws,status.rejected,status.live_meshes)==(1,4,3,1), vars(status)
        gx_status=c.create_string_buffer(2048)
        c.CFUNCTYPE(None,c.c_void_p)(gx.status)(gx_status)
        fifo_bytes=struct.unpack_from('Q',gx_status.raw)[0]
        assert fifo_bytes==0, 'Native renderer encoded or replayed FIFO bytes'
        assert native.destroy(handle), 'Mesh release failed'
        assert not native.draw(c.byref(job)), 'Destroyed mesh silently reused'
        from PIL import Image
        image=Image.new('RGB',(160,120))
        image.putdata([pixel(x*4+2,y*4+2) for y in range(120) for x in range(160)])
        image.save(work/'native-render.png')
        result={'schema':1,'component_only':True,'title_qualified':False,'performance_qualified':False,
            'library':str(args.library.resolve()),'sha256':hashlib.sha256(args.library.read_bytes()).hexdigest(),
            'fifo_bytes':fifo_bytes,'mesh_uploads':status.mesh_uploads,'native_draws':status.draws,
            'checks':observations,'image':'native-render.png'}
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(result))
    finally:
        gx.stop()
        if directory:directory.close()


if __name__=='__main__':main()
