// SPDX-License-Identifier: MIT
// Included in Aurora's gfx namespace after segment_frame. No packet turnover,
// staging release, resource retirement or worker acknowledgement for a no-op.
// Physical completion and CPU/PE observers continue to call segment_frame.
bool submit_frame_if_needed() {
    if (g_recordingFrame && !g_inOffscreen && g_recordingFrame->completion.empty)
        return true;
    return segment_frame();
}
