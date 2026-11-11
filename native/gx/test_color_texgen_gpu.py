# SPDX-License-Identifier: MIT
"""Color-source texgens read computed XF colors, including without CLR inputs.

ROM-free GPU regression for the selected Aurora renderer. The hidden test has
no audio output and reads actual EFB pixels through the existing service.
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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--work-root', type=Path, required=True)
    args = p.parse_args()
    work = args.work_root.resolve(); work.mkdir(parents=True, exist_ok=True)
    (work/'cache').mkdir(exist_ok=True)
    os.environ.update(SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_AUDIODRIVER='dummy', GCN_AUDIO='0')
    header = (Path(__file__).resolve().parents[2]/'include/gcnllr/native_gx.h').read_text()
    abi = int(re.search(r'^#define GCNLLR_NATIVE_GX_ABI (\d+)u', header, re.M)[1])
    directory = os.add_dll_directory(str(args.library.resolve().parent)) if os.name == 'nt' else None
    dll = c.CDLL(str(args.library.resolve()))
    dll.gcnllr_native_gx_get_api.argtypes = [c.c_uint32]
    dll.gcnllr_native_gx_get_api.restype = c.POINTER(Api)
    api = dll.gcnllr_native_gx_get_api(abi).contents
    ram = (c.c_uint8*(24*1024*1024))()
    # One I8 tile: left half black, right half white. A computed red channel
    # selecting S=1 must sample white; an absent/raw blue CLR0 selects black.
    c.memmove(c.addressof(ram)+0x100, bytes([0]*4+[255]*4)*4, 32)
    bp_callback = BP(lambda *args: None); write = WRITE(lambda *args: 0)
    config = Config(abi, b'Color texture-coordinate regression', str(work).encode(),
        str(work/'cache').encode(), 1, c.addressof(ram), len(ram), None, bp_callback,
        write, EPOCH(), EPOCH(), None, None, None, None, RESOLVE(), LOCATE())
    def cp(reg, value): return b'\x08'+bytes([reg])+struct.pack('>I', value)
    def bp(values): return b''.join(b'\x61'+struct.pack('>I', reg<<24|value) for reg, value in values)
    def xf(reg, values, words=False):
        return b'\x10'+struct.pack('>I', (len(values)-1)<<16|reg)+struct.pack('>'+len(values)*('I' if words else 'f'), *values)
    def send(raw):
        raw += bytes((-len(raw))%32)
        data = (c.c_uint8*len(raw)).from_buffer_copy(raw)
        assert api.fifo(data, len(raw)) and api.drain(), 'GPU command submission failed'
    observations = []
    def check(name, expected):
        value = c.c_uint32(); assert api.peek(0, 320, 240, c.byref(value)), 'EFB readback failed'
        actual = ((value.value>>16)&255, (value.value>>8)&255, value.value&255)
        assert all(abs(a-b)<=2 for a,b in zip(actual, expected)), (name, actual, expected)
        observations.append({'check':name, 'actual':actual, 'expected':expected})
    assert api.start(c.byref(config)), 'GPU startup failed'
    try:
        setup = cp(0x50, 1<<9)+cp(0x60, 0)+cp(0x70, 1|(4<<1))+cp(0x80, 0)+cp(0x90, 0)
        setup += xf(0, (1,0,0,0,0,1,0,0,0,0,1,0))+xf(0x400, (1,0,0,0,1,0,0,0,1))
        setup += xf(0x1018, (0,), True)+xf(0x101a, (320,-240,16777215,662,582,16777215))
        setup += b'\x10'+struct.pack('>I', 6<<16|0x1020)+struct.pack('>6fI', 1,0,1,0,-1,0,1)
        setup += xf(0x103f, (1,), True)+xf(0x1040, (0x120,), True)+xf(0x1050, (61,), True)
        setup += xf(0x1009, (2,), True)+xf(0x100e, (0,0,0,0), True)
        setup += xf(0x100a, (0,0,0xff0000ff,0xff0000ff), True)
        setup += bp([(0,0x21),(0x20,(342<<12)|342),(0x21,(981<<12)|821),
            (0x59,171|(171<<10)),(0x28,1<<6),(0x30,7),(0x31,3),
            (0x40,0),(0x41,0x18),(0x43,0),(0xf3,0x3f0000),
            (0xf6,4),(0xf7,14),(0x80,0),(0x84,0),
            (0x88,(1<<20)|(3<<10)|7),(0x94,0x100>>5),
            (0xc0,8|(15<<4)|(15<<8)|(15<<12)|(1<<19)),
            (0xc1,(4<<4)|(7<<7)|(7<<10)|(7<<13)|(1<<19))])
        send(setup)
        positions = b''.join(struct.pack('>3f', x,y,.5) for x,y in ((-1,-1),(1,-1),(1,1),(-1,1)))
        quad = b'\x80\0\4'+positions
        send(quad); check('COLOR0 uses register material without vertex color', (255,255,255))
        send(xf(0x100c, (0x000000ff,), True)+quad)
        check('COLOR0 observes a changed material register', (0,0,0))
        send(xf(0x1040, (0x130,), True)+quad)
        check('COLOR1 uses its own computed channel', (255,255,255))
        # Contrast an explicitly blue raw vertex with the red register material.
        send(cp(0x50, (1<<9)|(1<<13))+cp(0x70, 1|(4<<1)|(5<<14))+
             xf(0x1040, (0x120,), True)+xf(0x100c, (0xff0000ff,), True))
        colored = b'\x80\0\4'+b''.join(struct.pack('>3f', x,y,.5)+bytes([0,0,255,255])
            for x,y in ((-1,-1),(1,-1),(1,1),(-1,1)))
        send(colored); check('COLOR0 texgen uses computed color over raw blue CLR0', (255,255,255))
        # A lit channel is also a texgen dependency even though the TEV stage
        # reads texture only and never requests RASC/RASA.
        send(cp(0x50, 1<<9)+cp(0x70, 1|(4<<1))+
             xf(0x100c, (0xffffffff,), True)+xf(0x100e, (0x406,), True)+
             xf(0x603, (0xff0000ff,), True)+xf(0x604, (1,0,0,1,0,0))+
             xf(0x60a, (0,0,100))+xf(0x60d, (0,0,-1))+quad)
        check('COLOR0 texgen includes enabled light contribution', (255,255,255))
        send(xf(0x100e, (0x402,), True)+quad)
        check('COLOR0 lighting texgen observes the light mask', (0,0,0))
        result = {'result':'passed', 'library_sha256':hashlib.sha256(args.library.read_bytes()).hexdigest(),
            'checks':observations, 'game_qualified':False, 'performance_qualified':False}
        (work/'result.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(result))
    finally:
        api.stop()
        if directory: directory.close()


if __name__ == '__main__': main()
