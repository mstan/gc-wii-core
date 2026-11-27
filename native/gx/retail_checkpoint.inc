// SPDX-License-Identifier: MIT
// Same-process logical decoder state. Referenced texture handles retain their
// resources; staging ranges and prepared geometry are rebuilt after restore.
struct MachineRetailCheckpoint {
    decltype(g_gxState) gx;
    decltype(machine_cp) cp;
    decltype(machine_cp_valid) cp_valid;
    decltype(machine_bp_valid) bp_valid;
    std::array<float,6> viewport;
    decltype(machine_projection) projection;
    decltype(machine_overlay) overlay;
    float split,shift,scale,anchor_x,anchor_y;
    bool viewport_valid,projection_valid;
};

extern "C" void* gcnllr_aurora_retail_capture() {
    if(s_display_list_depth)throw std::runtime_error("Retail capture inside a display list");
    machine_flush_batch();
    auto checkpoint=std::make_unique<MachineRetailCheckpoint>();
    checkpoint->gx=g_gxState;checkpoint->cp=machine_cp;
    checkpoint->cp_valid=machine_cp_valid;checkpoint->bp_valid=machine_bp_valid;
    std::copy(std::begin(machine_viewport),std::end(machine_viewport),checkpoint->viewport.begin());
    checkpoint->projection=machine_projection;checkpoint->overlay=machine_overlay;
    checkpoint->split=machine_overlay_split_x;checkpoint->shift=machine_overlay_shift_x;
    checkpoint->scale=machine_view_scale;checkpoint->anchor_x=machine_view_anchor_x;
    checkpoint->anchor_y=machine_view_anchor_y;
    checkpoint->viewport_valid=machine_viewport_valid;checkpoint->projection_valid=machine_projection_valid;
    return checkpoint.release();
}

extern "C" void gcnllr_aurora_retail_restore(const void* data) {
    const auto& checkpoint=*static_cast<const MachineRetailCheckpoint*>(data);
    // Allocate all copies before replacing logical state.
    auto gx=checkpoint.gx;
    for(auto& array:gx.arrays)array.cachedRange={};
    gcnllr_aurora_retail_reset();
    g_gxState=std::move(gx);g_gxState.stateDirty=true;
    machine_cp=checkpoint.cp;machine_cp_valid=checkpoint.cp_valid;machine_bp_valid=checkpoint.bp_valid;
    std::copy(checkpoint.viewport.begin(),checkpoint.viewport.end(),std::begin(machine_viewport));
    machine_projection=checkpoint.projection;machine_overlay=checkpoint.overlay;
    machine_overlay_split_x=checkpoint.split;machine_overlay_shift_x=checkpoint.shift;
    machine_view_scale=checkpoint.scale;machine_view_anchor_x=checkpoint.anchor_x;machine_view_anchor_y=checkpoint.anchor_y;
    machine_viewport_valid=checkpoint.viewport_valid;machine_projection_valid=checkpoint.projection_valid;
    machine_raster_dirty=true;
}
extern "C" void gcnllr_aurora_retail_release(void* data) {delete static_cast<MachineRetailCheckpoint*>(data);}
