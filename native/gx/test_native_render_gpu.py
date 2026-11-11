# SPDX-License-Identifier: MIT
"""Native persistent meshes/materials on the real GPU, with zero FIFO bytes.

This is a rendering-component check, not title or performance qualification.
Reuse the existing hidden, silent GPU service and its actual EFB readback.
The optional mixed-raster case adds original reference commands after the
zero-FIFO native checks; it never routes native materials through them.
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

class GuiMaterial(c.Structure):
    _fields_=[('low',c.c_int32*4),('high',c.c_int32*4)]

class GuiDraw(c.Structure):
    _fields_=[('draw',Draw),('color',GuiMaterial)]

class GuiApi(c.Structure):
    _fields_=[('abi',c.c_uint32),('draw',c.CFUNCTYPE(c.c_int,c.POINTER(GuiDraw)))]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library',type=Path,required=True)
    parser.add_argument('--work-root',type=Path,required=True)
    parser.add_argument('--gui',action='store_true',help='Also check the native GUI color-range material')
    parser.add_argument('--raster-interop',action='store_true',help='Check native to original draws with different raster state')
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
        job.model_view[3]=0;job.model_view[11]=.25
        job.material.flags=2|4 # ordinary LEQUAL with a depth write
        c.memset(c.addressof(ram)+0x100,128,32)
        assert native.draw(c.byref(job)), 'Native depth-writing draw failed'
        expect('nearer depth writes color',320,240,(128,0,0))
        job.model_view[11]=-.25
        c.memset(c.addressof(ram)+0x100,0,32)
        assert native.draw(c.byref(job)), 'Native occluded draw failed'
        expect('ordinary depth rejects farther geometry',320,240,(128,0,0))
        job.material.flags|=8 # explicit ALWAYS for menu overlays
        assert native.draw(c.byref(job)), 'Native ALWAYS depth draw failed'
        expect('ALWAYS depth accepts farther overlay',320,240,(0,0,0))
        bad=Draw.from_buffer_copy(job);bad.material.flags=0x80000000
        assert not native.draw(c.byref(bad)), 'Unknown material silently accepted'
        bad=Draw.from_buffer_copy(job);bad.material.texture.format=99
        assert not native.draw(c.byref(bad)), 'Unknown texture format silently accepted'
        bad=Draw.from_buffer_copy(job);bad.material.texture.size=1
        assert not native.draw(c.byref(bad)), 'Truncated tiled texture silently accepted'
        status=Status();native.status(c.byref(status))
        assert (status.mesh_uploads,status.draws,status.rejected,status.live_meshes)==(1,7,3,1), vars(status)
        if args.gui:
            dll.gc_wii_native_gui_get_api.argtypes=[c.c_uint32]
            dll.gc_wii_native_gui_get_api.restype=c.POINTER(GuiApi)
            assert not dll.gc_wii_native_gui_get_api(2)
            gui=dll.gc_wii_native_gui_get_api(1).contents
            for vertex in vertices:vertex.color[:]=(1,1,1,1)
            gui_handle=native.create(c.byref(mesh));assert gui_handle
            colored=GuiDraw();colored.draw=Draw.from_buffer_copy(job)
            colored.draw.mesh=gui_handle;colored.draw.material.flags=0;colored.draw.model_view[11]=0
            cases=[('GUI constant blue range',128,(52,190,237,0),(52,190,237,255),(52,190,237)),
                   ('GUI low endpoint',0,(138,148,153,0),(255,255,255,255),(138,148,153)),
                   ('GUI interpolated range',128,(138,148,153,0),(255,255,255,255),(197,202,204)),
                   ('GUI reversed range',255,(255,255,255,0),(50,50,50,255),(50,50,50))]
            for name,value,low,high,rgb in cases:
                c.memset(c.addressof(ram)+0x100,value,32)
                colored.color=GuiMaterial((c.c_int32*4)(*low),(c.c_int32*4)(*high))
                assert gui.draw(c.byref(colored)),name
                expect(name,320,240,rgb)
            colored.draw.material.texture=Texture()
            assert gui.draw(c.byref(colored))
            expect('GUI flat high endpoint',320,240,(50,50,50))
            colored.draw.material.texture=Texture(0x100,32,1,8,4,0,0,0,0,0,0)
            colored.draw.material.flags=1
            colored.color=GuiMaterial((c.c_int32*4)(0,255,0,0),(c.c_int32*4)(0,255,0,0))
            assert gui.draw(c.byref(colored))
            expect('GUI zero alpha range preserves background',320,240,(50,50,50))
            c.memset(c.addressof(ram)+0x100,128,32);colored.color.high[3]=255
            assert gui.draw(c.byref(colored))
            expect('GUI interpolated alpha blends range color',320,240,(25,153,25))
            colored.color.high[0]=256
            assert not gui.draw(c.byref(colored)), 'Unqualified color endpoint accepted'
            assert native.destroy(gui_handle)
            native.status(c.byref(status))
        gx_status=c.create_string_buffer(2048)
        c.CFUNCTYPE(None,c.c_void_p)(gx.status)(gx_status)
        fifo_bytes=struct.unpack_from('Q',gx_status.raw)[0]
        assert fifo_bytes==0, 'Native renderer encoded or replayed FIFO bytes'
        reference_fifo_bytes=0
        if args.raster_interop:
            # The original renderer caches a 640x480 raster. A native widget
            # uses a smaller viewport/scissor, then the original caller draws
            # again without rewriting its unchanged XF/BP raster inputs.
            def bp_regs(values):return b''.join(b'\x61'+struct.pack('>I',reg<<24|value) for reg,value in values)
            def cp(reg,value):return b'\x08'+bytes([reg])+struct.pack('>I',value)
            def floats(values):return struct.pack('>'+len(values)*'f',*values)
            def xf(reg,values):return b'\x10'+struct.pack('>I',((len(values)//4-1)<<16)|reg)+values
            def send(raw):
                raw+=bytes((-len(raw))%32)
                data=(c.c_uint8*len(raw)).from_buffer_copy(raw)
                assert gx.fifo(data,len(raw)) and gx.drain(), 'Original raster reference failed'
            def quad(color):return b'\x80\0\4'+b''.join(floats([x,y,.5])+bytes(color)
                for x,y in ((-1,-1),(1,-1),(1,1),(-1,1)))
            setup=cp(0x50,(1<<9)|(1<<13))+cp(0x60,0)+cp(0x70,1|(4<<1)|(5<<14))
            setup+=xf(0,floats([1,0,0,0,0,1,0,0,0,0,1,0]))
            setup+=xf(0x101a,floats([320,-240,16777215,662,582,16777215]))
            setup+=xf(0x1020,floats([1,0,1,0,-1,0])+struct.pack('>I',1))
            setup+=xf(0x1009,struct.pack('>I',1))+xf(0x100e,struct.pack('>I',1))+xf(0x1010,struct.pack('>I',1))
            setup+=xf(0x103f,struct.pack('>I',0))+bp_regs([(0,0x10),(0x20,(342<<12)|342),
                (0x21,(981<<12)|821),(0x59,171|(171<<10)),(0x28,0),(0x40,0),(0x41,0x18),(0x43,0),
                (0xf3,0x3f0000),(0xf6,4),(0xf7,14),
                (0xc0,10|(15<<4)|(15<<8)|(15<<12)|(1<<19)),
                (0xc1,(5<<4)|(7<<7)|(7<<10)|(7<<13)|(1<<19))])
            send(setup+quad((0,255,0,255)))
            expect('original full raster before native widget',320,400,(0,255,0))
            widget=Draw.from_buffer_copy(job);widget.material=Material();widget.model_view[11]=0
            widget.height=240;widget.clip_width=320;widget.clip_height=240
            assert native.draw(c.byref(widget))
            send(quad((0,0,255,255)))
            expect('original viewport restored after NativeRender',320,400,(0,0,255))
            expect('original scissor restored after NativeRender',480,100,(0,0,255))
            if args.gui:
                remapped=GuiDraw(widget,GuiMaterial((c.c_int32*4)(0,0,0,0),(c.c_int32*4)(255,255,255,255)))
                assert gui.draw(c.byref(remapped))
                send(quad((0,255,0,255)))
                expect('original viewport restored after GUI',320,400,(0,255,0))
                expect('original scissor restored after GUI',480,100,(0,255,0))
            c.CFUNCTYPE(None,c.c_void_p)(gx.status)(gx_status)
            reference_fifo_bytes=struct.unpack_from('Q',gx_status.raw)[0]
            assert reference_fifo_bytes>0
            native.status(c.byref(status))
        assert native.destroy(handle), 'Mesh release failed'
        assert not native.draw(c.byref(job)), 'Destroyed mesh silently reused'
        from PIL import Image
        image=Image.new('RGB',(160,120))
        image.putdata([pixel(x*4+2,y*4+2) for y in range(120) for x in range(160)])
        image.save(work/'native-render.png')
        result={'schema':1,'component_only':True,'title_qualified':False,'performance_qualified':False,
            'library':str(args.library.resolve()),'sha256':hashlib.sha256(args.library.read_bytes()).hexdigest(),
            'fifo_bytes':fifo_bytes,'mesh_uploads':status.mesh_uploads,'native_draws':status.draws,
            'original_reference_fifo_bytes':reference_fifo_bytes,
            'checks':observations,'image':'native-render.png'}
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(result))
    finally:
        gx.stop()
        if directory:directory.close()


if __name__=='__main__':main()
