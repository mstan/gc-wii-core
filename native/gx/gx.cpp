// SPDX-License-Identifier: MIT
#include "gcnllr/native_gx.h"
#include "memory.hpp"
#include "texture_encode.hpp"
#include "owned_texture.hpp"
#include "raster_viewport.hpp"
#include "pointer.hpp"
#include "gpu_bridge.hpp"
#include "efb_geometry.hpp"
#include "native_scene.hpp"
#include "native_gui.hpp"
#include "native_gui_effects.hpp"
#include "gcnllr/native_frame.h"
#include "gcnllr/native_output.h"
#include "gcnllr/native_presentation.h"
#include "gcnllr/native_video.h"
#if GC_WII_NATIVE_J3D
#include "native_j3d.hpp"
#endif
#include <unordered_map>
#include "fifo_worker.hpp"
#include "ordered_effects.hpp"
#include "gxcore_draw.hpp"
#if GCNLLR_ASYNC_COPIES
#include "gfx/texture.hpp"
#endif
#include "gfx/thread_cpu.hpp"
#include "window.hpp"
#if GCNLLR_NATIVE_DRAW_SINK
#include "native_draw_sink.hpp"
#endif
#if GCNLLR_RETAIL_PIPELINE
#include <gxruntime/gx_recomp.h>
#include "native_retail.hpp"
#include "retail_texture_identity.hpp"
#include "retail_copy.hpp"
#include "gx/recomp.hpp"
#include "gx/gx.hpp"
extern "C" void gcnllr_aurora_retail_reset();
#else
#include <gxruntime/aurora_recomp/retail_gx_frontend.hpp>
#include <gxruntime/gxcore/gxcore.hpp>
#endif
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace aurora::gfx::gxcore {
bool submit_draw_plan(const gxruntime::gxcore::DrawPlan&);
void copy_efb_to_texture(const gxruntime::gxcore::EfbCopyCommand&);
std::vector<uint8_t> read_efb_copy(const gxruntime::gxcore::EfbCopyCommand&);
void reset_texture_cache();
void note_frame_presented();
void publish_efb_copy_memory(uint32_t,const void*,uint32_t,uint32_t,uint32_t,uint32_t);
void efb_copy_memory_stats(uint64_t*,uint64_t*);
}
namespace aurora::gx::fifo { void drain(); }
namespace {
constexpr uint32_t guest_gpu_address_mask=GC_WII_GX_WII ? 0x00ffffffu:0x001fffffu;
using Clock = std::chrono::steady_clock;
#if GCNLLR_RETAIL_PIPELINE
using RetailGxFrontend=gcnllr::NativeRetailFrontend;
bool retail_active{};
gcnllr::RetailTextureIdentity retail_texture_ids;
#else
using gxruntime::aurora_recomp::RetailGxFrontend;
#endif
#if !GCNLLR_RETAIL_PIPELINE
#if GCNLLR_NATIVE_DRAW_SINK
using GxCoreSink=gcnllr::NativeDrawSink;
#else
using gxruntime::gxcore::GxCoreSink;
#endif
#endif
void flush_display_copies();
GcnllrNativeGxConfig config{};
GcnllrNativeGxStatus state{};
GcWiiNativeFrameStatus native_frame_state{};
std::atomic<bool> failed{};
std::atomic<uint64_t> readback_ns{};
std::atomic<uint64_t> resident_textures{},resident_texture_hits{},resident_texture_materializations{},resident_texture_incompatible{};
std::unique_ptr<gcnllr::FifoWorker> fifo_worker;
// Adapted donor recording ownership: a frame transition cannot close Aurora's
// packet beneath the translator. Queue waits always occur outside this lock.
std::mutex recording_mutex;
thread_local std::unique_lock<std::mutex>* worker_recording{};
struct RecordingBatch {
    std::unique_lock<std::mutex> lock{recording_mutex,std::defer_lock};
    RecordingBatch() {
        if(GCNLLR_GX_COOPERATIVE_VI)lock.lock();
        worker_recording=&lock;
    }
    ~RecordingBatch() {worker_recording=nullptr;}
};
void machine_effect(std::function<void()> effect);
void ordered_bp_effect(uint32_t reg,uint32_t value);
void retire_streams(bool wait);
uint64_t efb_generation=1,color_peek_generation{},depth_peek_generation{};
std::vector<uint8_t> color_peek;
std::vector<uint32_t> depth_peek;
uint32_t color_peek_width{},color_peek_height{},depth_peek_width{},depth_peek_height{};
std::unique_ptr<RetailGxFrontend> frontend;
#if GCNLLR_RETAIL_PIPELINE
// Aurora owns all command/state/vertex work. The machine retains only the BP
// registers needed for caller-visible copies and completion notifications.
class MachineSink {
public:
    std::array<uint32_t,256> bp{};
    std::array<bool,256> bp_seen{};
    void memory_boundary() {}
};
#else
#include "packet_renderer.inc"
#endif
std::unique_ptr<MachineSink> sink;
void flush_geometry() {
#if GCNLLR_RETAIL_PIPELINE
    gcnllr_aurora_retail_finish();
#else
    sink->core.flush_frame();
#endif
}
bool flush_commands() {
#if GCNLLR_RETAIL_PIPELINE
    return frontend->flush();
#else
    return frontend->flush(sink.get());
#endif
}
struct MemoryEpoch {uint32_t address,size;uint64_t epoch;bool valid;};
std::vector<MemoryEpoch> memory_epochs;
bool observe_pending_memory(uint32_t address,uint32_t size) {
    if(!config.pending_read || !config.pending_read(config.user,address,size))return true;
    bool ready=false;
    machine_effect([&]{ready=config.prepare_read && config.prepare_read(config.user,address,size);});
    // Cached host pointers remain valid addresses, but their bytes may just
    // have changed. Observe before an epoch hit or actual-content hash too.
    memory_epochs.clear();sink->memory_boundary();
    return ready;
}
bool memory_epoch(uint32_t address,uint32_t size,uint64_t* epoch) {
    if(!observe_pending_memory(address,size))throw std::runtime_error("GPU memory observation failed");
    // The machine tracker already versions whole pages. Share the same query
    // for texture subranges on those pages during this synchronous entry.
    // A writer still ends the entry or explicitly clears this memo below.
    GcnllrNativeGxMemory memory{};
    if(size && gcnllr::gx_memory(config,address,size,memory)) {
        address=memory.physical_address;
        const uint32_t first=address&~4095u;
        const uint64_t end=(uint64_t(address)+size+4095u)&~uint64_t(4095u);
        if(end<=uint64_t(UINT32_MAX)+1u) {address=first;size=uint32_t(end-first);}
    }
    for (const auto& entry:memory_epochs) if (entry.address==address && entry.size==size) {
        ++state.epoch_cached;*epoch=entry.epoch;return entry.valid;
    }
    uint64_t value{};++state.epoch_queries;
    const bool valid=config.memory_epoch && config.memory_epoch(config.user,address,size,&value);
    if (memory_epochs.size()<64) memory_epochs.push_back({address,size,value,valid});
    *epoch=value;return valid;
}
DolGuestAddressResolver resolver{};
bool texture_epoch(uint32_t address,uint32_t size,uint64_t* epoch) {
    if(!config.texture_epoch)return memory_epoch(address,size,epoch);
    if(!observe_pending_memory(address,size))throw std::runtime_error("GPU texture observation failed");
    // Flush/device generations can change while the native CPU runs. Do not
    // carry a stopped-writer memo across repeated bindings in a worker batch.
    ++state.epoch_queries;
    return config.texture_epoch(config.user,address,size,epoch)!=0;
}
bool initialized{}, frame_open{};
gcnllr::NativeScene native_scene;
#if GC_WII_NATIVE_J3D
gcnllr::NativeJ3D native_j3d;
#endif
std::array<uint8_t,64> fifo_tail{};
std::vector<uint8_t> xfb;
Clock::time_point last_events{};
SDL_Gamepad* gamepad{};
FILE* frame_times{};
std::atomic<uint64_t> completion_service_ns{};
uint64_t last_completion_service_ns{};
std::array<char,4u*1024u*1024u> frame_times_buffer;
bool profile{};
bool gpu_trace{};
// Diagnostic-only, bounded distinct layouts; FIFO translation owns the set.
// This identifies why actual copied textures still require materialization.
std::array<std::array<uint32_t,10>,128> incompatible_views{};
size_t incompatible_view_count{};
struct Cost {uint64_t calls{},wall_ns{},cpu_cycles{};};
Cost fifo_cost,draw_cost,copy_cost,present_cost,status_cost,owner_effect_cost;
Clock::time_point last_profile{};
struct Measure {
    Cost& cost;Clock::time_point start;uint64_t cycles{};
    explicit Measure(Cost& value):cost(value) {
        if (!profile) return;
        start=Clock::now();
#ifdef _WIN32
        QueryThreadCycleTime(GetCurrentThread(),&cycles);
#endif
    }
    ~Measure() {
        if (!profile) return;
        ++cost.calls;cost.wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
#ifdef _WIN32
        uint64_t end{};QueryThreadCycleTime(GetCurrentThread(),&end);cost.cpu_cycles+=end-cycles;
#endif
    }
};
void machine_effect(std::function<void()> effect) {
    Measure measurement(owner_effect_cost);
    if(worker_recording && worker_recording->owns_lock())
        fifo_worker->on_owner(std::move(effect),*worker_recording);
    else if(worker_recording)fifo_worker->on_owner(std::move(effect));
    else effect();
}
void profile_report(bool force=false) {
    if(!profile || (!force && Clock::now()-last_profile<std::chrono::seconds(5)))return;
    last_profile=Clock::now();
    const Cost* costs[]={&fifo_cost,&draw_cost,&copy_cost,&present_cost,&status_cost,&owner_effect_cost};
    const char* names[]={"fifo-inclusive","draw","copy","present","status","owner-effect"};
    std::fprintf(stderr,"[native-gx-costs] time_ns=%llu",(unsigned long long)
        std::chrono::duration_cast<std::chrono::nanoseconds>(last_profile.time_since_epoch()).count());
    for(unsigned i=0;i<6;i++)std::fprintf(stderr," %s=%llu/%.3f",names[i],
        (unsigned long long)costs[i]->calls,double(costs[i]->wall_ns)/1e6);
    std::fprintf(stderr,"\n");
}

void fail(const char* message) {
    std::fprintf(stderr,"gcnllr native GX: %s\n",message);
    failed.store(true);
}
void log_message(AuroraLogLevel level,const char* module,const char* message,unsigned length) {
    if (level==LOG_INFO && !std::strcmp(module,"aurora::gfx") && length>=17 &&
        !std::strncmp(message,"Segmenting frame ",17)) return;
    std::fprintf(stderr,"[native-gx:%s] %.*s\n",module,int(length),message);
    if (level==LOG_FATAL) failed.store(true);
}
void place_playtest_window(SDL_Window* window) {
#ifdef _WIN32
    // Use the decorated frame and the selected monitor's work area. A taskbar
    // can occupy any edge; desktop (0,0) is not a valid launch policy.
    const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    RECT frame{};MONITORINFO monitor{sizeof(MONITORINFO)};
    if(!hwnd || !GetWindowRect(hwnd,&frame) ||
       !GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))return;
    constexpr LONG margin=12;
    const LONG left=monitor.rcWork.left+margin,top=monitor.rcWork.top+margin;
    const LONG right=std::max(left,monitor.rcWork.right-margin-(frame.right-frame.left));
    const LONG bottom=std::max(top,monitor.rcWork.bottom-margin-(frame.bottom-frame.top));
    const LONG x=std::clamp(frame.left,left,right),y=std::clamp(frame.top,top,bottom);
    if(x!=frame.left || y!=frame.top)
        SetWindowPos(hwnd,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
#else
    // Let the window manager place a normal decorated window on other hosts.
    (void)window;
#endif
}
bool resolve(void*,uint32_t address,uint32_t size,DolGuestAddressSpace space,
             DolGuestResourceKind resource,DolGuestResolvedRange* out) {
    GcnllrNativeGxMemory memory{};
    if(!gcnllr::gx_memory(config,address,size,memory))return false;
    // Texture/copy-destination resolution supplies only pointer and layout.
    // Actual RAM consumers still observe in the texture decoder, while a
    // qualified live GPU producer can be sampled without downloading it.
    if(resource!=DOL_GUEST_RESOURCE_TEXTURE && resource!=DOL_GUEST_RESOURCE_COPY_DESTINATION &&
       !observe_pending_memory(memory.physical_address,size))return false;
    *out={memory.bytes,address,size,memory.available,space,resource};
    return true;
}
void pump(bool force=false) {
    if (!initialized || (!force && Clock::now()-last_events<std::chrono::milliseconds(4))) return;
    last_events=Clock::now();
    for (const auto* event=aurora_update();event && event->type!=AURORA_NONE;++event)
        if (event->type==AURORA_EXIT) state.quit=1;
    auto& pad=state.pad;
    state.pointer={};
    if (config.hidden) {pad={};pad.stick_x=pad.stick_y=pad.substick_x=pad.substick_y=128;return;}
    auto* window=aurora::window::get_sdl_window();
    if(window && SDL_GetMouseFocus()==window && SDL_GetKeyboardFocus()==window) {
        float x,y;const auto buttons=SDL_GetMouseState(&x,&y);
        const auto size=aurora::window::get_window_size();
        state.pointer=gcnllr::image_pointer(x,y,
            ((buttons&SDL_BUTTON_LMASK)?1u:0u)|((buttons&SDL_BUTTON_RMASK)?2u:0u)|
            ((buttons&SDL_BUTTON_MMASK)?4u:0u),size.width,size.height,
            size.native_fb_width,size.native_fb_height,size.fb_width,size.fb_height);
    }
    const bool* keys=SDL_GetKeyboardState(nullptr);
    pad={}; pad.stick_x=uint8_t(128+100*(int(keys[SDL_SCANCODE_RIGHT])-int(keys[SDL_SCANCODE_LEFT])));
    pad.stick_y=uint8_t(128+100*(int(keys[SDL_SCANCODE_UP])-int(keys[SDL_SCANCODE_DOWN])));
    pad.substick_x=uint8_t(128+100*(int(keys[SDL_SCANCODE_L])-int(keys[SDL_SCANCODE_J])));
    pad.substick_y=uint8_t(128+100*(int(keys[SDL_SCANCODE_I])-int(keys[SDL_SCANCODE_K])));
    if (keys[SDL_SCANCODE_X]) pad.buttons|=0x100;
    if (keys[SDL_SCANCODE_Z]) pad.buttons|=0x200;
    if (keys[SDL_SCANCODE_C]) pad.buttons|=0x400;
    if (keys[SDL_SCANCODE_V]) pad.buttons|=0x800;
    if (keys[SDL_SCANCODE_RETURN]) pad.buttons|=0x1000;
    if (keys[SDL_SCANCODE_A]) pad.buttons|=0x10; // Z
    if (keys[SDL_SCANCODE_S]) { pad.buttons|=0x40; pad.trigger_l=255; }
    if (keys[SDL_SCANCODE_D]) { pad.buttons|=0x20; pad.trigger_r=255; }
    if (keys[SDL_SCANCODE_KP_4]) pad.buttons|=1;
    if (keys[SDL_SCANCODE_KP_6]) pad.buttons|=2;
    if (keys[SDL_SCANCODE_KP_2]) pad.buttons|=4;
    if (keys[SDL_SCANCODE_KP_8]) pad.buttons|=8;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=nullptr; }
    if (!gamepad) {
        int count=0; auto* ids=SDL_GetGamepads(&count);
        if (ids && count) gamepad=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (gamepad) {
        auto axis=[&](SDL_GamepadAxis a,bool invert=false) {
            int value=SDL_GetGamepadAxis(gamepad,a);
            if (value>-4096 && value<4096) return uint8_t(128);
            if (invert) value=-value;
            return uint8_t(std::clamp(128+value/256,0,255));
        };
        auto pressed=[&](SDL_GamepadButton b){return SDL_GetGamepadButton(gamepad,b);};
        auto merge_axis=[&](uint8_t& output,SDL_GamepadAxis a,bool invert=false) {
            const auto value=axis(a,invert);if (value!=128) output=value;
        };
        merge_axis(pad.stick_x,SDL_GAMEPAD_AXIS_LEFTX);merge_axis(pad.stick_y,SDL_GAMEPAD_AXIS_LEFTY,true);
        merge_axis(pad.substick_x,SDL_GAMEPAD_AXIS_RIGHTX);merge_axis(pad.substick_y,SDL_GAMEPAD_AXIS_RIGHTY,true);
        if (pressed(SDL_GAMEPAD_BUTTON_SOUTH)) pad.buttons|=0x100;
        if (pressed(SDL_GAMEPAD_BUTTON_EAST)) pad.buttons|=0x200;
        if (pressed(SDL_GAMEPAD_BUTTON_WEST)) pad.buttons|=0x400;
        if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) pad.buttons|=0x800;
        if (pressed(SDL_GAMEPAD_BUTTON_START)) pad.buttons|=0x1000;
        if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pad.buttons|=0x10;
        if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pad.buttons|=0x40;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) pad.buttons|=1;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) pad.buttons|=2;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) pad.buttons|=4;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_UP)) pad.buttons|=8;
        pad.trigger_l=std::max(pad.trigger_l,uint8_t(std::max(0,int(SDL_GetGamepadAxis(gamepad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)))/128));
        pad.trigger_r=std::max(pad.trigger_r,uint8_t(std::max(0,int(SDL_GetGamepadAxis(gamepad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)))/128));
        if (pad.trigger_l>=230) pad.buttons|=0x40;
        if (pad.trigger_r>=230) pad.buttons|=0x20;
    }
}
#if !GCNLLR_RETAIL_PIPELINE
#include "packet_draw.inc"
#endif
struct PendingDisplay {uint32_t destination,width,height,stride;float yscale;};
std::vector<PendingDisplay> pending_displays;
#if GCNLLR_ASYNC_COPIES
struct StreamedBytes {
    aurora::gfx::AsyncReadback ticket;
    aurora::gfx::AsyncReadback completion;
    gcnllr::gpu_bridge::EncodedImageHandle image;
    aurora::gfx::gxcore::OwnedTexture texture;
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    std::exception_ptr error;
    bool completed{};
    const std::vector<uint8_t>& materialize() {
        std::lock_guard lock(mutex);
        if(!completed) {
            try {
                // Composed generations remain GPU-only until an actual
                // memory observer demands their immutable bytes. The source
                // is recorded before ownership publication. The batched
                // owner-thread provider submits it here before observation;
                // no call back to a waiting FIFO worker is required.
                if(!ticket)ticket=image ? gcnllr::gpu_bridge::queue_display_readback(image) :
                    aurora::gfx::queue_texture_readback(texture.texture);
                if(!aurora::gfx::async_readback_ready(ticket))aurora::gfx::poll_async_readbacks(true);
                gcnllr::gpu_bridge::validate_submission();
                bytes=aurora::gfx::take_async_readback(ticket);
                if(texture.texture)bytes=gcnllr::encode_native_copy(texture.format,texture.width,texture.height,
                    bytes,texture.texture->size.width,texture.texture->size.height);
            }catch(...){error=std::current_exception();}
            completed=true;
        }
        if(error)std::rethrow_exception(error);
        return bytes;
    }
};
gcnllr::OrderedEffects streamed_effects;
std::atomic<size_t> streamed_pending{};
size_t streamed_readbacks{};
size_t streamed_guest_effects{};
uint64_t streamed_retired{},streamed_polls{},streamed_barriers{};
void queue_stream(uint64_t token,uint32_t address,const std::shared_ptr<StreamedBytes>& stream) {
    const auto completion=stream->completion ? stream->completion:stream->ticket;
    // Recorded immutable generations have already captured their inputs. Queue
    // order supplies GPU consumers; RAM/EFB observers still materialize and wait.
    // Eager transfers retain the physical completion provider in either build.
    const bool owned=GCNLLR_OWNED_COMPLETION && GCNLLR_COMPOSED_SCANOUT &&
        !stream->ticket && (stream->image || stream->texture.texture);
    streamed_effects.push([completion,owned]{return owned || aurora::gfx::async_readback_ready(completion);},[token,address,stream,completion,owned] {
        // Preserve physical-ticket errors for eager/physical providers and
        // terminal submission errors even after the guest overwrites a token.
        // Owned completion does not claim the host GPU has finished executing.
        if(!owned)aurora::gfx::validate_async_readback(completion);
        gcnllr::gpu_bridge::validate_submission();
#if !GCNLLR_COMPOSED_SCANOUT
        machine_effect([token]{
            if(!config.retire_copy(config.user,token))throw std::runtime_error("Machine rejected ordered GPU completion");
        });
        // GX cache state is FIFO-thread-owned. Never update it from an early
        // CPU materialization racing translation. The owner handoff above
        // completes RAM publication before this optional matching-image hash.
        {std::lock_guard lock(stream->mutex);
         if(stream->completed && !stream->error && stream->texture.texture)
             aurora::gfx::gxcore::publish_owned_copy_memory(stream->texture,address,stream->bytes);}
#endif
        // GPU completion need not publish MEM1 in the composed group. Keep
        // the immutable generation for actual RAM observers. Completion
        // has no dependency on a transfer or mapping in this provider group.
        // A matching-RAM texture hash cannot be published before those stores.
        --streamed_readbacks;++streamed_retired;
        streamed_pending.fetch_sub(1,std::memory_order_release);
    });
    ++streamed_readbacks;streamed_pending.fetch_add(1,std::memory_order_release);
}
#endif
void retire_streams(bool wait) {
#if GCNLLR_ASYNC_COPIES
    if(!streamed_effects.size())return;
    const auto start=Clock::now();
    gcnllr::gpu_bridge::submit_copies();
    // A retained GPU generation permits asynchronous GPU execution, but not
    // successful PE retirement ahead of a failed host encoding operation.
    if(GCNLLR_OWNED_COMPLETION)gcnllr::gpu_bridge::acknowledge_submission();
    if(wait)++streamed_barriers;else ++streamed_polls;
    streamed_effects.drain(wait,[](bool block){aurora::gfx::poll_async_readbacks(block);});
    memory_epochs.clear();sink->memory_boundary();
    completion_service_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(),std::memory_order_relaxed);
#else
    (void)wait;
#endif
}
void service_streams() {
#if GCNLLR_COMPOSED_SCANOUT
    // Publication already submitted an immutable resource. CPU/DMA readers
    // can obtain its bytes independently, and native VI uses GPU queue order.
    // A small FIFO/device quantum is therefore not itself a host GPU fence.
    // Join only when an earned PE notification needs ordered retirement.
    // Explicit drain, shutdown and bounded-copy backpressure still join all.
    if(streamed_guest_effects)retire_streams(true);
#else
    retire_streams(true);
#endif
}
void ordered_bp_effect(uint32_t reg,uint32_t value) {
#if GCNLLR_ASYNC_COPIES
    streamed_effects.push({},[reg,value]{
        machine_effect([reg,value]{config.bp_write(config.user,reg,value);});
        --streamed_guest_effects;
        streamed_pending.fetch_sub(1,std::memory_order_release);
    });
    ++streamed_guest_effects;
    streamed_pending.fetch_add(1,std::memory_order_release);
#else
    machine_effect([reg,value]{config.bp_write(config.user,reg,value);});
#endif
}
struct RetainedCopy {
    GcnllrNativeGxCopy copy;
    gcnllr::gpu_bridge::EncodedImageHandle image;
    aurora::gfx::gxcore::OwnedTexture texture;
#if GCNLLR_ASYNC_COPIES
    std::shared_ptr<StreamedBytes> stream;
#endif
};
std::mutex retained_mutex;
std::unordered_map<uint64_t,RetainedCopy> retained_copies;
std::unordered_map<uint32_t,uint64_t> texture_producers;
uint64_t next_copy_token;
RetainedCopy retained_copy(uint64_t token) {
    std::lock_guard lock(retained_mutex);
    const auto found=retained_copies.find(token);
    return found==retained_copies.end()?RetainedCopy{}:found->second;
}
void release_copy(uint64_t token) {
    std::lock_guard lock(retained_mutex);
    const auto found=retained_copies.find(token);
    if(found==retained_copies.end())return;
    const auto current=texture_producers.find(found->second.copy.address);
    if(current!=texture_producers.end() && current->second==token)texture_producers.erase(current);
    retained_copies.erase(found);
}
std::shared_ptr<aurora::gfx::TextureRef> owned_texture(uint32_t address,uint32_t size,
    uint32_t format,uint32_t width,uint32_t height) {
    const uint32_t offset=address&0x1fffffffu;
    std::lock_guard lock(retained_mutex);
    const auto current=texture_producers.find(offset);
    if(current==texture_producers.end())return {};
    const auto found=retained_copies.find(current->second);
    if(found==retained_copies.end())return {};
    const auto& retained=found->second;
    const auto& c=retained.copy;const auto& t=retained.texture;
    // Qualified views preserve this converter's exact packed-byte decode:
    // I4 keeps both nibbles, while Z16's [Zmid,Zhi] bytes decode as IA8
    // (RGB=Zhi, alpha=Zmid). Other reinterpretations use the RAM decoder.
    // Dense addressing cannot stand in for stride gaps or interior bases.
    const bool compatible_format=
        ((format==0u || format==1u || format==4u || format==6u) && t.format==format) ||
        (format==3u && t.format==0x13u);
    if(compatible_format && t.texture &&
       t.width==width && t.height==height && c.stride==c.row_bytes &&
       uint64_t(c.row_bytes)*c.rows==size) {
        resident_texture_hits.fetch_add(1,std::memory_order_relaxed);
        return t.texture;
    }
    resident_texture_incompatible.fetch_add(1,std::memory_order_relaxed);
    if(profile) {
        const std::array<uint32_t,10> key{format,width,height,size,t.format,t.width,t.height,c.row_bytes,c.rows,c.stride};
        const auto end=incompatible_views.begin()+incompatible_view_count;
        if(incompatible_view_count<incompatible_views.size() && std::find(incompatible_views.begin(),end,key)==end) {
            incompatible_views[incompatible_view_count++]=key;
            std::fprintf(stderr,"[native-owned-incompatible] request=%u/%ux%u/%u copy=%u/%ux%u rows=%u/%u/%u texture=%u\n",
                format,width,height,size,t.format,t.width,t.height,c.row_bytes,c.rows,c.stride,unsigned(bool(t.texture)));
        }
    }
    return {};
}
int materialize_copy(uint64_t token) {
    if(!initialized || failed.load())return 0;
    const auto retained=retained_copy(token);
    if(!retained.image && !retained.texture.texture)return 0;
    const auto begin=Clock::now();
    try {
        const auto& c=retained.copy;
        const bool display=(c.flags&GCNLLR_NATIVE_GX_COPY_DISPLAY)!=0;
        std::vector<uint8_t> bytes;
#if GCNLLR_ASYNC_COPIES
        if(retained.stream)bytes=retained.stream->materialize();
        else
#endif
        if(display) {
            const auto image=gcnllr::gpu_bridge::materialize_display_copy(retained.image);
            if(image.width*2!=c.row_bytes || image.height!=c.rows)
                throw std::runtime_error("Resident XFB geometry changed");
            bytes=image.yuy2;
        } else bytes=aurora::gfx::gxcore::materialize_owned_texture(retained.texture);
        if(bytes.size()!=size_t(c.row_bytes)*c.rows)
            throw std::runtime_error("Resident copy byte geometry changed");
        // Called by the machine owner. No FIFO wait or owner round-trip is
        // allowed here: a publisher can be waiting for this materialization.
        const uint32_t flags=c.flags|GCNLLR_NATIVE_GX_COPY_MATERIALIZING;
        if(c.stride==c.row_bytes) {
            if(!config.write_copy(config.user,c.address,bytes.data(),uint32_t(bytes.size()),flags,
                    display ? c.row_bytes/2:0,display ? c.rows:0,display ? c.stride:0))
                throw std::runtime_error("Machine rejected observed GPU bytes");
        } else {
            for(uint32_t row=0;row<c.rows;row++)
                if(!config.write_copy(config.user,c.address+row*c.stride,
                        bytes.data()+size_t(row)*c.row_bytes,c.row_bytes,flags,
                        display ? c.row_bytes/2:0,display ? 1:0,display ? c.row_bytes:0))
                    throw std::runtime_error("Machine rejected observed GPU row");
        }
        ++state.resident_materializations;
        if(!display)resident_texture_materializations.fetch_add(1,std::memory_order_relaxed);
        readback_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count(),std::memory_order_relaxed);
        return 1;
    } catch(const std::exception& e) {fail(e.what());return 0;}
}
void display_readback(const gxruntime::gxcore::EfbCopyCommand& copy,uint32_t destination,
                      uint32_t stride,float yscale,const gcnllr::gpu_bridge::DisplayCopyFilter& filter) {
    if (pending_displays.size()>=16) flush_display_copies();
    const uint32_t h=uint32_t(float(copy.height)*yscale),w=copy.width;
    if (!w || !h || (w&1) || w>640 || h>576 || stride<w*2 || stride>2048)
        throw std::runtime_error("Invalid guest XFB dimensions/stride");
    // Snapshot the live BP filter/clamp state with this immutable copy.
    // A programmed (or reset) all-zero filter is black, not identity.
    gcnllr::gpu_bridge::record_display_copy(copy,filter);
    pending_displays.push_back({destination,w,h,stride,yscale});
    ++state.display_copies;
}
void display_readback(const gxruntime::gxcore::EfbCopyCommand& copy) {
    const auto& bp=sink->bp;
    const uint32_t scale=bp[0x4E]&0xFFFFu;
    const bool invert=(bp[0x52]&(1u<<10))!=0;
    const float yscale=invert ? (scale ? 256.f/float(scale) : 1.f) : float(scale)/256.f;
    display_readback(copy,(bp[0x4B]&guest_gpu_address_mask)<<5,(bp[0x4D]&0x3FFu)<<5,yscale,
        {bp[0x53],bp[0x54],(bp[0x52]&1u)!=0,(bp[0x52]&2u)!=0});
}
void flush_display_copies() {
    if (pending_displays.empty()) return;
    // Native row scaling remains the eager provider until a resident encoded
    // layout implements it. This is a format capability, not a title check.
    if(config.own_copy && std::all_of(pending_displays.begin(),pending_displays.end(),
            [](const PendingDisplay& copy){return copy.yscale==1.f;})) {
        auto images=gcnllr::gpu_bridge::retain_display_copies();
        if(images.size()!=pending_displays.size())throw std::runtime_error("Resident copy fence mismatch");
        for(size_t i=0;i<images.size();i++) {
            const auto& copy=pending_displays[i];
#if GCNLLR_ASYNC_COPIES
            if(streamed_readbacks>=16)retire_streams(true);
            auto stream=std::make_shared<StreamedBytes>();
#if GCNLLR_COMPOSED_SCANOUT
            stream->image=images[i];
            if(!GCNLLR_OWNED_COMPLETION)stream->completion=aurora::gfx::queue_gpu_completion();
#else
            stream->ticket=gcnllr::gpu_bridge::queue_display_readback(images[i]);
#endif
#endif
            if(next_copy_token==UINT64_MAX)throw std::runtime_error("GPU copy identity exhausted");
            const GcnllrNativeGxCopy descriptor{++next_copy_token,copy.destination,copy.width*2,copy.height,copy.stride,GCNLLR_NATIVE_GX_COPY_DISPLAY};
            {
                std::lock_guard lock(retained_mutex);
                retained_copies.emplace(descriptor.token,RetainedCopy{descriptor,std::move(images[i]),{}});
#if GCNLLR_ASYNC_COPIES
                retained_copies.at(descriptor.token).stream=stream;
#endif
            }
            try {
                machine_effect([&]{
                    if(!config.own_copy(config.user,&descriptor))
                        throw std::runtime_error("Machine rejected GPU copy ownership");
                });
            } catch(...) {release_copy(descriptor.token);throw;}
#if GCNLLR_ASYNC_COPIES
            queue_stream(descriptor.token,descriptor.address,stream);
#endif
            ++state.resident_copies;
        }
        pending_displays.clear();memory_epochs.clear();sink->memory_boundary();
        return;
    }
    retire_streams(true); // Unsupported resident row scaling uses eager floor.
    const auto start=Clock::now();
    auto images=gcnllr::gpu_bridge::take_display_copies();
    readback_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(),std::memory_order_relaxed);
    if (images.size()!=pending_displays.size()) throw std::runtime_error("GPU display-copy fence mismatch");
    for (size_t i=0;i<images.size();i++) {
        const auto& image=images[i];const auto& copy=pending_displays[i];
        const auto w=copy.width,h=copy.height,stride=copy.stride;
        if (image.width!=w || !image.height || image.yuy2.size()!=size_t(w)*image.height*2)
            throw std::runtime_error("GPU display-copy byte geometry mismatch");
        xfb.resize(size_t(stride)*h);
        for (uint32_t y=0;y<h;y++) {
          const size_t sy=std::min(size_t(image.height-1),size_t(float(y)/copy.yscale+0.5f));
          std::memcpy(xfb.data()+size_t(y)*stride,image.yuy2.data()+sy*w*2,w*2);
        }
        machine_effect([&]{
            if (!config.write_copy(config.user,copy.destination,xfb.data(),uint32_t(xfb.size()),1,w,h,stride))
                throw std::runtime_error("Machine rejected completed GPU XFB write");
        });
        memory_epochs.clear();
        sink->memory_boundary();
    }
    pending_displays.clear();
}
void copy(const gxruntime::gxcore::EfbCopyCommand& command,void*) {
    Measure measurement(copy_cost);
    if (command.clear) ++efb_generation;
    if (command.format==0xFu) { display_readback(command); return; }
    flush_display_copies();
    const auto tile=gcnllr::native_copy_layout(command.format);
    if(!tile.bytes || !command.destination_width || !command.destination_height)
        throw std::runtime_error("Native texture-copy format/size is unsupported");
    const uint32_t row_bytes=((command.destination_width+tile.width-1)/tile.width)*tile.bytes;
    const uint32_t rows=(command.destination_height+tile.height-1)/tile.height;
    const uint32_t stride=(sink->bp[0x4D]&0x3FFu)<<5;
    const uint64_t extent=uint64_t(rows-1)*stride+row_bytes;
    GcnllrNativeGxMemory destination{};
    if (!stride || extent>UINT32_MAX ||
        !gcnllr::gx_memory(config,command.dest_address,uint32_t(extent),destination))
        throw std::runtime_error("Invalid native texture-copy memory layout");
    if(config.own_copy && stride>=row_bytes) {
#if GCNLLR_ASYNC_COPIES
        if(streamed_readbacks>=16)retire_streams(true);
#endif
        auto texture=aurora::gfx::gxcore::capture_owned_efb_copy(command);
#if GCNLLR_ASYNC_COPIES
        auto stream=std::make_shared<StreamedBytes>();stream->texture=texture;
#if GCNLLR_COMPOSED_SCANOUT
        if(!GCNLLR_OWNED_COMPLETION)stream->completion=aurora::gfx::queue_gpu_completion();
#else
        stream->ticket=aurora::gfx::queue_texture_readback(texture.texture);
#endif
#endif
        if(next_copy_token==UINT64_MAX)throw std::runtime_error("GPU copy identity exhausted");
        const GcnllrNativeGxCopy descriptor{++next_copy_token,command.dest_address,row_bytes,rows,stride,0};
        {
            std::lock_guard lock(retained_mutex);
            retained_copies.emplace(descriptor.token,RetainedCopy{descriptor,{},std::move(texture)});
            texture_producers[descriptor.address]=descriptor.token;
#if GCNLLR_ASYNC_COPIES
            retained_copies.at(descriptor.token).stream=stream;
#endif
        }
        try {
            machine_effect([&]{
                if(!config.own_copy(config.user,&descriptor))
                    throw std::runtime_error("Machine rejected GPU texture ownership");
            });
        } catch(...) {release_copy(descriptor.token);throw;}
#if GCNLLR_ASYNC_COPIES
        queue_stream(descriptor.token,descriptor.address,stream);
#endif
        resident_textures.fetch_add(1,std::memory_order_relaxed);
        ++state.texture_copies;
        memory_epochs.clear();sink->memory_boundary();
        return;
    }
    retire_streams(true);
    aurora::gfx::gxcore::copy_efb_to_texture(command);
    const auto bytes=aurora::gfx::gxcore::read_efb_copy(command);
    if (bytes.size()!=size_t(row_bytes)*rows)
        throw std::runtime_error("Native texture-copy CPU materialization is unsupported");
    machine_effect([&]{
        for (uint32_t row=0;row<rows;row++)
            if (!config.write_copy(config.user,command.dest_address+row*stride,bytes.data()+size_t(row)*row_bytes,
                                   row_bytes,0,0,0,0))
                throw std::runtime_error("Machine rejected GPU texture-copy write");
    });
    if(stride==row_bytes)
        aurora::gfx::gxcore::publish_efb_copy_memory(command.dest_address,bytes.data(),uint32_t(bytes.size()),
            command.destination_width,command.destination_height,command.format);
    memory_epochs.clear();
    sink->memory_boundary();
    ++state.texture_copies;
}
void stop() {
    if(fifo_worker) {
        try {fifo_worker->wait(true);}catch(const std::exception& e){fail(e.what());}
        fifo_worker->stop();
        const auto stats=fifo_worker->stats();
        std::fprintf(stderr,"[native-gx-worker] batches=%llu bytes=%llu barriers=%llu owner_calls=%llu cpu_us=%llu\n",
            (unsigned long long)stats.batches,(unsigned long long)stats.bytes,
            (unsigned long long)stats.barriers,(unsigned long long)stats.owner_calls,
            (unsigned long long)aurora::gfx::thread_cpu::cpu_us(aurora::gfx::thread_cpu::Role::GxWorker));
        fifo_worker.reset();
    }
    if (initialized) {try {flush_display_copies();retire_streams(true);} catch (const std::exception& e) {fail(e.what());}}
    if (initialized && profile) {
        profile_report(true);
        const Cost* costs[]={&fifo_cost,&draw_cost,&copy_cost,&present_cost,&status_cost,&owner_effect_cost};
        const char* names[]={"fifo-inclusive","draw","copy","present","status","owner-effect"};
        for (unsigned i=0;i<6;i++) std::fprintf(stderr,"[native-gx-cost] %s calls=%llu wall_ms=%.3f cpu_cycles=%llu\n",
            names[i],(unsigned long long)costs[i]->calls,double(costs[i]->wall_ns)/1e6,
            (unsigned long long)costs[i]->cpu_cycles);
    }
    if (initialized) std::fprintf(stderr,"[native-gx-summary] draws=%llu copies=%llu presents=%llu readback_ms=%.3f\n",
        (unsigned long long)state.draws,(unsigned long long)state.display_copies,
        (unsigned long long)state.presents,double(readback_ns.load(std::memory_order_relaxed))/1e6);
    if (initialized && frontend) std::fprintf(stderr,"[native-gx-fifo] bytes=%llu pending=%zu\n",
        (unsigned long long)state.fifo_bytes,frontend->pending_fifo_size());
    if (initialized && std::getenv("GCNLLR_GPU_TRACE")) {
        std::fprintf(stderr,"[native-gx-fifo-tail] ");
        for(auto b:fifo_tail)std::fprintf(stderr,"%02X",b);
        std::fprintf(stderr,"\n");
    }
    const auto& cache=aurora::gfx::gxcore::texture_cache_stats();
    if (initialized) std::fprintf(stderr,"[native-gx-cache] generation_hits=%llu hashes=%llu epoch_queries=%llu epoch_cached=%llu\n",
        cache.generation_hits,cache.hashed_lookups,(unsigned long long)state.epoch_queries,(unsigned long long)state.epoch_cached);
    uint64_t copy_hits{},copy_misses{};aurora::gfx::gxcore::efb_copy_memory_stats(&copy_hits,&copy_misses);
    if(initialized)std::fprintf(stderr,"[native-gx-copy-cache] verified_hits=%llu invalidations=%llu\n",
        (unsigned long long)copy_hits,(unsigned long long)copy_misses);
#if !GCNLLR_RETAIL_PIPELINE
    if (initialized && sink) std::fprintf(stderr,"[native-gx-state] duplicate_draw_states=%llu duplicate_textures=%llu cache_placement_states=%llu\n",
        (unsigned long long)sink->duplicate_draw_states,(unsigned long long)sink->duplicate_textures,
        (unsigned long long)sink->cache_placement_states);
#endif
#if GCNLLR_ASYNC_COPIES
    std::fprintf(stderr,"[native-streamed-copies] retired=%llu polls=%llu barriers=%llu pending=%zu\n",
        (unsigned long long)streamed_retired,(unsigned long long)streamed_polls,
        (unsigned long long)streamed_barriers,streamed_effects.size());
    if(streamed_effects.size())fail("GPU stopped with unresolved ordered effects");
    streamed_effects={};streamed_pending.store(0);streamed_readbacks=0;streamed_guest_effects=0;
#endif
    if (initialized && frame_open) {
        try {gcnllr::gpu_bridge::finish();}
        catch(const std::exception& error) {fail(error.what());}
        catch(...) {fail("Native GPU completion failed during shutdown");}
        frame_open=false;
    }
    {
        std::lock_guard lock(retained_mutex);
        if(!retained_copies.empty())fail("GPU stopped with unresolved memory ownership");
        texture_producers.clear();retained_copies.clear();
    }
    aurora::gfx::gxcore::set_owned_texture_lookup(nullptr);
    native_scene.reset();
#if GC_WII_NATIVE_J3D
    native_j3d.reset();
#endif
    frontend.reset(); sink.reset();
#if GCNLLR_RETAIL_PIPELINE
    std::fprintf(stderr,"[native-retail-textures] queries=%llu reused=%llu\n",
        (unsigned long long)retail_texture_ids.queries,(unsigned long long)retail_texture_ids.reused);
    retail_active=false;retail_texture_ids.clear();aurora::gx::recomp::clear_guest_address_resolver();
#endif
    if (gamepad) { SDL_CloseGamepad(gamepad);gamepad=nullptr; }
    if (initialized) { gcnllr::gpu_bridge::release();aurora_shutdown();initialized=false; }
    if (frame_times) {std::fclose(frame_times);frame_times=nullptr;}
    frame_open=false;
}
int start(const GcnllrNativeGxConfig* input) {
    if (!input || input->abi!=GCNLLR_NATIVE_GX_ABI || initialized ||
        !input->bp_write || !input->write_copy || !input->data_directory || !input->cache_directory) return 0;
    if(bool(input->resolve_memory)!=bool(input->locate_memory))return 0;
    if(!input->resolve_memory && (!input->mem1 || input->mem1_size!=24u*1024u*1024u))return 0;
    const unsigned ownership=unsigned(input->own_copy!=nullptr)+unsigned(input->pending_read!=nullptr)+
        unsigned(input->prepare_read!=nullptr);
    if(ownership!=0 && ownership!=3)return 0;
#if GCNLLR_ASYNC_COPIES
    if(ownership!=3 || !input->retire_copy)return 0;
    streamed_retired=streamed_polls=streamed_barriers=0;
#endif
    config=*input;state={};native_frame_state={};failed.store(false);readback_ns.store(0);next_copy_token=0;
    aurora::gfx::gxcore::reset_resident_geometry_stats();
    resident_textures.store(0);resident_texture_hits.store(0);
    resident_texture_materializations.store(0);resident_texture_incompatible.store(0);
#if GCNLLR_GX_FIFO_WORKER
    if(config.memory_epoch) {fail("Concurrent FIFO worker cannot use stopped-writer memory epochs");return 0;}
#endif
    fifo_tail.fill(0);
    ++efb_generation;color_peek_generation=depth_peek_generation=0;
    color_peek.clear();depth_peek.clear();
    profile=std::getenv("GCNLLR_GX_PROFILE")!=nullptr;
    gpu_trace=std::getenv("GCNLLR_GPU_TRACE")!=nullptr;
    incompatible_view_count=0;
    fifo_cost={};draw_cost={};copy_cost={};present_cost={};status_cost={};owner_effect_cost={};last_profile=Clock::now();
    try {
        SDL_SetMainReady();
        SDL_SetHint("GCNLLR_RENDER_HIDDEN",input->hidden ? "1" : "0");
        SDL_SetHint("GCNLLR_VALIDATE_GPU",std::getenv("GCNLLR_GPU_VALIDATE") ? "1" : "0");
#ifdef _WIN32
        HMODULE module{};char path[32768]{};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const char*>(&config),&module) || !GetModuleFileNameA(module,path,sizeof(path)))
            throw std::runtime_error("Native graphics runtime directory is unavailable");
        std::string runtime_path(path);
        runtime_path.resize(runtime_path.find_last_of("\\/")+1);
        SDL_SetHint("GCNLLR_NATIVE_RUNTIME_DIR",runtime_path.c_str());
        const unsigned system_length=GetSystemDirectoryA(path,sizeof(path));
        if (!system_length || system_length+2>=sizeof(path)) throw std::runtime_error("System runtime directory is unavailable");
        path[system_length]='\\';path[system_length+1]=0;
        SDL_SetHint("GCNLLR_SYSTEM_RUNTIME_DIR",path);
#endif
        AuroraConfig settings{};
        settings.appName=input->title;settings.userPath=input->data_directory;settings.cachePath=input->cache_directory;
#if defined(GCNLLR_GPU_VULKAN)
        settings.desiredBackend=BACKEND_VULKAN;
#elif defined(_WIN32)
        settings.desiredBackend=BACKEND_D3D12;
#elif defined(__APPLE__)
        settings.desiredBackend=BACKEND_METAL;
#else
        settings.desiredBackend=BACKEND_VULKAN;
#endif
        settings.windowWidth=800;settings.windowHeight=600;
        settings.windowPosX=-1;settings.windowPosY=-1;
        settings.vsync=false;settings.startFullscreen=false;settings.pauseOnFocusLost=false;
        settings.allowJoystickBackgroundEvents=true;settings.logLevel=LOG_INFO;settings.logCallback=log_message;
        auto info=aurora_initialize(0,nullptr,&settings);
        initialized=true;
        if (!info.window) throw std::runtime_error("Aurora did not create a native GPU window");
        if (info.backend==BACKEND_NULL) throw std::runtime_error("A real GPU backend is required");
        if (info.backend!=settings.desiredBackend) throw std::runtime_error("Requested native GPU backend is unavailable");
        if (input->hidden) SDL_HideWindow(info.window);
        else place_playtest_window(info.window);
        gcnllr::gpu_bridge::initialize();
        GXInit(nullptr,0);
        GXSetCopyClear(GXColor{0,0,0,255},0xFFFFFFu);
        GXRenderModeObj mode{};mode.viTVmode=VI_TVMODE_NTSC_PROG;
        mode.fbWidth=mode.viWidth=640;mode.efbHeight=mode.xfbHeight=mode.viHeight=480;
        VIConfigure(&mode);
        aurora_set_frame_buffer_scale(1.0f);
        /* The machine owns a fixed physical EFB; resizing the presentation window
         * must not expand its raster grid to the host window's aspect. */
        aurora::window::set_frame_buffer_aspect_fit(true);
        aurora_set_frame_interpolation(false);
        aurora_set_bloom(false);aurora_set_far_blur(false);aurora_set_ambient_occlusion(false);
        /* Consume the host SDK's initialization commands before retail GX
         * owns the renderer. Delaying this until end_frame overwrites the
         * guest's copy-clear with GXInit's grey default after its first copy. */
#if !GCNLLR_RETAIL_PIPELINE
        dol_guest_address_resolver_init_callback(&resolver,resolve,nullptr);
#endif
#if GCNLLR_RETAIL_PIPELINE
        frontend=std::make_unique<RetailGxFrontend>();
#else
        frontend=std::make_unique<RetailGxFrontend>(resolver);
        frontend->set_packet_drain_enabled(true);
#endif
        sink=std::make_unique<MachineSink>();
#if !GCNLLR_RETAIL_PIPELINE
        sink->core.set_guest_resolver(&resolver);sink->core.set_plan_observer(plan,nullptr);sink->core.set_copy_observer(copy,nullptr);
#endif
        aurora::gfx::gxcore::reset_texture_cache();
        memory_epochs.clear();
        aurora::gfx::gxcore::set_texture_dirty_epoch_observer(texture_epoch);
        aurora::gfx::gxcore::set_efb_copy_epoch_observer(memory_epoch);
        aurora::gfx::gxcore::set_owned_texture_lookup(config.own_copy ? owned_texture:nullptr);
        frame_open=aurora_begin_frame();
        if (!frame_open) throw std::runtime_error("Initial native GPU frame failed");
        aurora::gx::fifo::drain();
#if GCNLLR_RETAIL_PIPELINE
        aurora::gx::recomp::set_guest_address_resolver([](void*,uint32_t address,uint32_t size,
            aurora::gx::recomp::AddressSpace space,aurora::gx::recomp::ResourceKind resource,
            const void** data,uint32_t* available) {
            DolGuestResolvedRange range{};
            if(!resolve(nullptr,address,size,static_cast<DolGuestAddressSpace>(space),
                static_cast<DolGuestResourceKind>(resource),&range))return false;
            // Resolving a texture pointer does not consume its RAM bytes.
            // The selected native resolver first considers an exact live GPU
            // view, then observes memory only if conversion really needs it.
            *data=range.data;*available=range.available;return true;
        },nullptr);
        gcnllr_aurora_retail_reset();retail_texture_ids.clear();aurora::gx::clear_static_texture_cache();
        retail_active=true;
        std::fprintf(stderr,"[native-gx-pipeline] provider=aurora-retail packet_frontend=0 gxcore_state_owner=0\n");
#endif
        if (const auto* path=std::getenv("GCNLLR_FRAME_TIMES")) {
            frame_times=std::fopen(path,"wx");
            if (!frame_times) throw std::runtime_error("Could not create new frame-time evidence");
            // Keep diagnostic disk writes out of a normal qualification run.
            // The bounded buffer holds over ten minutes and flushes on close.
            if(std::setvbuf(frame_times,frame_times_buffer.data(),_IOFBF,frame_times_buffer.size()))
                throw std::runtime_error("Could not buffer frame-time evidence");
            completion_service_ns=last_completion_service_ns=0;
            std::fprintf(frame_times,"frame,time_ns,present_call_ns,copy_readback_ns,main_cpu_cycles,completion_service_ns\n");
        }
        pump(true);
        if (!gcnllr::gpu_bridge::settle_framebuffer())
            throw std::runtime_error("Native framebuffer initialization did not settle");
#if GCNLLR_GX_FIFO_WORKER
        fifo_worker=std::make_unique<gcnllr::FifoWorker>([](std::span<const uint8_t> bytes,bool flush){
            static thread_local const bool registered=[] {
                aurora::gfx::thread_cpu::register_current(aurora::gfx::thread_cpu::Role::GxWorker);return true;
            }();
            (void)registered;
            RecordingBatch recording;
            Measure measurement(fifo_cost);
            memory_epochs.clear();sink->memory_boundary();
            // The pinned donor's drain mode emits trace events at command
            // boundaries before its bounded ring fills. Keep the handoff as
            // one stream, including partial-command carry, instead of forcing
            // a parse/erase/packet handoff for every 1 KiB of guest commands.
            if(!bytes.empty() && (!frontend->write_fifo(bytes) || !flush_commands()))
                throw std::runtime_error(frontend->last_error() ? frontend->last_error() : "Native FIFO resource validation failed");
            if(flush) {
                if(!flush_commands())throw std::runtime_error("Native FIFO barrier failed");
                flush_geometry();flush_display_copies();
            }
#if GCNLLR_ASYNC_COPIES
            else {
                // A copy without a later PE finish must still execute. The
                // resolved copy already flushes preceding draw assembly in
                // GXCore. Encode those display copies without breaking draw
                // fusion at every unrelated CPU-to-worker handoff.
                flush_display_copies();
            }
#endif
#if GCNLLR_ASYNC_COPIES
            // Wait once for this completed batch on the worker. The next
            // earned device service joins this work before guest execution
            // continues; host latency must not extend guest PE wait time.
            retire_streams(true);
#else
            retire_streams(flush);
#endif
            if(failed.load())throw std::runtime_error("Native FIFO worker failed");
        });
#endif
        std::fprintf(stderr,"[native-gx] translation=%s vi=%s\n",fifo_worker ? "worker" : "synchronous",
            GCNLLR_GX_COOPERATIVE_VI ? "completed-scanout/recording-lock" : "full-barrier");
        return 1;
    } catch (const std::exception& e) { fail(e.what());stop();return 0; }
}
int fifo(const uint8_t* bytes,uint32_t size) {
    if (!initialized || failed.load() || (!bytes && size)) return 0;
#if GCNLLR_GPU_TRACE_CACHE
    const bool trace=gpu_trace;
#else
    const bool trace=std::getenv("GCNLLR_GPU_TRACE")!=nullptr;
#endif
    if(trace) {
        const auto count=std::min<size_t>(size,fifo_tail.size());
        std::memmove(fifo_tail.data(),fifo_tail.data()+count,fifo_tail.size()-count);
        if(count)std::memcpy(fifo_tail.data()+fifo_tail.size()-count,bytes+size-count,count);
    }
    try {
        if(fifo_worker)fifo_worker->append({bytes,size});
        else {
            Measure measurement(fifo_cost);
            memory_epochs.clear();sink->memory_boundary();
            if (!frontend->write_fifo({bytes,size}) || !flush_commands())
                throw std::runtime_error(frontend->last_error() ? frontend->last_error() : "Native FIFO resource validation failed");
#if GCNLLR_ASYNC_COPIES
            // Owner-thread translation eliminates a condition-variable
            // round-trip for each small emulated GPU quantum. Actual GPU
            // encoding remains on Aurora's render worker. Composed ownership
            // joins for PE effects rather than every small FIFO quantum.
            flush_display_copies();service_streams();
#endif
        }
        state.fifo_bytes+=size;
        return !failed.load();
    } catch (const std::exception& e) { fail(e.what());return 0; }
}
int drain() {
    if (!initialized || failed.load()) return 0;
    try {
        gcnllr::gpu_bridge::validate_submission();
        if(fifo_worker)fifo_worker->wait(true);
        else {
            memory_epochs.clear();sink->memory_boundary();
            if (!flush_commands())
                throw std::runtime_error(frontend->last_error() ? frontend->last_error() : "Native FIFO resource validation failed");
            flush_geometry();flush_display_copies();retire_streams(true);
        }
        return !failed.load();
    } catch (const std::exception& e) { fail(e.what());return 0; }
}
void status(GcnllrNativeGxStatus* output) {
    if (!output) return;
    Measure measurement(status_cost);
    try {
        if(initialized)gcnllr::gpu_bridge::validate_submission();
        if(fifo_worker) {
            if(GCNLLR_GX_COOPERATIVE_VI)fifo_worker->service();
            else fifo_worker->wait();
        }
    }catch(const std::exception& e){fail(e.what());}
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if (initialized) pump();
    const auto& cache=aurora::gfx::gxcore::texture_cache_stats();
    state.texture_generation_hits=cache.generation_hits;state.texture_hashes=cache.hashed_lookups;
    aurora::gfx::gxcore::efb_copy_memory_stats(&state.verified_copy_hits,&state.verified_copy_misses);
    profile_report();
    *output=state;output->failed=failed.load();output->readback_ns=readback_ns.load(std::memory_order_relaxed);
    output->resident_textures=resident_textures.load(std::memory_order_relaxed);
    output->resident_texture_hits=resident_texture_hits.load(std::memory_order_relaxed);
    output->resident_texture_materializations=resident_texture_materializations.load(std::memory_order_relaxed);
    output->resident_texture_incompatible=resident_texture_incompatible.load(std::memory_order_relaxed);
}
int service() {
    if(!initialized || failed.load())return 0;
    try {
        gcnllr::gpu_bridge::validate_submission();
        if(fifo_worker) {
            fifo_worker->service();
#if GCNLLR_ASYNC_COPIES
            // This service is an earned guest-device deadline/observation.
            // Host GPU latency may stall the host here, but must not consume
            // more guest time while the CPU spins on a late PE completion.
            // Keep the copies batched; retire the submitted batch together.
            if(fifo_worker->pending())fifo_worker->wait();
            // Defensive completion for effects produced outside a normal
            // handoff. Ordinary batches retire before their parsed marker.
            if(streamed_pending.load(std::memory_order_acquire))fifo_worker->wait(true);
#endif
        }
#if GCNLLR_ASYNC_COPIES
        else service_streams();
#endif
        return !failed.load();
    }
    catch(const std::exception& e){fail(e.what());return 0;}
}
int peek(uint32_t depth,uint16_t x,uint16_t y,uint32_t* value) {
    if (!value || !drain()) return 0;
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if (depth==2) {
        try {return gcnllr::gpu_bridge::peek_scanout(x,y,value);}
        catch(const std::exception& e) {fail(e.what());return 0;}
    }
    // Reads address the physical EFB independently of VI's scanout height.
    // They must observe the current
    // ordered EFB, including before the first VI presentation. Reuse a CPU
    // download only until an accepted draw, clearing copy or new frame.
    if (x>=gcnllr::efb_width || y>=gcnllr::efb_height || depth>1) return 0;
    try {
        if (depth) {
            if (depth_peek_generation!=efb_generation) {
                depth_peek=gcnllr::gpu_bridge::read_depth(depth_peek_width,depth_peek_height);
                depth_peek_generation=efb_generation;
            }
            const auto sx=std::min(depth_peek_width-1,uint32_t((uint64_t(x)*2+1)*depth_peek_width/(2*gcnllr::efb_width)));
            const auto sy=std::min(depth_peek_height-1,uint32_t((uint64_t(y)*2+1)*depth_peek_height/(2*gcnllr::efb_height)));
            *value=depth_peek[size_t(sy)*depth_peek_width+sx];
        } else {
            if (color_peek_generation!=efb_generation) {
                color_peek=gcnllr::gpu_bridge::read_efb(color_peek_width,color_peek_height);
                color_peek_generation=efb_generation;
            }
            const auto sx=std::min(color_peek_width-1,uint32_t((uint64_t(x)*2+1)*color_peek_width/(2*gcnllr::efb_width)));
            const auto sy=std::min(color_peek_height-1,uint32_t((uint64_t(y)*2+1)*color_peek_height/(2*gcnllr::efb_height)));
            const auto* p=color_peek.data()+(size_t(sy)*color_peek_width+sx)*4;
            *value=(uint32_t(p[3])<<24)|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
#if GCNLLR_RETAIL_PIPELINE
            if((sink->bp[0x43]&7u)!=1u)*value|=0xff000000u;
#endif
        }
        return 1;
    } catch (const std::exception& e) {fail(e.what());return 0;}
}
int present_impl(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride,
                 const gcnllr::gpu_bridge::EncodedImageHandle& resident={},
                 const std::vector<gcnllr::gpu_bridge::DisplaySpan>& spans={}) {
    Measure measurement(present_cost);
    if (!initialized || failed.load()) return 0;
    if(GCNLLR_GX_COOPERATIVE_VI) {
        if(!service())return 0;
    } else if(!drain())return 0;
    const auto start=Clock::now();
    try {
        std::unique_lock recording(recording_mutex,std::defer_lock);
        if(GCNLLR_GX_COOPERATIVE_VI) {
            recording.lock();
            // Preserve pending geometry before closing its recording packet.
            // Owner effects release this lock only after earlier assembly was
            // flushed for their copy/token, so this cannot re-enter a draw.
            flush_geometry();
        }
        const bool shown=!spans.empty() ? gcnllr::gpu_bridge::present_display_spans(spans,width,height) :
            resident ? gcnllr::gpu_bridge::present_display_copy(resident,width,height) :
            gcnllr::gpu_bridge::present(yuy2,width,height,stride);
        if (!shown)
            throw std::runtime_error("VI frame could not be presented");
        aurora::gfx::gxcore::note_frame_presented();
        ++state.presents;
        if(resident || !spans.empty())++state.resident_scanouts;
        ++efb_generation;
        const auto now=Clock::now();
        if (frame_times) {
            const auto completion_ns=completion_service_ns.load(std::memory_order_relaxed);
            uint64_t cycles{};
#ifdef _WIN32
            QueryThreadCycleTime(GetCurrentThread(),&cycles);
#endif
            std::fprintf(frame_times,"%llu,%llu,%llu,%llu,%llu,%llu\n",(unsigned long long)state.presents,
                (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count(),
                (unsigned long long)readback_ns.load(std::memory_order_relaxed),(unsigned long long)cycles,
                (unsigned long long)(completion_ns-last_completion_service_ns));
            last_completion_service_ns=completion_ns;
        }
        pump(true);return 1;
    } catch (const std::exception& e) {fail(e.what());return 0;}
}
int present(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride) {
    return present_impl(yuy2,width,height,stride);
}
int present_copy(uint64_t token,uint32_t width,uint32_t height,uint32_t stride) {
    if(!initialized || failed.load())return -1;
    const auto retained=retained_copy(token);
    if(!(retained.copy.flags&GCNLLR_NATIVE_GX_COPY_DISPLAY) || !retained.image ||
       width!=retained.copy.row_bytes/2 || height!=retained.copy.rows ||
       stride!=retained.copy.stride)return 0;
    return present_impl(nullptr,width,height,stride,retained.image) ? 1:-1;
}
int present_spans(const GcnllrNativeGxScanoutSpan* plan,uint32_t count,uint32_t width,uint32_t height) {
    if(!initialized || failed.load())return -1;
    if(!plan || !count || count>576 || !width || !height || width>640 || height>576)return 0;
    try {
        std::vector<gcnllr::gpu_bridge::DisplaySpan> spans;spans.reserve(count);
        uint64_t end=0,total=uint64_t(width)*height*2;
        for(uint32_t n=0;n<count;n++) {
            const auto& p=plan[n];
            if(!p.size || ((p.source_offset|p.destination_offset|p.size)&3u) ||
               p.destination_offset!=end ||
               uint64_t(p.destination_offset)+p.size>total)return 0;
            if(p.token) {
                const auto retained=retained_copy(p.token);
                if(!(retained.copy.flags&GCNLLR_NATIVE_GX_COPY_DISPLAY) || !retained.image ||
                   uint64_t(p.source_offset)+p.size>uint64_t(retained.copy.row_bytes)*retained.copy.rows)return 0;
                spans.push_back({retained.image,p.source_offset,p.destination_offset,p.size,{}});
            } else {
                GcnllrNativeGxMemory memory{};
                if(!gcnllr::gx_memory(config,p.source_offset,p.size,memory))return 0;
                if(config.pending_read && config.pending_read(config.user,p.source_offset,p.size) &&
                   (!config.prepare_read || !config.prepare_read(config.user,p.source_offset,p.size)))
                    throw std::runtime_error("CPU scanout observation failed");
                spans.push_back({{},0,p.destination_offset,p.size,
                    std::vector<uint8_t>(memory.bytes,memory.bytes+p.size)});
            }
            end+=p.size;
        }
        if(end!=total)return 0;
        return present_impl(nullptr,width,height,width*2,{},spans)?1:-1;
    } catch(const std::exception& e){fail(e.what());return -1;}
}
const GcnllrNativeGxApi api={GCNLLR_NATIVE_GX_ABI,start,fifo,drain,status,peek,stop,present,service,
    GCNLLR_NATIVE_GX_RESIDENT_COPY|(GCNLLR_GX_COOPERATIVE_VI ? GCNLLR_NATIVE_GX_COMPLETED_SCANOUT : 0u)
        |(GCNLLR_ASYNC_COPIES ? GCNLLR_NATIVE_GX_STREAMED_COPY : 0u)
        |(GCNLLR_COMPOSED_SCANOUT ? GCNLLR_NATIVE_GX_DEFERRED_RETIREMENT : 0u)
        |(GCNLLR_GX_COOPERATIVE_VI ? GCNLLR_NATIVE_GX_COMPOSED_SCANOUT : 0u)
        |(!GCNLLR_GX_FIFO_WORKER && !GCNLLR_ASYNC_COPIES && !GCNLLR_COMPOSED_SCANOUT && !GCNLLR_GX_COOPERATIVE_VI &&
          !GCNLLR_RESIDENT_XFB ? GCNLLR_NATIVE_GX_SERIAL_RAM_WRITES : 0u),
    materialize_copy,release_copy,present_copy,present_spans};
}
#ifdef _WIN32
#define GCNLLR_EXPORT __declspec(dllexport)
#else
#define GCNLLR_EXPORT __attribute__((visibility("default")))
#endif
#if GCNLLR_RETAIL_PIPELINE
extern "C" uint64_t gcnllr_aurora_retail_array(const void* data,uint32_t bytes) {
    if(!retail_active || !data || !bytes)return 0;
    uint32_t address;
    if(!gcnllr::gx_address(config,data,bytes,address))
        throw std::runtime_error("Native retail array exceeds consumer memory");
    const auto identity=retail_texture_ids.identify({address,bytes,0,0,UINT32_MAX,0},
        static_cast<const uint8_t*>(data),[&](uint64_t* epoch){return memory_epoch(address,bytes,epoch);});
    return (uint64_t(identity.first)<<32)|identity.second;
}
extern "C" bool gcnllr_aurora_retail_texture(const void* data,uint32_t bytes,uint32_t width,
    uint32_t height,uint32_t format,uint32_t mips,uint32_t* id,uint32_t* version) {
    if(!retail_active || !bytes || !data)return false;
    uint32_t address;
    if(!gcnllr::gx_address(config,data,bytes,address))return false;
    const auto identity=retail_texture_ids.identify({address,bytes,width,height,format,mips},
        static_cast<const uint8_t*>(data),[&](uint64_t* epoch){return texture_epoch(address,bytes,epoch);});
    *id=identity.first;*version=identity.second;return true;
}
extern "C" void gcnllr_aurora_retail_draw() {
    if(retail_active){++state.draws;++efb_generation;}
}
extern "C" void gcnllr_aurora_retail_observe(const void* data,uint32_t size) {
    if(!retail_active || !size || !data)return;
    uint32_t address;
    if(!gcnllr::gx_address(config,data,0,address))return; /* donor-owned storage */
    if(!gcnllr::gx_address(config,data,size,address) || !observe_pending_memory(address,size))
        throw std::runtime_error("Native retail resource observation failed");
}
// The exact live machine producer is the authority, including CPU writes and
// overlap retirement. On a miss the normal native texture decoder observes RAM.
std::shared_ptr<aurora::gfx::TextureRef> gcnllr_aurora_retail_owned(
    const void* data,uint32_t bytes,uint32_t width,uint32_t height,uint32_t format,uint32_t mips) {
    if(!retail_active || !config.own_copy || !data || !bytes || mips>1)return {};
    uint32_t address;
    if(!gcnllr::gx_address(config,data,bytes,address))return {};
    return owned_texture(address,bytes,format,width,height);
}
extern "C" bool gcnllr_aurora_retail_bp(uint32_t reg,uint32_t value) {
    if(!retail_active)return false;
    sink->bp[reg]=value;sink->bp_seen[reg]=true;
    if(reg==0x45 || reg==0x47 || reg==0x48) {
        gcnllr_aurora_retail_finish();
        flush_display_copies();ordered_bp_effect(reg,value);return true;
    }
    if(reg!=0x52)return false;
    gcnllr_aurora_retail_finish();
    const auto& bp=sink->bp;
    const uint32_t width=(bp[0x4a]&1023u)+1,height=((bp[0x4a]>>10)&1023u)+1;
    const bool display=(value&(1u<<14))!=0;
    const bool half=!display && (value&(1u<<9));
    const uint32_t dst_width=half?(width+1)/2:width,dst_height=half?(height+1)/2:height;
    const uint32_t format=display?0xf:gcnllr::retail_copy_format(value,(bp[0x43]&7u)==3u);
    const auto tile=gcnllr::native_copy_layout(format);
    if(!display && !tile.bytes)throw std::runtime_error("Native retail copy format is unsupported");
    const uint32_t bytes=display?0:((dst_width+tile.width-1)/tile.width)*
        ((dst_height+tile.height-1)/tile.height)*tile.bytes;
    const auto c0=bp[0x4f],c1=bp[0x50],cz=bp[0x51];
    const gxruntime::gxcore::EfbCopyCommand command{
        .dest_address=(bp[0x4b]&guest_gpu_address_mask)<<5,.byte_size=bytes,.format=format,
        .src_x=bp[0x49]&1023u,.src_y=(bp[0x49]>>10)&1023u,.width=width,.height=height,
        .destination_width=dst_width,.destination_height=dst_height,.clear=(value&(1u<<11))!=0,
        .clear_r=c0&255,.clear_g=(c1>>8)&255,.clear_b=c1&255,.clear_a=(c0>>8)&255,.clear_z=cz&0xffffff,
        .color_update=!sink->bp_seen[0x41] || (bp[0x41]&8)!=0,
        .alpha_update=!sink->bp_seen[0x41] || (bp[0x41]&16)!=0,
        .depth_update=!sink->bp_seen[0x40] || (bp[0x40]&16)!=0,
        .efb_has_alpha=!sink->bp_seen[0x43] || (bp[0x43]&7)==1};
    copy(command,nullptr);
    if(!display) {
        aurora::gx::g_gxState.stateDirty=true;
        for(auto& binding:aurora::gx::g_gxState.textures)binding.texObj.texObjId=0;
    }
    return true;
}
#endif
extern "C" GCNLLR_EXPORT const GcnllrNativeGxApi* gcnllr_native_gx_get_api(uint32_t abi) {
    return abi==GCNLLR_NATIVE_GX_ABI ? &api : nullptr;
}
namespace {
int native_idle_dimming(uint32_t enabled) {
    if(!initialized || failed.load() || enabled>1)return 0;
    gcnllr::gpu_bridge::set_idle_dimming(enabled!=0);return 1;
}
const GcWiiNativeVideoApi native_video_api={GC_WII_NATIVE_VIDEO_ABI,native_idle_dimming};
}
extern "C" GCNLLR_EXPORT const GcWiiNativeVideoApi* gc_wii_native_video_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_VIDEO_ABI?&native_video_api:nullptr;
}
namespace {
int presentation_frame(uint32_t address,const GcWiiNativePresentationFrame* frame) {
    if(!initialized || failed.load())return 0;
    try{return gcnllr::gpu_bridge::set_presentation_frame(address,frame);}catch(const std::exception& e){fail(e.what());return 0;}
}
int presentation_source(uint32_t address) {
    if(!initialized || failed.load())return 0;
    gcnllr::gpu_bridge::select_presentation_source(address);return 1;
}
int presentation_window(uint32_t* width,uint32_t* height) {
    if(!initialized || !width || !height)return 0;
    int w=0,h=0;auto* window=aurora::window::get_sdl_window();
    if(!window || !SDL_GetWindowSizeInPixels(window,&w,&h) || w<=0 || h<=0)return 0;
    *width=uint32_t(w);*height=uint32_t(h);return 1;
}
int presentation_read(uint8_t* output,uint32_t capacity,uint32_t* width,uint32_t* height) {
    if(!initialized || failed.load())return 0;
    try{return gcnllr::gpu_bridge::read_presentation(output,capacity,width,height);}catch(const std::exception& e){fail(e.what());return 0;}
}
int presentation_resize(uint32_t width,uint32_t height) {
    if(!initialized || width<160 || height<120 || width>8192 || height>8192)return 0;
    auto* window=aurora::window::get_sdl_window();
    return window && SDL_SetWindowSize(window,int(width),int(height));
}
const GcWiiNativePresentationApi presentation_api={GC_WII_NATIVE_PRESENTATION_ABI,
    presentation_frame,presentation_source,presentation_window,presentation_resize,presentation_read};
}
extern "C" GCNLLR_EXPORT const GcWiiNativePresentationApi* gc_wii_native_presentation_get_api(uint32_t abi) {
    /* Queued scanout needs its own retained target integration before it can
     * offer this extension. Existing presentation remains available there. */
    return !GCNLLR_QUEUED_PRESENT && abi==GC_WII_NATIVE_PRESENTATION_ABI?&presentation_api:nullptr;
}

namespace {
uint64_t create_native_mesh(const GcWiiNativeMesh* mesh) {
    if (!initialized || failed.load() || fifo_worker || !mesh) {
        native_scene.rejected(); return 0;
    }
    try { return native_scene.create_mesh(*mesh); }
    catch (const std::exception& e) { fail(e.what()); return 0; }
}
int destroy_native_mesh(uint64_t mesh) { return native_scene.destroy_mesh(mesh); }
bool native_scene_ready() {
    if (!initialized || !frame_open || failed.load() || fifo_worker ||
        !frontend || frontend->pending_fifo_size()) {
        std::fprintf(stderr,"[native-scene-boundary] initialized=%u frame=%u failed=%u worker=%u frontend=%u pending=%zu\n",
            unsigned(initialized),unsigned(frame_open),unsigned(failed.load()),unsigned(bool(fifo_worker)),
            unsigned(bool(frontend)),frontend?size_t(frontend->pending_fifo_size()):size_t(0));
        native_scene.rejected(); return false;
    }
    return true;
}
int submit_native_scene(gxruntime::gxcore::DrawPlan* plan) {
        if (!plan) return 0;
        const auto image=[](uint32_t address,uint32_t bytes,const void*& data,uint32_t& available,DolGuestResourceKind kind) {
            DolGuestResolvedRange range{};
            if(!resolve(nullptr,address,bytes,DOL_GUEST_ADDRESS_AUTO,kind,&range)){std::fprintf(stderr,"[native-scene-rejected] Unreadable resource address=%08x size=%u\n",address,bytes);return false;}
            data=range.data;available=range.available;return true;
        };
        if (plan->has_texture) {
            if(plan->texmap_mask) {
                for(unsigned i=0;i<8;++i)if(plan->texmap_mask&(1u<<i)) {
                    auto& t=plan->textures[i];
                    if(!t.valid || !image(t.address,t.size,t.data,t.available,DOL_GUEST_RESOURCE_TEXTURE) ||
                       (t.has_tlut && (t.tlut_entries>16384 || t.tlut_format>2 ||
                        !image(t.tlut_address,t.tlut_entries*2,t.tlut_data,t.tlut_available,DOL_GUEST_RESOURCE_TLUT)))) {
                        native_scene.rejected();return 0;
                    }
                }
            } else {
                if(!image(plan->tex_address,plan->tex_size,plan->tex_data,plan->tex_available,DOL_GUEST_RESOURCE_TEXTURE) ||
                   (plan->has_tlut && (plan->tlut_entries>16384 || plan->tlut_format>2 ||
                    !image(plan->tlut_address,plan->tlut_entries*2,plan->tlut_data,plan->tlut_available,DOL_GUEST_RESOURCE_TLUT)))) {
                    native_scene.rejected();return 0;
                }
            }
        }
        // Direct typed submission. No command construction, FIFO append,
        // register decoding or original guest renderer executes here.
        if (!aurora::gfx::gxcore::submit_draw_plan(*plan)) {
            native_scene.rejected(); fail("Native scene draw submission failed"); return 0;
        }
        native_scene.submitted(); ++state.draws; ++efb_generation;
        return 1;
}
int draw_native_mesh(const GcWiiNativeDraw* job) {
    if(!job){native_scene.rejected();return 0;}
    if(!native_scene_ready())return 0;
    try {return submit_native_scene(native_scene.prepare(*job));}
    catch (const std::exception& e) { fail(e.what()); return 0; }
}
int draw_native_gui(const GcWiiNativeGuiDraw* job) {
    if(!job){native_scene.rejected();return 0;}
    if(!native_scene_ready())return 0;
    try {return submit_native_scene(gcnllr::prepare_gui(native_scene,*job));}
    catch (const std::exception& e) { fail(e.what()); return 0; }
}
int draw_native_gui_layers(const GcWiiNativeGuiLayersDraw* job) {
    if(!job){native_scene.rejected();return 0;}
    if(!native_scene_ready())return 0;
    try {return submit_native_scene(gcnllr::prepare_gui_layers(native_scene,*job));}
    catch(const std::exception& e){fail(e.what());return 0;}
}
int draw_native_gui_mask(const GcWiiNativeGuiMaskDraw* job) {
    if(!job){native_scene.rejected();return 0;}
    if(!native_scene_ready())return 0;
    try{return submit_native_scene(gcnllr::prepare_gui_mask(native_scene,*job));}
    catch(const std::exception& e){fail(e.what());return 0;}
}
void native_scene_status(GcWiiNativeRenderStatus* output) {
    if (output) *output=native_scene.status();
}
const GcWiiNativeRenderApi native_scene_api={GC_WII_NATIVE_RENDER_ABI,
    create_native_mesh,destroy_native_mesh,draw_native_mesh,native_scene_status};
const GcWiiNativeGuiApi native_gui_api={GC_WII_NATIVE_GUI_ABI,draw_native_gui};
const GcWiiNativeGuiLayersApi native_gui_layers_api={GC_WII_NATIVE_GUI_LAYERS_ABI,draw_native_gui_layers};
const GcWiiNativeGuiMaskApi native_gui_mask_api={GC_WII_NATIVE_GUI_MASK_ABI,draw_native_gui_mask};
template<class Input> int draw_gui_effect(const Input* input,gxruntime::gxcore::DrawPlan* (*prepare)(gcnllr::NativeScene&,const Input&)) {
    if(!input){native_scene.rejected();return 0;}if(!native_scene_ready())return 0;
    try{return submit_native_scene(prepare(native_scene,*input));}catch(const std::exception& e){fail(e.what());return 0;}
}
int draw_gui_cutout(const GcWiiNativeGuiCutoutDraw* input){return draw_gui_effect(input,gcnllr::prepare_gui_cutout);}
int draw_gui_add(const GcWiiNativeGuiAddDraw* input){return draw_gui_effect(input,gcnllr::prepare_gui_add);}
int draw_gui_product(const GcWiiNativeGuiProductDraw* input){return draw_gui_effect(input,gcnllr::prepare_gui_product);}
uint64_t create_gui_mesh(const GcWiiNativeGuiMesh* input) {
    if(!initialized || failed.load() || fifo_worker || !input){native_scene.rejected();return 0;}
    try{return native_scene.create_mesh(input->mesh,input->extra_uv,input->extra_uv_count);}catch(const std::exception& e){fail(e.what());return 0;}
}
const GcWiiNativeGuiEffectsApi native_gui_effects_api={GC_WII_NATIVE_GUI_EFFECTS_ABI,create_gui_mesh,draw_gui_cutout,draw_gui_add,draw_gui_product};
}
extern "C" GCNLLR_EXPORT const GcWiiNativeRenderApi* gc_wii_native_render_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_RENDER_ABI && !GCNLLR_GX_FIFO_WORKER ? &native_scene_api:nullptr;
}
extern "C" GCNLLR_EXPORT const GcWiiNativeGuiApi* gc_wii_native_gui_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_GUI_ABI && !GCNLLR_GX_FIFO_WORKER ? &native_gui_api:nullptr;
}
extern "C" GCNLLR_EXPORT const GcWiiNativeGuiLayersApi* gc_wii_native_gui_layers_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_GUI_LAYERS_ABI && !GCNLLR_GX_FIFO_WORKER?&native_gui_layers_api:nullptr;
}
extern "C" GCNLLR_EXPORT const GcWiiNativeGuiMaskApi* gc_wii_native_gui_mask_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_GUI_MASK_ABI && !GCNLLR_GX_FIFO_WORKER?&native_gui_mask_api:nullptr;
}
extern "C" GCNLLR_EXPORT const GcWiiNativeGuiEffectsApi* gc_wii_native_gui_effects_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_GUI_EFFECTS_ABI && !GCNLLR_GX_FIFO_WORKER?&native_gui_effects_api:nullptr;
}

namespace {
int reject_native_frame(const char* error) {
    ++native_frame_state.rejected;
    std::snprintf(native_frame_state.error,sizeof(native_frame_state.error),"%s",error);return 0;
}
bool native_frame_ready() {
    if(!initialized || !frame_open || failed.load() || fifo_worker || !frontend || frontend->pending_fifo_size()) {
        reject_native_frame("Native frame requires an open frame and drained machine ingress");return false;
    }
    return true;
}
bool native_clear_value(const GcWiiNativeClear& value) {
    return value.red<=255 && value.green<=255 && value.blue<=255 && value.alpha<=255 &&
        value.depth<=0xffffff && !(value.flags&~7u);
}
bool native_frame_rect(uint32_t x,uint32_t y,uint32_t width,uint32_t height) {
    return x<gcnllr::efb_width && y<gcnllr::efb_height && width && height &&
        width<=gcnllr::efb_width-x && height<=gcnllr::efb_height-y;
}
gxruntime::gxcore::EfbCopyCommand native_clear_command(const GcWiiNativeClear& value) {
    return {.src_x=value.x,.src_y=value.y,.width=value.width,.height=value.height,.clear=true,
        .clear_r=value.red,.clear_g=value.green,.clear_b=value.blue,.clear_a=value.alpha,.clear_z=value.depth,
        .color_update=bool(value.flags&GC_WII_NATIVE_CLEAR_COLOR),
        .alpha_update=bool(value.flags&GC_WII_NATIVE_CLEAR_ALPHA),
        .depth_update=bool(value.flags&GC_WII_NATIVE_CLEAR_DEPTH)};
}
int native_frame_clear(const GcWiiNativeClear* value) {
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if(!native_frame_ready())return 0;
    if(!value || !native_clear_value(*value) || !native_frame_rect(value->x,value->y,value->width,value->height))
        return reject_native_frame("Unsupported native EFB clear rectangle/value/masks");
    try {
        flush_geometry();flush_display_copies();
        gcnllr::gpu_bridge::record_efb_clear(native_clear_command(*value));
        if(value->flags)++efb_generation;
        ++native_frame_state.clears;return 1;
    } catch(const std::exception& e){reject_native_frame(e.what());fail(e.what());return 0;}
}
int native_frame_display_copy(const GcWiiNativeDisplayCopy* value) {
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if(!native_frame_ready())return 0;
    if(!value || !native_frame_rect(value->x,value->y,value->width,value->height) ||
        ((value->x|value->width)&1u) || !value->scale_y || value->scale_y>65535 || value->invert_y_scale>1 ||
        value->gamma || (value->flags&~7u) || value->stride!=value->width*2 ||
        ((value->destination|value->stride)&31u) || !native_clear_value(value->clear_value))
        return reject_native_frame("Unsupported native YUY2 display-copy descriptor");
    for(auto tap:value->filter)if(tap>63)return reject_native_frame("Native display filter tap exceeds six bits");
    if(!gcnllr::gpu_bridge::presentation_matches_copy(value->destination))
        return reject_native_frame("Presentation does not name this display copy");
    const float scale=value->invert_y_scale?256.f/float(value->scale_y):float(value->scale_y)/256.f;
    const auto rows=uint32_t(float(value->height)*scale);
    GcnllrNativeGxMemory memory{};
    if(!rows || rows>576 || !gcnllr::gx_memory(config,value->destination,value->stride*rows,memory))
        return reject_native_frame("Native display-copy destination is outside its coherent memory bank");
    auto command=native_clear_command(value->clear_value);
    command.dest_address=memory.physical_address;command.format=0xf;
    command.src_x=value->x;command.src_y=value->y;command.width=command.destination_width=value->width;
    command.height=command.destination_height=value->height;command.clear=bool(value->flags&GC_WII_NATIVE_COPY_CLEAR);
    const auto* taps=value->filter;
    const gcnllr::gpu_bridge::DisplayCopyFilter filter{
        taps[0]|(taps[1]<<6)|(taps[2]<<12)|(taps[3]<<18),taps[4]|(taps[5]<<6)|(taps[6]<<12),
        bool(value->flags&GC_WII_NATIVE_COPY_CLAMP_TOP),bool(value->flags&GC_WII_NATIVE_COPY_CLAMP_BOTTOM)};
    try {
        flush_geometry();
        display_readback(command,memory.physical_address,value->stride,scale,filter);
        if(command.clear)++efb_generation;
        flush_display_copies();++native_frame_state.display_copies;return 1;
    } catch(const std::exception& e){reject_native_frame(e.what());fail(e.what());return 0;}
}
int native_frame_complete(uint32_t events,uint32_t token) {
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if(!native_frame_ready())return 0;
    if((events&~7u) || (events&6u)==6u || token>65535u)
        return reject_native_frame("Unsupported native completion event/token");
    try {
        flush_geometry();flush_display_copies();
        gcnllr::gpu_bridge::complete_frame(!GCNLLR_OWNED_COMPLETION);
        if(events&GC_WII_NATIVE_COMPLETE_TOKEN)ordered_bp_effect(0x47,token);
        if(events&GC_WII_NATIVE_COMPLETE_TOKEN_INTERRUPT)ordered_bp_effect(0x48,token);
        if(events&GC_WII_NATIVE_COMPLETE_DRAW_DONE)ordered_bp_effect(0x45,2);
        retire_streams(true);++native_frame_state.completions;return 1;
    } catch(const std::exception& e){reject_native_frame(e.what());fail(e.what());return 0;}
}
void native_frame_status(GcWiiNativeFrameStatus* value){if(value)*value=native_frame_state;}
const GcWiiNativeFrameApi native_frame_api={GC_WII_NATIVE_FRAME_ABI,native_frame_clear,native_frame_display_copy,
    native_frame_complete,native_frame_status};
}
extern "C" GCNLLR_EXPORT const GcWiiNativeFrameApi* gc_wii_native_frame_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_FRAME_ABI && !GCNLLR_GX_FIFO_WORKER?&native_frame_api:nullptr;
}

namespace {
int native_output_submit() {
    // This extension deliberately retains the serial machine-owner profile.
    // Only immutable GPU resources are deferred, never guest-memory stores.
    if(!config.own_copy || !config.pending_read || !config.prepare_read ||
       GCNLLR_GX_FIFO_WORKER || GCNLLR_ASYNC_COPIES || GCNLLR_COMPOSED_SCANOUT ||
       GCNLLR_GX_COOPERATIVE_VI || GCNLLR_RESIDENT_XFB)
        return reject_native_frame("Native output requires guarded serial ownership");
    if(!native_frame_ready())return 0;
    try {
        flush_geometry();flush_display_copies();
        gcnllr::gpu_bridge::complete_frame(false);
        ++native_frame_state.completions;return 1;
    } catch(const std::exception& e){reject_native_frame(e.what());fail(e.what());return 0;}
}
const GcWiiNativeOutputApi native_output_api={GC_WII_NATIVE_OUTPUT_ABI,native_output_submit};
}
extern "C" GCNLLR_EXPORT const GcWiiNativeOutputApi* gc_wii_native_output_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_OUTPUT_ABI && !GCNLLR_GX_FIFO_WORKER && !GCNLLR_ASYNC_COPIES &&
        !GCNLLR_COMPOSED_SCANOUT && !GCNLLR_GX_COOPERATIVE_VI && !GCNLLR_RESIDENT_XFB?&native_output_api:nullptr;
}

#if GC_WII_NATIVE_J3D
namespace {
std::span<const uint8_t> native_j3d_resolve(void*,uint32_t address,size_t length) {
    if(length>UINT32_MAX)return {};
    DolGuestResolvedRange range{};
    if(!resolve(nullptr,address,uint32_t(length),DOL_GUEST_ADDRESS_AUTO,DOL_GUEST_RESOURCE_TEXTURE,&range) || range.available<length)return {};
    return {static_cast<const uint8_t*>(range.data),length};
}
uint64_t create_native_j3d(uint32_t address) {
    if(!initialized || failed.load() || fifo_worker){native_j3d.rejected("Native J3D service is unavailable");return 0;}
    try {
        auto header=native_j3d_resolve(nullptr,address,32);
        if(header.empty())throw std::runtime_error("Native J3D resource header is unreadable");
        const auto size=uint32_t(header[8])<<24|uint32_t(header[9])<<16|uint32_t(header[10])<<8|header[11];
        if(size<32 || size>64u*1024u*1024u)throw std::runtime_error("Native J3D resource size is invalid");
        auto bytes=native_j3d_resolve(nullptr,address,size);
        if(bytes.empty())throw std::runtime_error("Native J3D resource is incomplete");
        return native_j3d.create(bytes,{address,nullptr,native_j3d_resolve});
    }catch(const std::exception& e){native_j3d.rejected(e.what());return 0;}
}
int destroy_native_j3d(uint64_t handle){return native_j3d.destroy(handle);}
int draw_native_j3d(const GcWiiJ3DDraw* input) {
    if(!initialized || !frame_open || failed.load() || fifo_worker || !input || !frontend || frontend->pending_fifo_size()) {
        char reason[192];
        std::snprintf(reason,sizeof(reason),"Native J3D boundary unavailable: initialized=%u frame=%u failed=%u worker=%u input=%u frontend=%u pending=%zu",
            unsigned(initialized),unsigned(frame_open),unsigned(failed.load()),unsigned(bool(fifo_worker)),unsigned(input!=nullptr),unsigned(bool(frontend)),
            frontend?size_t(frontend->pending_fifo_size()):size_t(0));
        native_j3d.rejected(reason);return 0;
    }
    try {
        auto* prepared=native_j3d.prepare(*input);
        auto image=[&](uint32_t& address,uint32_t size,const void*& data,uint32_t& available,DolGuestResourceKind kind) {
            GcnllrNativeGxMemory range{};
            if(!gcnllr::gx_memory(config,address,size,range) || range.available<size ||
               (kind==DOL_GUEST_RESOURCE_TLUT && !observe_pending_memory(range.physical_address,size)))
                throw std::runtime_error("Native J3D texture memory is unreadable");
            address=range.physical_address;data=range.bytes;available=range.available;
        };
        // Texture memory may be rewritten by the CPU or owned by an earlier
        // GPU copy. Use the consumer's coherent physical identity and current
        // bytes; a one-time model import must not freeze animated image data.
        auto& material=prepared->draw.material;
        for(unsigned t=0;t<8;++t)if(material.texture_mask&(1u<<t)) {
            auto& texture=material.textures[t];image(texture.address,texture.size,texture.data,texture.available,DOL_GUEST_RESOURCE_TEXTURE);
            if(texture.has_tlut)image(texture.tlut_address,texture.tlut_entries*2,texture.tlut_data,texture.tlut_available,DOL_GUEST_RESOURCE_TLUT);
        }
        for(const auto& mesh:prepared->meshes) {
            if(!aurora::gfx::gxcore::submit_resident_draw(mesh,prepared->draw))throw std::runtime_error("Native resident J3D GPU submission failed");
            ++state.draws;++efb_generation;native_j3d.submitted();
        }
        return 1;
    }catch(const std::exception& e){native_j3d.rejected(e.what());return 0;}
}
void native_j3d_status(GcWiiJ3DStatus* output){if(output)*output=native_j3d.status();}
const GcWiiNativeJ3DApi native_j3d_api={GC_WII_NATIVE_J3D_ABI,create_native_j3d,destroy_native_j3d,draw_native_j3d,native_j3d_status};
}
extern "C" GCNLLR_EXPORT const GcWiiNativeJ3DApi* gc_wii_native_j3d_get_api(uint32_t abi) {
    return abi==GC_WII_NATIVE_J3D_ABI?&native_j3d_api:nullptr;
}
#endif
