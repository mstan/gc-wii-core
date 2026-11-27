// SPDX-License-Identifier: MIT
#include "gcnllr/native_gx.h"
#include <cstdio>

extern "C" const GcnllrNativeGxApi* gcnllr_native_gx_get_api(uint32_t);

static_assert(GCNLLR_NATIVE_GX_DISPLAY_COLOR == 32u);
static_assert(GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES == 512u);
static_assert((GCNLLR_NATIVE_GX_DISPLAY_COLOR & GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES) == 0u);

int main() {
    // Inspect the real provider before initialization: no window/GPU/guest work.
    if (gcnllr_native_gx_get_api(12u) || gcnllr_native_gx_get_api(0u)) return 1;
    const auto* api = gcnllr_native_gx_get_api(GCNLLR_NATIVE_GX_ABI);
    if (!api || api->abi != GCNLLR_NATIVE_GX_ABI || !api->start || !api->fifo ||
        !api->materialize_copy || !api->materialize_copy_range) return 2;
    if ((api->capabilities & GCNLLR_NATIVE_GX_DISPLAY_COLOR) &&
        (api->capabilities & GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES)) return 3;
    std::puts("Native GX ABI rejects old meanings and keeps distinct owner capabilities");
    return 0;
}
