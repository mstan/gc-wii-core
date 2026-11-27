// SPDX-License-Identifier: MIT
#pragma once
#include "native_gui.hpp"
#include <gcnllr/native_gui_program.h>
namespace gcnllr {
gxruntime::gxcore::DrawPlan* prepare_gui_program(NativeScene&,const GcWiiNativeGuiProgramDraw&);
}
