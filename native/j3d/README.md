# Native J3D resources

`import_model` converts BMD3/BDL4 resources into owned host geometry, topology,
matrix palettes, material equations, texture/mipmap data, joints, envelopes and
the scene hierarchy. It reads VTX1/SHP1/MAT3/TEX1 and the skeletal sections
directly. No live FIFO, command processor or guest routine is executed.
Layouts and defaults follow zeldaret/tww's CC0 J3D declarations; the importer
implementation is original MIT code.

Loaded models can share images and palettes with another resource. Supply the
optional consumer memory resolver and the model's guest address for these
references. The resolver must return exactly the requested checked range.
Standalone file imports reject out-of-resource references. Resource conversion
copies data and does not borrow guest pointers. Consumers own coherence and
the lifetime of their native model instances.

Build this directory directly with CMake for the ROM-free checks, or select
`GC_WII_J3D_ASSETS=ON` in the shared core. The option defaults off, preserving
the base LLE build's compiler requirements. The importer requires C++20.
`test_j3d_assets <model>` checks an original local model; its `--ram` mode can
check loaded models in a private MEM1 fixture with external resource resolution.
Original content and generated comparison output stay outside source control.

This is the asset conversion portion of the native renderer. Material recipes
still require host shader compilation and runtime animation inputs; it does not
establish a working game binding or a speedup. Unavailable material recipes
remain explicit. Consumers must reject unsupported rendering rather than
silently substituting a different picture or a legacy GX route.
