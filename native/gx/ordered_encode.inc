// SPDX-License-Identifier: MIT
// Included in Aurora's gfx namespace. The caller has just resolved/sealed the
// preceding pass. Record into the same packet before any following pass: no
// submission, staging unmap or frame-slot turnover is needed for this copy.
// Actual CPU/PE observations still submit the packet before waiting for bytes.
void machine_encode_after_resolve(std::function<void(wgpu::CommandEncoder&)> encode) {
  current_frame_packet().completion.record();
  const size_t slot = g_recordingFrameSlot;
  render_worker::enqueue_encode_pass(current_frame_packet().frameId, 0,
      [slot, encode = std::move(encode)] { encode(g_framePackets[slot].encoder); });
}
// Seal preceding EFB work, then append scanout composition and conversion to
// that packet. Frame end remains responsible for unmapping staging and submit.
void machine_encode_tail(std::function<void(wgpu::CommandEncoder&)> encode) {
  finish();
  machine_encode_after_resolve(std::move(encode));
}
