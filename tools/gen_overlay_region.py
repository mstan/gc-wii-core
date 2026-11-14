#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Emit one optional 8 KiB region from two pages observed in the SAME image.

The existing per-page catalog remains authoritative.  Consumers link region.c
and include overlay_regions.inc, retain their original member functions, and
forward invalidations to the optional region API.  members/*.c are compatibility
fixtures, not additional sources to link beside an existing page catalog.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

from gen_overlay_module import fnv1a64

PAGE = 4096
CASE = re.compile(r"^\s*case 0x([0-9A-Fa-f]{8})u:(.*?)(?=^\s*(?:case 0x[0-9A-Fa-f]{8}u:|default:))", re.M | re.S)
LABEL = re.compile(r"^label_([0-9A-Fa-f]{8}):\s*$", re.M)


def physical(address: int) -> int:
    if address >> 29 not in (0, 4, 6) or address & 0x1fffffff >= 0x1800000:
        raise ValueError("address is outside a supported MEM1 PC namespace")
    return address & 0x1fffffff


def guard_body(source: str, base: int) -> str:
    """Preserve the emitter's cycle-prefix representation on every exit."""
    switch = source.index("    switch (ctx->pc) {")
    first_label = LABEL.search(source)
    if first_label is None:
        raise ValueError("emitted body has no instruction labels")
    cases = {}
    for match in CASE.finditer(source, switch, first_label.start()):
        address, block = match.groups()
        targets = re.findall(r"goto label_([0-9A-Fa-f]{8});", block)
        prefixes = re.findall(r"dr_cycles -= (\d+)u;", block)
        if targets != [address] or len(prefixes) > 1:
            raise ValueError("canonical case/label mismatch")
        pc = int(address, 16)
        if pc in cases:
            raise ValueError("duplicate canonical case")
        cases[pc] = int(prefixes[0]) if prefixes else 0
    expected = set(range(base, base + 2 * PAGE, 4))
    labels = [int(m.group(1), 16) for m in LABEL.finditer(source)]
    if set(cases) != expected or set(labels) != expected or len(labels) != len(expected):
        raise ValueError("requires one complete, canonical 2048-instruction body")

    def entry(match: re.Match) -> str:
        pc = int(match.group(1), 16)
        prefix = cases[pc]
        # A switch entry subtracts this prefix; a local edge/fallthrough does
        # not.  On a decline charge it once, exactly as the original exit does.
        return (match.group(0) + "\n"
                f"    if (dr_region_page != 0x{pc & ~4095:08X}u) {{\n"
                f"        if (!gcn_overlay_region_cross(&dr_region_invocation, ctx, 0x{pc:08X}u,\n"
                f"                                      dr_cycles + {prefix}u, dr_deadline)) {{\n"
                f"            ctx->pc = 0x{pc:08X}u;\n"
                f"            dr_cycles += {prefix}u;\n"
                "            DR_RET();\n"
                "        }\n"
                f"        dr_region_page = 0x{pc & ~4095:08X}u;\n"
                "    }")

    source = LABEL.sub(entry, source)
    source = source.replace("    switch (ctx->pc) {",
        "    const GcnOverlayRegionInvocation dr_region_invocation = gcn_overlay_region_invocation();\n"
        "    u32 dr_region_page = ctx->pc & ~4095u;\n"
        "    switch (ctx->pc) {", 1)
    return source.replace('#include "../generated_abi.h"',
                          '#include "generated_abi.h"\n#include "cpu/overlay_region.h"', 1)


def emit(compiler: Path, image: Path, base: int, suffix: str,
         instructions: int, destination: Path, cpu: str) -> tuple[str, bytes]:
    subprocess.run([str(compiler), "--gamecube-ipl", str(image), str(destination),
                    "--base", hex(base), "--entry", hex(base),
                    "--chunk-instructions", str(instructions),
                    "--symbol-suffix", suffix, "--cpu", cpu],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    chunks = list((destination / "generated/chunks").glob("*.c"))
    if len(chunks) != 1:
        raise ValueError("compiler did not emit exactly one body")
    return chunks[0].read_text(encoding="utf-8"), (destination / "generated/generated_abi.h").read_bytes()


def generate(compiler: Path, image: Path, image_base: int, base: int,
             output: Path, pages: list[Path], exclude: list[int], cpu: str) -> dict:
    start = physical(base)
    if base & 4095 or start + 2 * PAGE > 0x1800000:
        raise ValueError("region must be two adjacent aligned MEM1 pages")
    if any((physical(p) & ~4095) in (start, start + PAGE) for p in exclude):
        raise ValueError("region overlaps an excluded owner page")
    offset = start - physical(image_base)
    if offset < 0:
        raise ValueError("region precedes the supplied co-observed image")
    with image.open("rb") as stream:
        stream.seek(offset)
        data = stream.read(2 * PAGE)
    if len(data) != 2 * PAGE:
        raise ValueError("co-observed image does not contain the complete region")
    if pages and (len(pages) != 2 or any(p.read_bytes() != data[i*PAGE:(i+1)*PAGE]
                                       for i, p in enumerate(pages))):
        raise ValueError("member page bytes do not match the co-observed image")
    output.mkdir(parents=True, exist_ok=True)
    members = []
    with tempfile.TemporaryDirectory(prefix="gcn-region-") as temporary:
        work = Path(temporary)
        abi = None
        (output / "members").mkdir(exist_ok=True)
        for i in range(2):
            address = base + i * PAGE
            chunk = data[i*PAGE:(i+1)*PAGE]
            digest = fnv1a64(chunk)
            suffix = f"_h{digest:016X}"
            blob = work / f"page{i}.bin"
            blob.write_bytes(chunk)
            source, member_abi = emit(compiler, blob, address, suffix, 1024, work / f"member{i}", cpu)
            if abi is not None and abi != member_abi:
                raise ValueError("member ABI mismatch")
            abi = member_abi
            relative = f"members/page_{address:08X}.c"
            (output / relative).write_text(source, encoding="utf-8")
            members.append(dict(start=address, hash=f"{digest:016X}",
                                symbol=f"func_{address:08X}{suffix}", source=relative))
        blob = work / "pair.bin"
        blob.write_bytes(data)
        suffix = f"_region_{fnv1a64(data):016X}"
        source, region_abi = emit(compiler, blob, base, suffix, 2048, work / "region", cpu)
        if region_abi != abi:
            raise ValueError("combined/member ABI mismatch")
        (output / "generated_abi.h").write_bytes(abi)
        (output / "region.c").write_text(guard_body(source, base), encoding="utf-8")
    symbol = f"func_{base:08X}{suffix}"
    declarations = "\n".join(f"extern void {m['symbol']}(CPUState*);" for m in members)
    (output / "overlay_regions.inc").write_text(
        "/* Generated; link region.c and retain the original catalog members. */\n"
        '#include "cpu/overlay_region.h"\n' + declarations + "\n"
        f"extern void {symbol}(CPUState*);\n"
        "static const GcnOverlayFn native_region_members_0[] = {\n    " +
        ",\n    ".join(m["symbol"] for m in members) + "\n};\n"
        "static GcnOverlayRegion native_regions[] = {\n"
        f"    {{.start=0x{base:08X}u,.page_count=2,.members=native_region_members_0,.body={symbol}}}\n"
        "};\n#define GCN_OVERLAY_REGION_COUNT 1u\n", encoding="utf-8")
    manifest = dict(start=base, page_count=2, members=members, body=symbol,
                    coobserved_image=str(image), image_offset=offset,
                    tuple_sha256=hashlib.sha256(data).hexdigest(),
                    compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                    cpu=cpu, guard_labels=2048, region_source="region.c",
                    descriptor="overlay_regions.inc", compatibility_sources="members/*.c; do not duplicate existing catalog members")
    (output / "region.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dolrecomp", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True, help="one co-observed MEM1 image (or an exact 8KiB slice)")
    parser.add_argument("--image-base", type=lambda s: int(s, 0), default=0x80000000)
    parser.add_argument("--base", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--page", type=Path, action="append", default=[], help="optional two exact original page blobs, in address order")
    parser.add_argument("--exclude-page", type=lambda s: int(s, 0), action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpu", choices=("gekko", "broadway", "espresso"), default="broadway")
    args = parser.parse_args()
    try:
        print(json.dumps(generate(args.dolrecomp, args.image, args.image_base, args.base,
                                  args.output, args.page, args.exclude_page, args.cpu), indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"region generation failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
