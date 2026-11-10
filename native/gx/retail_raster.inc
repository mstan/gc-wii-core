// SPDX-License-Identifier: MIT
// Included inside Aurora's selected retail command processor namespace.
// Retain the original XF projection/viewport, then own raster mapping once
// when their inputs or wrapped scissor change. Host viewport stays in bounds.
static float machine_viewport[6]{};
static decltype(g_gxState.proj) machine_projection;
static bool machine_viewport_valid{},machine_projection_valid{},machine_raster_dirty=true;
static void machine_raster() {
    if(!machine_raster_dirty || !machine_viewport_valid || !machine_projection_valid)return;
    machine_raster_dirty=false;
    const auto& bp=g_gxState.bpRegCache;
    const auto origin=gcnllr::raster_origin(machine_viewport,bp[0x20],bp[0x21],bp[0x59]);
    float projection[4][4],viewport[6];
    static_assert(sizeof(projection)==sizeof(machine_projection));
    std::memcpy(projection,&machine_projection,sizeof(projection));
    std::memcpy(viewport,machine_viewport,sizeof(viewport));
    gcnllr::remap_raster_viewport(projection,viewport,origin,640,480);
    std::memcpy(&g_gxState.proj,projection,sizeof(projection));
    const int left=std::clamp(int((bp[0x20]>>12)&2047)-origin.x,0,640);
    const int top=std::clamp(int(bp[0x20]&2047)-origin.y,0,480);
    const int right=std::clamp(int((bp[0x21]>>12)&2047)+1-origin.x,left,640);
    const int bottom=std::clamp(int(bp[0x21]&2047)+1-origin.y,top,480);
    set_logical_scissor({left,top,right-left,bottom-top});
    set_logical_viewport({0,0,640,480,
        (machine_viewport[5]-machine_viewport[2])/1.6777215e7f,machine_viewport[5]/1.6777215e7f});
    g_gxState.stateDirty=true;
}
