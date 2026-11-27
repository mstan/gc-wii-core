// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gcnllr {
// EFB storage is independent of the VI scanout and the host window. Preserve
// the qualified GameCube profile while exposing the Wii's complete raster.
inline constexpr uint32_t efb_width = 640;
inline constexpr uint32_t efb_height = GC_WII_GX_WII ? 528 : 480;
}
