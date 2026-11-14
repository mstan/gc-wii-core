/* SPDX-License-Identifier: MIT */
#ifndef GCN_CPU_OVERLAY_BINDING_H
#define GCN_CPU_OVERLAY_BINDING_H
#include "cpu/overlay_module.h"

/* Resolve one content-verified body without executing it. A caller may cache
 * the result, including NULL, only while it retires that cached decision for
 * every covered execution alias before the existing invalidation/reset path.
 * This does not authorize a physical/cached/uncached namespace substitution. */
GcnOverlayFn gcn_overlay_module_bind(GcnOverlayModule*,CPUState*,u32 address);

/* Read-only ownership query for a complete MEM1 byte range. True excludes
 * every catalog namespace, including pages whose content is not yet bound.
 * Missing/invalid metadata is unknown ownership and returns false. This does
 * not check other native observers or host memory guards. */
bool gcn_overlay_module_unowned_range(const GcnOverlayModule*,u32 address,u32 size);

/* Same accounting and original CPU/PC contract as the generic call API. */
bool gcn_overlay_binding_call(GcnOverlayModule*,CPUState*,GcnOverlayFn body);
#endif
