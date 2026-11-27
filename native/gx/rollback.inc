// SPDX-License-Identifier: MIT
extern "C" void* gcnllr_aurora_retail_capture();
extern "C" void gcnllr_aurora_retail_restore(const void*);
extern "C" void gcnllr_aurora_retail_release(void*);

struct RetailRollback {
    uint64_t session{};
    void* decoder{};
    decltype(MachineSink::bp) bp;
    decltype(MachineSink::bp_seen) bp_seen;
    std::vector<uint8_t> pending;
    gcnllr::gpu_bridge::RollbackEfbHandle efb;
    decltype(retained_copies) copies;
    decltype(texture_producers) producers;
    ~RetailRollback(){gcnllr_aurora_retail_release(decoder);}
};

bool retail_rollback_supported() {
    return initialized && !fifo_worker && !GCNLLR_ASYNC_COPIES && !failed.load() &&
        frontend && sink && gcnllr::gpu_bridge::rollback_efb_supported();
}

int capture_rollback(void** output) {
    if(!output)return 0;
    *output=nullptr;
    if(!retail_rollback_supported() || !drain())return 0;
    try {
        auto saved=std::make_unique<RetailRollback>();saved->session=rollback_session;
        saved->pending=frontend->checkpoint();
        saved->bp=sink->bp;saved->bp_seen=sink->bp_seen;
        // Capture after EFB submission: its frame continuation invalidates
        // staging ranges, which the logical owner rebuilds on restoration.
        saved->efb=gcnllr::gpu_bridge::capture_rollback_efb();
        if(!saved->efb)return 0;
        saved->decoder=gcnllr_aurora_retail_capture();
        {
            std::lock_guard lock(retained_mutex);
            saved->copies=retained_copies;saved->producers=texture_producers;
        }
        *output=saved.release();return 1;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"[native-gx] rollback capture failed: %s\n",error.what());return 0;
    }
}

int restore_rollback(const void* data) {
    const auto* saved=static_cast<const RetailRollback*>(data);
    if(!saved || saved->session!=rollback_session || !retail_rollback_supported())return 0;
    try {
        auto copies=saved->copies;auto producers=saved->producers;auto pending=saved->pending;
        if(!drain() || !gcnllr::gpu_bridge::restore_rollback_efb(saved->efb))return 0;
        gcnllr_aurora_retail_restore(saved->decoder);
        frontend->restore_checkpoint(std::move(pending));
        sink->bp=saved->bp;sink->bp_seen=saved->bp_seen;
        {
            std::lock_guard lock(retained_mutex);
            retained_copies.swap(copies);texture_producers.swap(producers);
        }
        retail_texture_ids.clear();invalidate_memory_observations();
        ++efb_generation;color_peek_generation=depth_peek_generation=0;
        // Tokens remain monotonic across branches. Older snapshots retain
        // their immutable token maps and can still be restored independently.
        return 1;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"[native-gx] rollback restore failed: %s\n",error.what());return 0;
    }
}
void release_rollback(void* data){delete static_cast<RetailRollback*>(data);}
const GcnllrNativeGxRollbackApi rollback_api{capture_rollback,restore_rollback,release_rollback};
