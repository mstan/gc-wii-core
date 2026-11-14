#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic, asset-free PPC input for the compiled region differential test."""
from pathlib import Path
import argparse
import importlib.util
import struct
import sys


def fixture() -> bytes:
    data = bytearray(struct.pack(">I", 0x60000000) * 2048)
    def put(offset, word):
        struct.pack_into(">I", data, offset, word)
    def branch(source, target, link=False):
        put(source, 0x48000000 | ((target-source) & 0x03fffffc) | int(link))
    def add(offset, register, value):
        put(offset, 0x38000000 | register << 21 | register << 16 | value)
    # Plain tail exit; avoid relying on an interpreter at the fixture boundary.
    branch(0, 0x2000)
    # Local BL and shadow BLR cross in both directions.
    branch(0x10, 0x1020, True)
    add(0x14, 6, 1)
    branch(0x18, 0x2000)
    add(0x1020, 7, 3)
    put(0x1024, 0x4e800020)
    # Callback/store before local BL; callback may invalidate the other page.
    put(0x40, 0x91090000)  # stw r8,0(r9)
    branch(0x44, 0x1060, True)
    add(0x48, 12, 1)
    branch(0x4c, 0x2000)
    add(0x1060, 10, 3)
    put(0x1064, 0x4e800020)
    # Callback in B before a shadow return to A.
    branch(0x80, 0x1080, True)
    add(0x84, 14, 1)
    branch(0x88, 0x2000)
    put(0x1080, 0x91090000)
    add(0x1084, 15, 2)
    put(0x1088, 0x4e800020)
    # Consecutive FP instructions exercise multiline canonical switch entries.
    put(0x100, 0xfc22182a)  # fadd f1,f2,f3
    put(0x104, 0xfc81082a)  # fadd f4,f1,f2
    branch(0x108, 0x2000)
    # Sequential entry into the second page with a nonzero cycle prefix.
    put(0xff8, 0x91090000)
    add(0xffc, 4, 2)
    add(0x1000, 5, 3)
    branch(0x1004, 0x2000)
    return bytes(data)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    tools = Path(__file__).resolve().parents[1] / "tools"
    sys.path.insert(0, str(tools))
    from gen_overlay_region import generate, guard_body
    args.output.mkdir(parents=True, exist_ok=True)
    image = args.output / "synthetic.bin"
    image.write_bytes(fixture())
    for name, base in [("cached", 0x80001000), ("physical", 0x1000), ("uncached", 0xc0001000)]:
        generate(args.compiler, image, 0x1000, base, args.output / name, [], [], "broadway")
    # Fail closed on an owner page, unavailable bytes, mismatching member, or
    # an emitter shape that lacks a full prefix for every canonical label.
    refusals = 0
    bad_page = args.output / "bad-page.bin"
    bad_page.write_bytes(bytes(4096))
    for image_arg, base, pages, excluded in [
        (image, 0x80001000, [], [0xc0002000]),
        (image, 0x80002000, [], []),
        (image, 0x80001000, [bad_page, bad_page], []),
    ]:
        try:
            generate(args.compiler, image_arg, 0x1000, base,
                     args.output / "refused", pages, excluded, "broadway")
        except ValueError:
            refusals += 1
    try:
        guard_body("void bad(void) { switch (ctx->pc) {} }", 0x80001000)
    except ValueError:
        refusals += 1
    assert refusals == 4
    print("synthetic region generation and four refusals passed")


if __name__ == "__main__":
    main()
