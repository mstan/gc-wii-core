// SPDX-License-Identifier: MIT
struct MachineOverlayLayout {
    u32 columns{},rows{},tiles{},width{},height{};
};
static MachineOverlayLayout machine_overlay;

extern "C" void gcnllr_aurora_retail_overlay(u32 columns,u32 rows,u32 tiles,u32 width,u32 height) {
    machine_overlay={columns,rows,tiles,width,height};
}

template<class Draw>
static void machine_draw_shared_overlay(const Draw& draw) {
    if(!machine_overlay.tiles) {
        draw();
        return;
    }
    const auto [columns,rows,tiles,width,height]=machine_overlay;
    const int tile_width=static_cast<int>(width/columns);
    const int tile_height=static_cast<int>(height/rows);
    const float scale=float(tile_width)/640.f;
    const float source_center_y=1.f-float(height)/480.f;
    float original[4][4],projection[4][4];
    std::memcpy(original,&g_gxState.proj,sizeof(original));
    std::memcpy(projection,original,sizeof(projection));
    const auto scissor=g_gxState.logicalScissor;
    for(u32 tile=0;tile<columns*rows;++tile) {
        if(!(tiles&(1u<<tile)))continue;
        const int left=static_cast<int>(tile%columns)*tile_width;
        const int top=static_cast<int>(tile/columns)*tile_height;
        const float x=2.f*(float(left)+float(tile_width)*.5f)/640.f-1.f;
        const float y=1.f-2.f*(float(top)+float(tile_height)*.24f)/480.f-source_center_y*scale;
        for(unsigned i=0;i<4;++i) {
            projection[0][i]=scale*original[0][i]+x*original[3][i];
            projection[1][i]=scale*original[1][i]+y*original[3][i];
        }
        std::memcpy(&g_gxState.proj,projection,sizeof(projection));
        g_gxState.stateDirty=true;
        set_logical_scissor({left,top,tile_width,tile_height});
        draw();
    }
    std::memcpy(&g_gxState.proj,original,sizeof(original));
    g_gxState.stateDirty=true;
    set_logical_scissor(scissor);
}
