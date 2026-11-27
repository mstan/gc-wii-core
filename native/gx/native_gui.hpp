// SPDX-License-Identifier: MIT
#pragma once
#include "native_scene.hpp"
#include <gcnllr/native_gui.h>
namespace gcnllr {
void bind_gui_texture(gxruntime::gxcore::DrawPlan&,unsigned,const GcWiiNativeTexture&,const float (*)[3]);
gxruntime::gxcore::DrawPlan* prepare_gui(NativeScene&,const GcWiiNativeGuiDraw&);
gxruntime::gxcore::DrawPlan* prepare_gui_layers(NativeScene&,const GcWiiNativeGuiLayersDraw&);
gxruntime::gxcore::DrawPlan* prepare_gui_mask(NativeScene&,const GcWiiNativeGuiMaskDraw&);
}
