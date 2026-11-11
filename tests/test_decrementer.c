/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include "cpu/timing.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static unsigned observed,changed;
static void observe(CPUState* cpu,void* user,bool mutation) {
    (void)user;
    if(mutation)++changed;
    else {++observed;cpu->timebase=cpu->cycles/12;}
}

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x80001234u;
    cpu.msr = 0x00008000u; /* MSR[EE] */

    ppc_mtspr(&cpu, 22u, 10u, cpu.pc);
    ppc_decrementer_tick(&cpu, 9u);
    CHECK(cpu.spr[22] == 1u);

    ppc_decrementer_tick(&cpu, 2u);
    CHECK(cpu.spr[22] == 0xFFFFFFFFu);
    ppc_deliver_decrementer(&cpu);
    CHECK(cpu.exception == PPC_EXC_DECREMENTER);
    CHECK(cpu.pc == PPC_VECTOR_DECREMENTER);
    CHECK(cpu.srr0 == 0x80001234u);
    CHECK(!(cpu.msr & 0x00008000u));

    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x80005678u;
    ppc_mtspr(&cpu, 22u, 0u, cpu.pc);
    ppc_decrementer_tick(&cpu, 1u);
    ppc_deliver_decrementer(&cpu);
    CHECK(cpu.pc == 0x80005678u);
    cpu.msr = 0x00008000u;
    ppc_deliver_decrementer(&cpu);
    CHECK(cpu.pc == PPC_VECTOR_DECREMENTER);
    CHECK(cpu.srr0 == 0x80005678u);

    ppc_mtspr(&cpu, 22u, 0u, cpu.pc);
    ppc_decrementer_tick(&cpu, 1u);
    ppc_mtspr(&cpu, 22u, 123u, cpu.pc);
    CHECK(cpu.spr[22] == 123u);

    ppc_timing_set_observer(&cpu,observe,NULL);
    cpu.cycles=120;
    CHECK(ppc_mftb(&cpu,268,cpu.pc)==10 && observed==1);
    ppc_mfspr(&cpu,8,cpu.pc);CHECK(observed==1);
    ppc_mfspr(&cpu,22,cpu.pc);CHECK(observed==2);
    ppc_mtspr(&cpu,22,12,cpu.pc);CHECK(observed==3 && changed==1);
    ppc_mtspr(&cpu,284,42,cpu.pc);CHECK(cpu.timebase==42 && observed==4 && changed==2);
    ppc_decrementer_reset(&cpu);
    ppc_mftb(&cpu,268,cpu.pc);CHECK(observed==4);
    puts("decrementer timing: PASS");
    return 0;
}
