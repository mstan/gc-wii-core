// SPDX-License-Identifier: MIT
// Included in gpu_bridge; physical completion and validation remain unchanged.
static void complete_frame_submission(bool physical,bool allow_empty_continuation) {
    if (!((allow_empty_continuation && !physical && !recorded_copies) ? gfx::submit_frame_if_needed() : gfx::segment_frame()))
        throw std::runtime_error("Native frame completion submission failed");
    recorded_copies=false;
    if (physical) {
        struct Completion {bool done{},success{};};
        auto result=std::make_shared<Completion>();
        worker::enqueue_work([result] {
            const auto future=gpu::g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                [result](wgpu::QueueWorkDoneStatus status,wgpu::StringView) {
                    result->success=status==wgpu::QueueWorkDoneStatus::Success;result->done=true;
                });
            gpu::g_instance.WaitAny(future,5000000000ull);
        });
        worker::synchronize();
        if (!result->done || !result->success) throw std::runtime_error("Native frame GPU completion failed");
    } else if(!GCNLLR_NATIVE_QUEUED_OUTPUT)worker::synchronize();
    validate_submission();
}
void complete_frame(bool physical) { complete_frame_submission(physical,false); }
void complete_output() { complete_frame_submission(false,true); }
