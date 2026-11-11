# SPDX-License-Identifier: MIT
"""Typed clear/copy/completion on the real GPU, coherent banks and zero FIFO.

This is a hidden, silent component check; no game or throughput acceptance.
Uses the existing GPU service and immutable-copy ABI, with an owner double
that retains the real bank/address identity and materializes exact generations.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct

from test_memory_gpu import Api, Config, BP, WRITE, EPOCH, RESOLVE, LOCATE, Range
from test_native_render_gpu import Vertex, Mesh, Draw, NativeApi


class Clear(c.Structure):
    _fields_=[(name,c.c_uint32) for name in
              ('x','y','width','height','red','green','blue','alpha','depth','flags')]


class DisplayCopy(c.Structure):
    _fields_=[(name,c.c_uint32) for name in ('destination','stride','x','y','width','height',
              'scale_y','invert_y_scale','gamma','flags')]+[('filter',c.c_uint32*7),('clear_value',Clear)]


class FrameStatus(c.Structure):
    _fields_=[(name,c.c_uint64) for name in ('clears','display_copies','completions','rejected')]+[('error',c.c_char*192)]


class FrameApi(c.Structure):
    _fields_=[('abi',c.c_uint32),('clear',c.CFUNCTYPE(c.c_int,c.POINTER(Clear))),
              ('copy',c.CFUNCTYPE(c.c_int,c.POINTER(DisplayCopy))),
              ('complete',c.CFUNCTYPE(c.c_int,c.c_uint32,c.c_uint32)),
              ('status',c.CFUNCTYPE(None,c.POINTER(FrameStatus)))]


class OwnedCopy(c.Structure):
    _fields_=[('token',c.c_uint64)]+[(name,c.c_uint32) for name in ('address','row_bytes','rows','stride','flags')]


class ExtendedApi(c.Structure):
    _fields_=Api._fields_+[
        ('present',c.c_void_p),('service',c.CFUNCTYPE(c.c_int)),('capabilities',c.c_uint32),
        ('materialize',c.CFUNCTYPE(c.c_int,c.c_uint64)),('release',c.CFUNCTYPE(None,c.c_uint64)),
        ('present_copy',c.CFUNCTYPE(c.c_int,c.c_uint64,c.c_uint32,c.c_uint32,c.c_uint32)),('present_spans',c.c_void_p)]


OWN=c.CFUNCTYPE(c.c_int,c.c_void_p,c.POINTER(OwnedCopy))
PENDING=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_uint32,c.c_uint32)
RETIRE=c.CFUNCTYPE(c.c_int,c.c_void_p,c.c_uint64)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library',type=Path,required=True)
    parser.add_argument('--work-root',type=Path,required=True)
    parser.add_argument('--eager',action='store_true',help='Match a consumer without optional copy-ownership callbacks')
    parser.add_argument('--efb-height',type=int,choices=(480,528),default=480)
    args=parser.parse_args()
    work=args.work_root.resolve();work.mkdir(parents=True,exist_ok=True);(work/'cache').mkdir(exist_ok=True)
    os.environ.update(SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0',SDL_AUDIODRIVER='dummy',GCN_AUDIO='0')
    directory=os.add_dll_directory(str(args.library.resolve().parent)) if os.name=='nt' else None
    dll=c.CDLL(str(args.library.resolve()))
    dll.gcnllr_native_gx_get_api.argtypes=[c.c_uint32];dll.gcnllr_native_gx_get_api.restype=c.POINTER(ExtendedApi)
    gx=dll.gcnllr_native_gx_get_api(12).contents
    dll.gc_wii_native_frame_get_api.argtypes=[c.c_uint32];dll.gc_wii_native_frame_get_api.restype=c.POINTER(FrameApi)
    assert not dll.gc_wii_native_frame_get_api(2),'Unknown native frame ABI accepted'
    frame=dll.gc_wii_native_frame_get_api(1).contents
    dll.gc_wii_native_render_get_api.argtypes=[c.c_uint32]
    dll.gc_wii_native_render_get_api.restype=c.POINTER(NativeApi)
    render=dll.gc_wii_native_render_get_api(1).contents
    mem1=(c.c_uint8*(24*1024*1024))();mem2=(c.c_uint8*(1024*1024))()
    banks=((0,mem1),(0x10000000,mem2));owned={};observed=set();events=[];checks=[]

    def mapping(address,size):
        physical=address&0x1fffffff
        if address not in (physical,physical+0x80000000,physical+0xc0000000):return None
        for base,bank in banks:
            offset=physical-base
            if 0<=offset<len(bank) and size<=len(bank)-offset:return base,bank,offset
        return None

    @RESOLVE
    def resolve(user,address,size,out):
        mapped=mapping(address,size)
        if not mapped:return 0
        base,bank,offset=mapped
        out[0]=Range(c.cast(c.addressof(bank)+offset,c.POINTER(c.c_uint8)),base+offset,len(bank)-offset);return 1

    @LOCATE
    def locate(user,data,size,address):
        for base,bank in banks:
            offset=data-c.addressof(bank)
            if 0<=offset<len(bank) and size<=len(bank)-offset:address[0]=base+offset;return 1
        return 0

    @WRITE
    def write(user,address,source,size,flags,width,height,stride):
        mapped=mapping(address,size)
        if not mapped:return 0
        _,bank,offset=mapped;c.memmove(c.addressof(bank)+offset,source,size)
        events.append(['write',address,size,flags]);return 1

    @OWN
    def own(user,value):
        item=OwnedCopy.from_buffer_copy(value.contents)
        if not mapping(item.address,(item.rows-1)*item.stride+item.row_bytes):return 0
        owned[item.token]=item;events.append(['own',item.token,item.address]);return 1

    def overlaps(item,address,size):
        address&=0x1fffffff
        return address<item.address+(item.rows-1)*item.stride+item.row_bytes and item.address<address+size

    @PENDING
    def pending(user,address,size):return int(any(t not in observed and overlaps(v,address,size) for t,v in owned.items()))

    @PENDING
    def prepare(user,address,size):
        for token,value in list(owned.items()):
            if token not in observed and overlaps(value,address,size):
                if not gx.materialize(token):return 0
                observed.add(token)
        return 1

    @RETIRE
    def retire(user,token):
        if token not in observed:
            if not gx.materialize(token):return 0
            observed.add(token)
        return 1

    @BP
    def bp(user,reg,value):events.append(['pe',reg,value])

    config=Config(12,b'Native frame component check',str(work).encode(),str(work/'cache').encode(),1,None,0,None,
        bp,write,EPOCH(),EPOCH(),c.cast(own,c.c_void_p),c.cast(pending,c.c_void_p),c.cast(prepare,c.c_void_p),
        c.cast(retire,c.c_void_p),resolve,locate)
    if args.eager:config.own=config.pending=config.prepare=config.retire=None
    assert gx.start(c.byref(config)),'GPU startup failed'
    def status():
        value=FrameStatus();frame.status(c.byref(value));return value
    def clear(x,y,w,h,rgb=(0,0,0),alpha=255,depth=0xffffff,flags=7):
        value=Clear(x,y,w,h,*rgb,alpha,depth,flags)
        assert frame.clear(c.byref(value)),status().error.decode()
    def pixel(x,y,depth=0):
        value=c.c_uint32();assert gx.peek(depth,x,y,c.byref(value));return value.value
    def expect_color(name,x,y,color,alpha=None):
        value=pixel(x,y);actual=((value>>16)&255,(value>>8)&255,value&255)
        assert actual==color,(name,actual,color)
        if alpha is not None:assert value>>24==alpha,(name,'alpha',value>>24,alpha)
        checks.append(name)
    def copy(destination,y,height=1,taps=(0,0,21,22,21,0,0),flags=3,scale=256):
        value=DisplayCopy();value.destination=destination;value.stride=32;value.width=16
        value.y=y;value.height=height;value.scale_y=scale;value.flags=flags;value.filter[:]=taps
        value.clear_value=Clear(0,0,0,0,0,0,0,255,0xffffff,7)
        assert frame.copy(c.byref(value)),status().error.decode()
        return max(owned) if owned else None
    mesh_handle=0
    try:
        vertices=(Vertex*4)(*[Vertex((c.c_float*3)(x,y,.5),(c.c_float*4)(1,0,0,1),(c.c_float*2)(0,0))
            for x,y in ((-1,-1),(1,-1),(1,1),(-1,1))])
        indices=(c.c_uint16*6)(0,1,2,0,2,3)
        mesh_handle=render.create(c.byref(Mesh(vertices,indices,4,6)));assert mesh_handle
        job=Draw();job.mesh=mesh_handle;job.model_view[:]=(1,0,0,0,0,1,0,0,0,0,1,0)
        job.projection[:]=(1,0,0,0,0,1,0,0,0,0,-1,0,0,0,0,1)
        job.width=job.clip_width=640;job.height=job.clip_height=480;job.far_depth=1
        assert render.draw(c.byref(job)),'Pre-clear native geometry submission failed'
        clear(0,0,640,480,(0,0,255),85,0xabcdef)
        clear(2,3,4,5,(255,0,0),255,0,1)
        # Ordinary EFB peek follows the legacy RGB8 pixel format and reports
        # opaque alpha. Check independent color/depth masks through that API.
        expect_color('partial color clear',3,4,(255,0,0),255)
        assert abs(pixel(3,4,1)-0xabcdef)<=1
        clear(2,3,4,5,(0,0,0),170,0x123456,2|4)
        expect_color('alpha/depth clear preserves color',3,4,(255,0,0),255)
        assert abs(pixel(3,4,1)-0x123456)<=1
        expect_color('partial clear preserves outside pixels',10,4,(0,0,255),255)

        colors=((0,0,255),(255,255,255),(0,255,0),(255,0,0))
        for x in range(16):clear(x,10,1,2,colors[x%4])
        c.memset(c.addressof(mem1)+0x10000,0xa5,64)
        token=copy(0x90010000,10,2,flags=3|4)
        if token:assert owned[token].address==0x10010000,'MEM2 cached alias lost physical bank identity'
        assert frame.complete(1|4,0xbeef),status().error.decode()
        assert events[-2:]==[['pe',0x48,0xbeef],['pe',0x45,2]],events
        assert any(e[0]==('write' if args.eager else 'own') for e in events[:-2]),'Completion overtook copy publication'
        assert prepare(None,0x10010000,64)
        actual=bytes(mem2[0x10000:0x10008]);expected=bytes((41,212,235,114,144,81,82,109))
        assert actual==expected,('hardware quarter-weight chroma',actual.hex(),expected.hex())
        assert bytes(mem2[0x10020:0x10028])==expected
        assert bytes(mem1[0x10000:0x10040])==bytes([0xa5])*64,'MEM2 copy overwrote MEM1'
        checks.append('actual YUY2 bytes match quarter-weight chroma')
        expect_color('copy snapshots before its clear',0,10,(0,0,0))
        expect_color('copy clear retains outside pixels',20,10,(0,0,255),255)
        if token:assert gx.present_copy(token,16,2,32)==1,'Resident generation scanout failed'
        else:
            present=c.CFUNCTYPE(c.c_int,c.POINTER(c.c_uint8),c.c_uint32,c.c_uint32,c.c_uint32)(gx.present)
            assert present(c.cast(c.addressof(mem2)+0x10000,c.POINTER(c.c_uint8)),16,2,32),'Materialized generation scanout failed'
        value=pixel(0,0,2);scanout=((value>>16)&255,(value>>8)&255,value&255)
        y,u,_,v=expected[:4];base=298*(y-16)
        decoded=tuple(max(0,min(255,n>>8)) for n in
            (base+409*(v-128)+128,base-100*(u-128)-208*(v-128)+128,base+516*(u-128)+128))
        assert all(abs(a-b)<=1 for a,b in zip(scanout,decoded)),('generation scanout',scanout,decoded)
        checks.append('materialized scanout matches copied bytes' if args.eager else
                      'immutable generation survives clear and presents without RAM recopy')

        clear(0,99,16,3,(0,0,0));clear(0,100,16,1,(255,255,255))
        filtered=copy(0x90010200,100,taps=(8,8,10,12,10,8,8),flags=0)
        clamped=copy(0x90010400,100,taps=(8,8,10,12,10,8,8),flags=3)
        zero=copy(0x90010600,100,taps=(0,0,0,0,0,0,0))
        assert frame.complete(0,0),status().error.decode()
        for t in (filtered,clamped,zero):
            if t:assert retire(None,t)
        assert bytes(mem2[0x10200:0x10204])==bytes((125,128,125,128))
        assert bytes(mem2[0x10400:0x10404])==bytes((235,128,235,128))
        assert bytes(mem2[0x10600:0x10604])==bytes((16,128,16,128))
        checks.append('seven filter taps, external row halo, clamp and zero filter preserved')
        if args.efb_height==528:
            clear(0,480,16,48,(0,255,0))
            expect_color('physical EFB row 511 survives VI height 480',0,511,(0,255,0))
            expect_color('physical EFB last row is addressable',0,527,(0,255,0))
            extra=copy(0x90010800,510,4)
            assert frame.complete(0,0),status().error.decode()
            if extra:assert retire(None,extra)
            assert bytes(mem2[0x10800:0x10804])==bytes((144,54,144,34))
            assert bytes(mem2[0x10860:0x10864])==bytes((144,54,144,34))
            checks.append('display copy reads physical rows beyond VI height')
        bad=DisplayCopy(0x900ffff0,32,0,0,16,2,256,0,0,3)
        assert not frame.copy(c.byref(bad)),'Unaligned/out-of-bank destination accepted'
        bad.destination=0x900ffff0+16
        assert not frame.copy(c.byref(bad)),'Copy crossed a bank boundary'
        assert not frame.complete(2|4,0),'Conflicting token notifications accepted'
        clear_bad=Clear(0,0,641,1,0,0,0,255,0xffffff,7)
        assert not frame.clear(c.byref(clear_bad)),'Out-of-EFB clear accepted'
        gx_status=c.create_string_buffer(2048);c.CFUNCTYPE(None,c.c_void_p)(gx.status)(gx_status)
        fifo=struct.unpack_from('Q',gx_status.raw)[0];assert fifo==0,'Native frame emitted commands'
        fs=status();assert fs.display_copies==4+(args.efb_height==528) and fs.rejected==4, (fs.display_copies,fs.rejected,fs.error)
        result={'component_only':True,'game_qualified':False,'performance_qualified':False,'native_frame_abi':1,
            'library_sha256':hashlib.sha256(args.library.read_bytes()).hexdigest(),'fifo_bytes':fifo,'eager':args.eager,
            'efb_height':args.efb_height,
            'clears':fs.clears,'display_copies':fs.display_copies,'completions':fs.completions,
            'rejected':fs.rejected,'first_yuy2_bytes':actual.hex(),'checks':checks,'events':events}
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
    finally:
        if mesh_handle:render.destroy(mesh_handle)
        for token in owned:gx.release(token)
        gx.stop()
        if directory:directory.close()


if __name__=='__main__':main()
