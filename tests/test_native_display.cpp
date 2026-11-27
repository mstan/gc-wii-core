// SPDX-License-Identifier: MIT
#include "../native/gx/display_aspect.hpp"
#include "../native/gx/pointer.hpp"
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <initializer_list>
#define CHECK(value) do{if(!(value)){std::fprintf(stderr,"display check failed: %d\n",__LINE__);std::abort();}}while(0)
int main() {
    using namespace gcnllr::display_aspect;
    CHECK(set({16,9}));const auto widescreen=snapshot();
    for(const auto value:{Aspect{1,0},Aspect{0,1},Aspect{16385,9},Aspect{9,16385},Aspect{1,5},Aspect{5,1}}) {
        CHECK(!set(value));const auto kept=snapshot();CHECK(kept.numerator==16 && kept.denominator==9);
    }
    CHECK(set({}));const auto original=snapshot();
    uint32_t pointer_width=640,pointer_height=456;pointer_dimensions(pointer_width,pointer_height);
    CHECK(!gcnllr::image_pointer(100,360,1,1280,720,1280,720,pointer_width,pointer_height).valid);
    std::thread worker([=] {
        uint32_t width=640,height=456;active=widescreen;apply_dimensions(width,height);
        CHECK(width==16 && height==9); // Earlier queued frame keeps its display mode.
        publish(widescreen);active={};
    });
    worker.join();uint32_t width=640,height=456;apply_dimensions(width,height);
    CHECK(width==640 && height==456); // Worker state does not leak to another owner.
    // Pending already returned to source mode, but input still follows the
    // actual wide presentation until the following frame has been submitted.
    pointer_width=640;pointer_height=456;pointer_dimensions(pointer_width,pointer_height);
    const auto left=gcnllr::image_pointer(0,360,1,1280,720,1280,720,pointer_width,pointer_height);
    const auto right=gcnllr::image_pointer(1279,360,1,1280,720,1280,720,pointer_width,pointer_height);
    CHECK(left.valid && left.x==0 && right.valid && right.x>65000);
    const auto dpi=gcnllr::image_pointer(640,360,1,1280,720,2560,1440,pointer_width,pointer_height);
    CHECK(dpi.valid && dpi.x==32768 && dpi.y==32768);
    CHECK(!gcnllr::image_pointer(500,100,1,1000,1000,2000,2000,pointer_width,pointer_height).valid);
    CHECK(gcnllr::image_pointer(500,500,1,1000,1000,2000,2000,pointer_width,pointer_height).valid);
    std::thread restored([=] {
        active=original;uint32_t w=342,h=256;apply_dimensions(w,h);
        CHECK(w==342 && h==256);publish(original);active={};
    });restored.join();
    pointer_width=342;pointer_height=256;pointer_dimensions(pointer_width,pointer_height);
    CHECK(pointer_width==342 && pointer_height==256); // Existing MM9 source aspect.
    std::puts("native display: queued mode identity, invalid inputs, source fallback and worker isolation PASS");
}
