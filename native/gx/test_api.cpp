// SPDX-License-Identifier: MIT
#include "gcnllr/native_gx.h"
#include "gcnllr/native_frame.h"
#include "gcnllr/native_resolution.h"
#include <cstdio>

extern "C" const GcnllrNativeGxApi* gcnllr_native_gx_get_api(uint32_t);
extern "C" const GcWiiNativeFrameApi* gc_wii_native_frame_get_api(uint32_t);
extern "C" const GcWiiNativeResolutionApi* gc_wii_native_resolution_get_api(uint32_t);

static_assert(GCNLLR_NATIVE_GX_DISPLAY_COLOR == 32u, "Retain the display-owner capability");
static_assert(GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES == 512u, "Use a distinct serial-memory capability");
static_assert((GCNLLR_NATIVE_GX_DISPLAY_COLOR & GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES) == 0u,
              "Display and serial-memory ownership cannot alias");

int main() {
    // Inspect the real provider before initialization: no window/GPU/guest work.
    if (gcnllr_native_gx_get_api(12u) || gcnllr_native_gx_get_api(0u)) return 1;
    const auto* api = gcnllr_native_gx_get_api(GCNLLR_NATIVE_GX_ABI);
    if (!api || api->abi != GCNLLR_NATIVE_GX_ABI || !api->start || !api->fifo ||
        !api->materialize_copy || !api->materialize_copy_range) return 2;
    if ((api->capabilities & GCNLLR_NATIVE_GX_DISPLAY_COLOR) &&
        (api->capabilities & GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES)) return 3;
    if (gc_wii_native_frame_get_api(999u) || gc_wii_native_resolution_get_api(999u)) return 4;
    if (bool(gc_wii_native_frame_get_api(GC_WII_NATIVE_FRAME_ABI)) != bool(TEST_NATIVE_TYPED_OUTPUT) ||
        bool(gc_wii_native_resolution_get_api(GC_WII_NATIVE_RESOLUTION_ABI)) != bool(TEST_NATIVE_TYPED_OUTPUT)) return 5;
    std::puts("Native GX ABI rejects old meanings and keeps distinct owner capabilities");
    return 0;
}
