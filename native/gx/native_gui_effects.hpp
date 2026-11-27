// SPDX-License-Identifier: MIT
#pragma once
#include "native_gui.hpp"
#include <gcnllr/native_gui_effects.h>
namespace gcnllr {
gxruntime::gxcore::DrawPlan* prepare_gui_cutout(NativeScene&,const GcWiiNativeGuiCutoutDraw&);
gxruntime::gxcore::DrawPlan* prepare_gui_add(NativeScene&,const GcWiiNativeGuiAddDraw&);
gxruntime::gxcore::DrawPlan* prepare_gui_product(NativeScene&,const GcWiiNativeGuiProductDraw&);
}
