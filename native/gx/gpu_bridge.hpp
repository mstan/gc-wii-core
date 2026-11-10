// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#if GCNLLR_ASYNC_COPIES
#include "async_readback.hpp"
#endif
namespace gxruntime::gxcore { struct EfbCopyCommand; }
namespace gcnllr::gpu_bridge {
void initialize();
bool settle_framebuffer();
struct CopiedImage {uint32_t width{},height{};std::vector<uint8_t> yuy2;};
struct EncodedImage;
using EncodedImageHandle=std::shared_ptr<const EncodedImage>;
struct DisplayCopyFilter {
    uint32_t low{},high{};
    bool clamp_top{},clamp_bottom{};
};
void record_display_copy(const gxruntime::gxcore::EfbCopyCommand& copy,const DisplayCopyFilter& filter);
std::vector<CopiedImage> take_display_copies();
// Immutable encoded YUY2 generations. The batched provider records their GPU
// work in the current packet; observation submits before consuming real bytes.
std::vector<EncodedImageHandle> retain_display_copies();
// Submit any lazily recorded generations before CPU observation/PE completion.
// Segmented providers already submitted them; no live provider switch exists.
void submit_copies();
void validate_submission();
// Await host encoding/submission acknowledgement, without GPU execution,
// transfer or map completion. Propagate deferred encoding errors before PE.
void acknowledge_submission();
CopiedImage materialize_display_copy(const EncodedImageHandle&);
#if GCNLLR_ASYNC_COPIES
aurora::gfx::AsyncReadback queue_display_readback(const EncodedImageHandle&);
#endif
bool present_display_copy(const EncodedImageHandle&,uint32_t width,uint32_t height);
struct DisplaySpan {EncodedImageHandle image;uint32_t source,destination,size;std::vector<uint8_t> cpu;};
bool present_display_spans(const std::vector<DisplaySpan>&,uint32_t width,uint32_t height);
std::vector<uint8_t> read_efb(uint32_t& width,uint32_t& height);
std::vector<uint32_t> read_depth(uint32_t& width,uint32_t& height);
void release_depth();
void clear_efb(const gxruntime::gxcore::EfbCopyCommand& copy);
bool present(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride);
bool peek_scanout(uint16_t x,uint16_t y,uint32_t* value);
void finish();
void release();
}
