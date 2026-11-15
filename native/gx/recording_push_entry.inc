// SPDX-License-Identifier: MIT
// Pinned push_command entry: preserve the original outside-frame drop before
// inspecting packet state. Shared with the focused CPU entry-contract fixture.
if (g_currentRenderPass == UINT32_MAX)
  UNLIKELY {
    Log.warn("Dropping command {}", magic_enum::enum_name(type));
    return;
  }
if (type != CommandType::SetViewport && type != CommandType::SetScissor)
  current_frame_packet().completion.record();
