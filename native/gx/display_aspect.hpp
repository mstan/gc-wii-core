// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <atomic>
namespace gcnllr::display_aspect {
struct Aspect {uint32_t numerator{},denominator{};};
// The simulation/graphics owner sets pending. Each present copies it into its
// queued worker closure; later owner edits cannot change an earlier frame.
inline std::atomic<uint64_t> pending{};
inline std::atomic<uint64_t> presented{};
inline thread_local Aspect active;
inline Aspect snapshot() noexcept {
    const uint64_t value=pending.load(std::memory_order_acquire);
    return {uint32_t(value>>32),uint32_t(value)};
}
inline bool set(Aspect value) noexcept {
    if(value.numerator || value.denominator) {
        if(!value.numerator || !value.denominator || value.numerator>16384 || value.denominator>16384 ||
           uint64_t(value.numerator)*4<value.denominator || uint64_t(value.denominator)*4<value.numerator)return false;
    }
    pending.store(uint64_t(value.numerator)<<32|value.denominator,std::memory_order_release);return true;
}
inline void apply_dimensions(uint32_t& width,uint32_t& height) noexcept {
    if(active.numerator){width=active.numerator;height=active.denominator;}
}
inline void publish(Aspect value) noexcept {
    presented.store(uint64_t(value.numerator)<<32|value.denominator,std::memory_order_release);
}
inline void pointer_dimensions(uint32_t& width,uint32_t& height) noexcept {
    const uint64_t value=presented.load(std::memory_order_acquire);
    if(value){width=uint32_t(value>>32);height=uint32_t(value);}
}
}
