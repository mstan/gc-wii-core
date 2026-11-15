// SPDX-License-Identifier: MIT
// Exercise the production completion/ordered-encode bodies without a GPU.
#include "recording_completion.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#define CHECK(v) do { if (!(v)) throw std::runtime_error(#v); } while (0)

namespace wgpu {
struct CommandEncoder { uint64_t owner{}; };
enum class CallbackMode { WaitAnyOnly };
enum class QueueWorkDoneStatus { Success, Error };
struct StringView {};
}
namespace fixture {
struct FramePacket {
    gcnllr::RecordingCompletion completion;
    uint64_t frameId{};
    wgpu::CommandEncoder encoder;
    std::vector<std::shared_ptr<int>> retained;
};
static std::array<FramePacket,2> g_framePackets;
static FramePacket* g_recordingFrame;
static size_t g_recordingFrameSlot;
static bool g_inOffscreen,seal_failure;
static unsigned seals,syncs,validations,waits;
static bool validation_failure,gpu_success;
static std::deque<std::function<void()>> pending;
static std::vector<std::string> order;
static FramePacket& current_frame_packet() { CHECK(g_recordingFrame);return *g_recordingFrame; }
static void drain() { while(!pending.empty()){auto f=std::move(pending.front());pending.pop_front();f();} }
static void finish() { current_frame_packet().completion.record();order.push_back("finish"); }
static bool segment_frame() {
    ++seals;
    if(seal_failure)return false;
    drain();order.push_back("submit");
    auto& packet=current_frame_packet();const uint64_t next=packet.frameId+1;
    packet={};packet.frameId=next;packet.encoder.owner=next;packet.completion.begin(true);
    return true;
}
namespace render_worker {
static void enqueue_encode_pass(uint64_t id,uint32_t,std::function<void()> work) {
    // A queued encode must retain the exact recording packet until submission.
    pending.push_back([id,work=std::move(work)] {CHECK(current_frame_packet().frameId==id);work();});
}
}
#include "recording_completion.inc"
#include "ordered_encode.inc"
static void reset(bool continuation) {
    pending.clear();g_framePackets={};g_recordingFrameSlot=1;g_recordingFrame=&g_framePackets[1];
    g_recordingFrame->frameId=41;g_recordingFrame->encoder.owner=41;
    g_recordingFrame->completion.begin(continuation);
    g_inOffscreen=seal_failure=validation_failure=false;gpu_success=true;
    seals=syncs=validations=waits=0;order.clear();
}
}
namespace gpu {
struct Queue {
    template<class Callback> int OnSubmittedWorkDone(wgpu::CallbackMode,Callback cb) {
        cb(fixture::gpu_success?wgpu::QueueWorkDoneStatus::Success:wgpu::QueueWorkDoneStatus::Error,{});return 1;
    }
} g_queue;
struct Instance {void WaitAny(int,uint64_t){++fixture::waits;}} g_instance;
}
namespace worker {
static void enqueue_work(std::function<void()> work) {fixture::pending.push_back(std::move(work));}
static void synchronize(){++fixture::syncs;fixture::drain();}
}
namespace bridge {
namespace gfx=fixture;
static bool recorded_copies;
static void validate_submission(){++fixture::validations;if(fixture::validation_failure)throw std::runtime_error("deferred encode failed");}
#include "frame_completion.inc"
}
template<class F> static void fails(F&& f) {
    bool failed=false;try {f();}catch(const std::runtime_error&){failed=true;}CHECK(failed);
}
int main() {
    using namespace fixture;
    try {
        // A real frame's initial clear is work, including an otherwise drawless frame.
        reset(false);bridge::complete_output();CHECK(seals==1 && validations==1);
        CHECK(syncs==(GCNLLR_NATIVE_QUEUED_OUTPUT?0u:1u));

        // A fresh continuation holds its packet/staging/resource identity across no-ops.
        reset(true);
        auto resource=std::make_shared<int>(7);std::weak_ptr<int> weak=resource;
        current_frame_packet().retained.push_back(resource);resource.reset();
        auto* owner=g_recordingFrame;const auto slot=g_recordingFrameSlot;
        for(unsigned i=0;i<3;++i)bridge::complete_output();
        CHECK(seals==0 && validations==3 && g_recordingFrame==owner && slot==g_recordingFrameSlot);
        CHECK(owner->frameId==41 && owner->encoder.owner==41 && !weak.expired());

        // The actual direct-XFB-encode include records work although no op vector exists.
        pending.push_back([]{order.push_back("begin");});
        machine_encode_after_resolve([keep=weak.lock()](wgpu::CommandEncoder& encoder){
            CHECK(keep && *keep==7 && encoder.owner==41);order.push_back("encode");
        });
        CHECK(!current_frame_packet().completion.empty);
        bridge::complete_output();
        CHECK(seals==1 && pending.empty() && current_frame_packet().frameId==42 && weak.expired());
        CHECK((order==std::vector<std::string>{"begin","encode","submit"}));

        // Tail encodes preserve the production finish-before-encode ordering.
        reset(true);machine_encode_tail([](wgpu::CommandEncoder&){order.push_back("tail");});
        bridge::complete_output();CHECK((order==std::vector<std::string>{"finish","tail","submit"}));

        // Any recorded effect, default/unknown packet, or offscreen recording seals.
        reset(true);current_frame_packet().completion.record();bridge::complete_output();CHECK(seals==1);
        reset(true);current_frame_packet().completion={};bridge::complete_output();CHECK(seals==1);
        reset(true);g_inOffscreen=true;bridge::complete_output();CHECK(seals==1);

        // A failed seal cannot clear work, advance generation, or retire queued resources.
        reset(true);resource=std::make_shared<int>(9);weak=resource;
        machine_encode_after_resolve([keep=resource](wgpu::CommandEncoder&){CHECK(*keep==9);});resource.reset();
        seal_failure=true;bridge::recorded_copies=true;
        fails([]{bridge::complete_output();});fails([]{bridge::complete_output();});
        CHECK(seals==2 && validations==0 && current_frame_packet().frameId==41);
        CHECK(!current_frame_packet().completion.empty && !weak.expired() && pending.size()==1 && bridge::recorded_copies);
        seal_failure=false;bridge::complete_output();CHECK(weak.expired() && current_frame_packet().frameId==42);

        // Deferred failure is still observed when an empty completion emits no packet.
        reset(true);validation_failure=true;fails([]{bridge::complete_output();});
        CHECK(seals==0 && validations==1 && current_frame_packet().frameId==41);

        // The actual physical body always seals and waits, including an empty continuation.
        reset(true);bridge::complete_frame(true);CHECK(seals==1 && syncs==1 && waits==1 && validations==1);
        reset(true);gpu_success=false;fails([]{bridge::complete_frame(true);});CHECK(seals==1 && waits==1);
        // OWNED_COMPLETION PE callers pass false too; that route must still seal.
        reset(true);bridge::complete_frame(false);CHECK(seals==1 && validations==1);
        // Existing CPU/PE observers call the unconditional segment entry directly.
        reset(true);CHECK(segment_frame());CHECK(seals==1 && current_frame_packet().frameId==42);
        std::cout<<"recording completion PASS (queued="<<GCNLLR_NATIVE_QUEUED_OUTPUT<<")\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
