/* SPDX-License-Identifier: MIT */
#ifndef GCN_CPU_OVERLAY_BINDING_H
#define GCN_CPU_OVERLAY_BINDING_H
#include "cpu/overlay_module.h"

/* Resolve one content-verified body without executing it. A caller may cache
 * the result, including NULL, only while it retires that cached decision for
 * every covered execution alias before the existing invalidation/reset path.
 * This does not authorize a physical/cached/uncached namespace substitution. */
GcnOverlayFn gcn_overlay_module_bind(GcnOverlayModule*,CPUState*,u32 address);

/* Same accounting and original CPU/PC contract as the generic call API. */
bool gcn_overlay_binding_call(GcnOverlayModule*,CPUState*,GcnOverlayFn body);
#endif
