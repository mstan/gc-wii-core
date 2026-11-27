# SPDX-License-Identifier: MIT
"""Native/retail draw interleaving restores each pipeline's binding layouts."""
import argparse
import ctypes as c
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import time
from test_native_render_gpu import Vertex, Mesh, Texture, Draw, NativeApi
from test_memory_gpu import Api, Config, BP, WRITE, EPOCH, RESOLVE, LOCATE


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library',type=Path,required=True)
    p.add_argument('--work-root',type=Path,required=True)
    args=p.parse_args();work=args.work_root.resolve();work.mkdir(parents=True,exist_ok=True);(work/'cache').mkdir(exist_ok=True)
    os.environ.update(SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0',SDL_AUDIODRIVER='dummy',GCN_AUDIO='0')
    directory=os.add_dll_directory(str(args.library.resolve().parent)) if os.name=='nt' else None
    header=(Path(__file__).resolve().parents[2]/'include/gcnllr/native_gx.h').read_text(encoding='utf-8')
    abi=int(re.search(r'^#define GCNLLR_NATIVE_GX_ABI (\d+)u',header,re.M)[1])
    dll=c.CDLL(str(args.library.resolve()))
    dll.gcnllr_native_gx_get_api.argtypes=[c.c_uint32];dll.gcnllr_native_gx_get_api.restype=c.POINTER(Api)
    dll.gc_wii_native_render_get_api.argtypes=[c.c_uint32];dll.gc_wii_native_render_get_api.restype=c.POINTER(NativeApi)
    gx=dll.gcnllr_native_gx_get_api(abi).contents;native=dll.gc_wii_native_render_get_api(1).contents
    ram=(c.c_uint8*(24*1024*1024))();c.memset(c.addressof(ram)+0x100,255,32)
    bp_callback=BP(lambda *args:None);write=WRITE(lambda *args:0)
    config=Config(abi,b'Native retail draw binding check',str(work).encode(),str(work/'cache').encode(),1,c.addressof(ram),len(ram),None,bp_callback,write,EPOCH(),EPOCH(),None,None,None,None,RESOLVE(),LOCATE())
    def cp(reg,value):return b'\x08'+bytes([reg])+struct.pack('>I',value)
    def bp(values):return b''.join(b'\x61'+struct.pack('>I',reg<<24|value) for reg,value in values)
    def xf(reg,values,words=False):return b'\x10'+struct.pack('>I',(len(values)-1)<<16|reg)+struct.pack('>'+len(values)*('I' if words else 'f'),*values)
    def send(raw):
        raw+=bytes((-len(raw))%32);data=(c.c_uint8*len(raw)).from_buffer_copy(raw)
        assert gx.fifo(data,len(raw)) and gx.drain(),'Retail submission failed'
    observations=[]
    def pixel():
        value=c.c_uint32();assert gx.peek(0,320,240,c.byref(value)),'Mixed draw readback failed'
        return ((value.value>>16)&255,(value.value>>8)&255,value.value&255)
    def check(name,rgb):
        actual=pixel()
        assert all(abs(a-b)<=2 for a,b in zip(actual,rgb)),(name,actual,rgb)
        observations.append({'check':name,'actual':actual,'expected':rgb})
    assert gx.start(c.byref(config)),'GPU startup failed'
    handles=[]
    try:
        vertices=(Vertex*4)(*[Vertex((c.c_float*3)(x,y,.5),(c.c_float*4)(1,0,0,1),(c.c_float*2)(u,v)) for x,y,u,v in ((-1,-1,0,1),(1,-1,1,1),(1,1,1,0),(-1,1,0,0))])
        indices=(c.c_uint16*6)(0,1,2,0,2,3);mesh=Mesh(vertices,indices,4,6);handle=native.create(c.byref(mesh));assert handle;handles.append(handle)
        job=Draw();job.mesh=handle;job.model_view[:]=(1,0,0,0,0,1,0,0,0,0,1,0);job.projection[:]=(1,0,0,0,0,1,0,0,0,0,-1,0,0,0,0,1)
        job.width=job.clip_width=640;job.height=job.clip_height=480;job.far_depth=1
        job.material.texture=Texture(0x100,32,1,8,4,0,0,0,0,0,0)
        setup=cp(0x50,(1<<9)|(1<<13))+cp(0x60,0)+cp(0x70,1|(4<<1)|(5<<14))+cp(0x80,0)+cp(0x90,0)
        setup+=xf(0,(1,0,0,0,0,1,0,0,0,0,1,0))+xf(0x400,(1,0,0,0,1,0,0,0,1))
        setup+=xf(0x1018,(0,),True)+xf(0x101a,(320,-240,16777215,662,582,16777215))
        setup+=b'\x10'+struct.pack('>I',6<<16|0x1020)+struct.pack('>6fI',1,0,1,0,-1,0,1)
        setup+=xf(0x103f,(0,),True)+xf(0x1009,(1,),True)+xf(0x100e,(1,1),True)
        setup+=bp([(0,0x10),(0x20,(342<<12)|342),(0x21,(981<<12)|821),(0x59,171|(171<<10)),(0x28,0),(0x40,0),(0x41,0x18),(0x43,0),(0xf3,0x3f0000),(0xf6,4),(0xf7,14),
            (0xc0,15|(15<<4)|(15<<8)|(10<<12)|(1<<19)),(0xc1,(7<<4)|(7<<7)|(7<<10)|(5<<13)|(1<<19))])
        quad=b'\x80\0\4'+b''.join(struct.pack('>3f',x,y,.5)+bytes([0,255,0,255]) for x,y in ((-1,-1),(1,-1),(1,1),(-1,1)))
        send(setup)
        # Warm the asynchronous retail pipeline before the ordering check.
        # A skipped cold pipeline is a different condition from stale bindings.
        for _ in range(100):
            send(quad)
            if pixel()==(0,255,0):break
            time.sleep(.05)
        else:raise AssertionError(('Retail warmup did not render',pixel()))
        # No peek or completed-frame barrier between these submissions: both
        # pipeline families must correctly bind their own resources in one pass.
        assert native.draw(c.byref(job)),'First native draw failed'
        send(quad);check('native textured then retail untextured',(0,255,0))
        assert native.draw(c.byref(job)),'Second native draw failed'
        check('retail untextured then native textured',(255,0,0))
        send(quad);check('native return to retail untextured',(0,255,0))
        result={'result':'passed','component_only':True,'title_qualified':False,'performance_qualified':False,
            'library_sha256':hashlib.sha256(args.library.read_bytes()).hexdigest(),'checks':observations}
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
    finally:
        for handle in handles:native.destroy(handle)
        gx.stop()
        if directory:directory.close()


if __name__=='__main__':main()
