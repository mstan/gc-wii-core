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

`gc_wii_native_gui_get_api` adds a named GUI color-range material while keeping
RenderAPI1 unchanged. It shares the same mesh handles, texture resources and
recording owner. One texture is mapped between byte RGBA endpoints, clamped,
then multiplied by pane color; a flat material uses its high endpoint instead.
The existing integer shader compiler lowers that complete expression. Wider
source colors and unsupported inputs reject before submission. Run the same
component check with `--gui` for range, tint, alpha and rejection coverage.

`gc_wii_native_gui_layers_get_api` accepts a fixed three-image GUI background:
remap the primary RGB range, then add two weighted overlays with saturation
after each operation. Opacity uses the high endpoint. Each overlay has its own
affine transform of the retained primary UVs. Resources share the existing
multi-texture cache and bank resolution. GUI1, Render1 and Frame1 layouts remain
unchanged. Add `--layers` to the component check for independent coordinates,
texture rewrites, saturation and explicit unsupported-input rejection.

`GC_WII_NATIVE_J3D=ON` additionally builds the native BMD/BDL material and mesh
provider, exported by `gc_wii_native_j3d_get_api`. See `native/j3d/README.md` and
`include/gcnllr/native_j3d.h`. Its per-instance rendering inputs are explicit;
missing poses, texture matrices or referenced lights reject the draw. These
experimental APIs do not enable a title replacement automatically.

Color-source texture generators consume the computed XF color, including its
material registers and lighting. The selected Aurora renderer applies scoped
source adaptations for this dependency and COLOR0/COLOR1 selection without
editing the donor checkout. `test_color_texgen_gpu.py` checks actual GPU pixels
for missing/contrasting raw vertex colors, register changes and light masks.

`gc_wii_native_output_get_api(1)` supplies an optional owned-output submission
boundary, separate from NativeFrame1. It requires the serial owner profile and
all three memory-ownership callbacks. The caller must guard every actual memory
observer, including raw compiled pointers. Submission acknowledges host encoding
and immutable resource capture without waiting for GPU execution or copying
bytes into RAM. It emits no PE notification. NativeFrame1 completion retains
its existing physical/owned policy. Retained display generations use the same
YUY2 presentation shader as ordinary CPU-backed scanout.

The existing `test_native_frame_gpu.py --output --efb-height 528` exercises the
new submission contract and compares all pixels of a colored direct scanout
with its materialized YUY2 presentation. It remains a hidden, silent component
check; title correctness and throughput require their own replay gates.
