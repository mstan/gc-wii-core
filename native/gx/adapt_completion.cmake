# SPDX-License-Identifier: MIT
# Adapt only the pinned donor; fail closed if any actual recording seam changes.
macro(_completion_replace _old _new)
  string(FIND "${_gcnllr_common}" "${_old}" _completion_at)
  if(_completion_at LESS 0)
    message(FATAL_ERROR "Pinned completion recording seam differs: ${_old}")
  endif()
  string(REPLACE "${_old}" "${_new}" _gcnllr_common "${_gcnllr_common}")
endmacro()
_completion_replace("#include \"common.hpp\""
  "#include \"${CMAKE_CURRENT_LIST_DIR}/recording_completion.hpp\"\n#include \"common.hpp\"")
_completion_replace("struct FramePacket {" "struct FramePacket {\n  gcnllr::RecordingCompletion completion;")
_completion_replace("frame.continuation = preserveEfb;"
  "frame.continuation = preserveEfb;\n  frame.completion.begin(preserveEfb);")
# Keep the caller's existing dropped-command path ahead of any packet access.
set(_completion_push_entry [=[static inline void push_command(CommandType type, const Command::Data& data) {
  if (g_currentRenderPass == UINT32_MAX)
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
]=])
_completion_replace("${_completion_push_entry}"
  "static inline void push_command(CommandType type, const Command::Data& data) {\n#include \"${CMAKE_CURRENT_LIST_DIR}/recording_push_entry.inc\"\n")
# Any queued pass/op is conservative work, including observable depth snapshots.
_completion_replace("static void enqueue_op(FramePacket& frame, size_t frameSlot, uint32_t opIndex) {"
  "static void enqueue_op(FramePacket& frame, size_t frameSlot, uint32_t opIndex) {\n  frame.completion.record();")
foreach(_completion_seam
    "void queue_texture_upload(TextureUpload upload) {"
    "void begin_color_pass(const ColorPassDescriptor& desc) {"
    "void queue_palette_conv(tex_palette_conv::ConvRequest req) {"
    "void begin_offscreen(uint32_t width, uint32_t height) {")
  _completion_replace("${_completion_seam}"
    "${_completion_seam}\n  current_frame_packet().completion.record();")
endforeach()
# Copy clears are observable without a Draw command; do not infer emptiness
# from command or operation vectors. Direct native encodes mark their own seam.
_completion_replace("  pass.machineClearPipeline = pipeline;"
  "  current_frame_packet().completion.record();\n  pass.machineClearPipeline = pipeline;")
_completion_replace("bool staging_has_capacity(size_t vertLength, size_t indexLength,"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/recording_completion.inc\"\nbool staging_has_capacity(size_t vertLength, size_t indexLength,")

