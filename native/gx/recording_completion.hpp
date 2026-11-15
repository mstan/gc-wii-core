// SPDX-License-Identifier: MIT
#pragma once

namespace gcnllr {
// Owned by the recording packet, alongside its staging and retained resources.
// Unknown/default packets seal normally. Only a fresh EFB-preserving continuation
// is empty; every recorded GPU/observer operation revokes that property.
struct RecordingCompletion {
    bool empty = false;
    void begin(bool preserve_efb) noexcept { empty = preserve_efb; }
    void record() noexcept { empty = false; }
};
}
