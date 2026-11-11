/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runtime-only Gekko timing state. Keep this out of cpu/cpu.h: every generated
 * AOT shard includes that ABI header, so device-timing changes must not
 * invalidate the entire title module.
 */
#ifndef GCN_CPU_TIMING_H
#define GCN_CPU_TIMING_H

#include "cpu/cpu.h"

#define PPC_EXC_DECREMENTER    0x00000080u
#define PPC_VECTOR_DECREMENTER 0x00900u

void ppc_decrementer_reset(CPUState* cpu);
void ppc_decrementer_tick(CPUState* cpu, u32 tb_ticks);
void ppc_deliver_decrementer(CPUState* cpu);

/* Runtime-owned observation seam. CPU helpers may bring TB/DEC to the
 * published instruction cycle without changing the generated CPU ABI.
 * Reset clears the observer. One machine owns a runtime instance. */
typedef void (*PpcTimeObserver)(CPUState*, void* user, bool changed);
void ppc_timing_set_observer(CPUState*, PpcTimeObserver, void* user);
void ppc_timing_observe(CPUState*);
void ppc_timing_changed(CPUState*);

/* A machine may admit pure RAM polling only while no asynchronous writer can
 * change that range before the already armed CPU deadline. No CPU ABI change. */
typedef bool (*PpcIdleAdmission)(CPUState*,u32 address,void* user);
void ppc_idle_set_admission(CPUState*,PpcIdleAdmission,void* user);
bool ppc_idle_can_skip(CPUState*,u32 address);
void ppc_idle_note_cycles(u64 cycles);

#endif
