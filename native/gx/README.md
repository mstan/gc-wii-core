# Native GX service

Extracted from `mstan/gcnllr` commit `c05553224c95ccffcab3336da14d9652592ddf08`,
`src/native_services` and `tools/aurora_retail.py`. Original MIT notices remain.
This library adapts the pinned donor Aurora dependencies; it does not link a
donor game executable. The package's software GX implementation remains the
low-level reference. Machine/FIFO policy and host loading remain consumers.

Include `attach.cmake` in the donor dependency build, with `GXRUNTIME_DIR`
and its Aurora/GX targets provided by that build. The graphics ABI is declared
in `include/gcnllr/native_gx.h`. ABI11 adds paired consumer memory resolution
and reverse-address callbacks, keeping physical identities in copy/epoch
callbacks. Consumers own memory maps, coherence, completion, scanout selection,
audio and persistent paths. The old single-MEM1 setup is retained for GameCube.

ABI12 appends focused, image-relative mouse state to the status result. Pointer
coordinates are normalized to 0..65535 inside the aspect-fitted image, accounting
for logical window units and physical pixel dimensions. Positions outside the
image or an unfocused/hidden window are invalid. Button bits are left/right/middle;
consumers map them to their own input devices. Rebuild consumers for the new ABI.

`GC_WII_GX_WII=ON` selects Wii texture/TLUT/copy address fields when building
the service. The default retains GameCube masks. `test_memory_gpu.py` exercises
real FIFO draws from MEM2 textures, MEM2 XFB writes, CPU rewrites and bank
isolation against that build. It requires `--library` and `--work-root` paths.

Extraction and multi-bank support require consumer build/runtime qualification;
the service test does not establish a working Wii machine or menu rendering.

The optional `gc_wii_native_render_get_api` export in
`include/gcnllr/native_render.h` accepts native meshes and material draws without
constructing or decoding FIFO commands. Geometry is copied and converted once;
draws reuse it with explicit transforms, clipping and material inputs. The
current material family is unlit vertex color with one optional texture and one
mip level. The existing host GPU service still owns lifecycle, memory coherence,
copies and scanout. Calls run on its recording owner after earlier FIFO work has
been drained. Consumers select the rendering provider when building.

`test_native_render_gpu.py` checks actual GPU pixels, mesh ownership/reuse,
texture rewrites, ordering, clipping, transforms and zero submitted FIFO bytes.
It uses a hidden window and muted output. This is an experimental component:
title bindings, complete model/material families and end-to-end performance
qualification remain open. Retained geometry is currently staged to the GPU
through the shared draw backend; permanent GPU mesh residency is not claimed.

`GC_WII_NATIVE_J3D=ON` additionally builds the native BMD/BDL material and mesh
provider, exported by `gc_wii_native_j3d_get_api`. See `native/j3d/README.md` and
`include/gcnllr/native_j3d.h`. Its per-instance rendering inputs are explicit;
missing poses, texture matrices or referenced lights reject the draw. These
experimental APIs do not enable a title replacement automatically.
